# Descriptor buffer：复用 VMA Virtual Allocator 的实验

本文记录逐 bind 分配阶段的候选评估。后续生产临时路径采用[提交前预留与录制游标](VULKAN_DESCRIPTOR_RESERVATION.md)，没有接入 VMA；下文的“当前实现”指实验时的基线。独立工具保留对 `ReserveRange` 的对照，不测量新的提交计划与游标路径。

**结论：可以复用，但不建议把每次 descriptor 申请直接替换成一次 VMA 分配。** 本次候选消除了两个已知的 64 KiB 块尾容量反例，代价是分配、回收和跨提交并发开销明显增加；CPU 元数据 OOM 的保证也不能直接继承。候选保留在独立测试工具中，生产 allocator 没有切换。

对照代码：`d4b5ecb8`，日期：2026-10-23。使用仓库已有的 `3rdparty/VulkanMemoryAllocator/vk_mem_alloc.h`，没有引入新依赖。结果仅涉及 CPU 区间分配，不代表整帧 FPS 或 Vulkan/GPU 集成验证。

## 从约束推导接入边界

需要分配的是已有 6 MiB descriptor buffer 内的字节区间。真实约束有四个：并发写入不能重叠；descriptor offset 必须对齐；独立 flush 的提交不能错误共享 non-coherent memory atom；GPU 使用结束之前不能回收。

64 KiB 块和“只尝试最后一块的尾部”是当前实现的设计选择。为了隔离这个选择的影响，候选让每次请求直接申请对齐后的大小，使用 VMA 默认 TLSF 算法管理空闲区间。没有使用 linear 模式，因为多个提交的分配和完成可以交错。

| 条件 | 候选中的责任及代价 |
| --- | --- |
| 分配区间不重叠，释放后可复用 | VMA Virtual Allocator 管理区间；不分配新的 GPU buffer |
| 并发修改分配器状态安全 | 适配层全局 mutex 同时保护 virtual block 和 lease 记录；VmaVirtualBlock 自身不提供线程安全 |
| GPU 完成前不回收 | 仍由提交 completion 授权 `Release`；shared_ptr 引用计数不代表 GPU 完成 |
| 放弃录制后恢复 checkpoint | 调用者先 join/discard，适配层释放 checkpoint 后的 allocation handles；保留原前缀 |
| descriptor offset 对齐 | 向 VMA 明确传入 alignment；VMA 不会从虚拟区间推导 Vulkan 设备属性 |
| 独立提交不共享 flush atom | 非一致性实验中，每次申请的 offset 和 size 都按 atom 对齐；小请求会产生额外填充 |
| C++ 记录扩容失败不损失空间 | 在调用 VMA 前扩容 lease 记录；这不保证 VMA 内部 CPU 元数据 OOM 可恢复 |

每个 lease 保留 allocation handle，供乱序完成和回滚释放使用。适配层没有新增 completion 时间线，也没有让 CPU 引用析构自动授权 GPU 空间回收。它的析构清空 virtual block 只用于本次 CPU 实验结束。

## 容量：已知反例得到改善，但没有消除所有碎片

三方共用 6 MiB 容量。当前实现的 descriptor alignment 为 32 B，块大小为 64 KiB；VMA 分别测试 32 B 对齐和模拟的 256 B non-coherent atom 隔离。六个 lease 的请求交错进行。

| 请求序列 | 当前实现 | VMA，32 B 对齐 | VMA，每次请求隔离 256 B atom |
| --- | --- | --- | --- |
| 空 lease 申请 32 B，实际占用 | 65,536 B | 32 B | 256 B |
| 六个 lease 各申请 `16 × 40 KiB`，再追加 32 KiB | 失败；前缀占用 6 MiB | 成功；前缀占用 3.75 MiB | 成功；前缀占用 3.75 MiB |
| 六个 lease 各申请 `3632 × 288 B`，再追加 288 B | 失败；前缀使用 6,276,096 B，占用 6,291,456 B | 成功；前缀使用与占用均为 6,276,096 B | 前缀就耗尽：288 B 被扩为 512 B，使用 3,538,944 B 时占满池 |
| 先分六个 1 MiB 区间，释放交替的三个，再申请连续 2 MiB | 失败 | 失败 | 失败 |

最后一行保留了 3 MiB 空闲，但最大连续空洞只有 1 MiB。VMA 能管理和合并相邻空洞，不能合并被存活分配隔开的空间，也不会自动迁移本实验中的 live descriptors。

256 B atom 是测试输入，不是对本机实际内存类型或设备 atom 的测量。这一结果说明，“每个 bind 独立申请且独立隔离 atom”本身可能重新引入浪费。生产实现把隔离边界放在块上，同一 lease 的小请求可以共享一个 atom。

## 性能：逐请求调用的成本无法忽略

环境：AMD Ryzen 9 9950X，MSVC 19.44，Release，关闭 LTO。两侧均为独立翻译单元；当前侧直接编译生产 allocator。每组 3 次热身、40 对随机先后顺序样本，独立执行 3 轮。线程创建和 allocator/lease 创建不计入 Reserve 时间；分配期间的元数据扩容计入。snapshot 和 retirement 分别计时。计时期间不并行构建项目。

| 32 B 请求负载 | 当前 ns/请求，三轮中位数范围 | VMA ns/请求，三轮中位数范围 | VMA/当前配对耗时比，三轮范围 |
| --- | ---: | ---: | ---: |
| 1 线程，1 lease | 12.46–12.73 | 47.90–51.13 | 3.75–4.10 |
| 8 线程，共用 1 lease | 35.94–60.77 | 89.48–148.91 | 2.24–2.50 |
| 8 线程，各自独立 lease | 5.76–6.08 | 84.14–115.24 | 13.48–17.17 |

多线程的 ns/请求是总墙钟时间除以总请求数，表示吞吐成本，不是单个调用的延迟。配对耗时比是每对样本的比值中位数，不能直接用表中两端的范围相除。调度噪声明显，因此保留三轮范围，不把倍率当作常数。

当前实现通常只移动 lease 内游标，取得新块才访问共享池。候选每次都执行 TLSF 分配、保存 handle 并取得全局锁。因此，独立 lease 原本可以并行的快路径也被串行化了。候选的 0 B 请求虽然不调用 TLSF，但仍验证 lease 并取得同一把锁；本次没有混入调用方跳过 0 B 请求的优化。

对 16,384 个 32 B 请求，单 lease 回收成本折算约为当前 0.02 ns/原始请求、VMA 7.01–7.41 ns/原始请求。当前释放 8 个块，候选释放 16,384 个 allocation handles；这不是单次 free 只需 0.02 ns 的含义。

written-ranges 也受分配粒度影响：单 lease 的相邻范围可合并为 1 段；8 个独立 lease 交错时，三轮候选的范围数量中位数分别为 406.5、747.5、650.5，当前均为 8。这是 CPU 快照的范围数，没有实际调用 Vulkan flush；不能把它直接换算为 GPU 开销。

## 健壮性验证与明确限制

- Release：3 个 CTest 通过，包含两侧的逻辑契约以及两侧的 C++ OOM 注入探针。
- Debug：开启 VMA 内部 assertions 的逻辑契约测试通过。
- 逻辑契约覆盖零请求、大小溢出、容量耗尽后状态不变、foreign/retired lease 拒绝、shared handle 移交、checkpoint 回滚保留前缀及其他 lease、乱序回收、20,000 步随机区间所有权校验、8 线程共享 lease 分配、8 线程各 300 轮独立 lease 分配/回收、256 B atom 隔离以及最终全容量恢复。
- OOM 注入覆盖创建 lease、首次记录和记录扩容、written-ranges 快照；Release 下失败后占用不变，随后可以回滚、释放并重新申请整个池。
- **VMA 内部 CPU 元数据 OOM 没有得到恢复保证。** 探针覆盖的是适配层 `operator new`，不覆盖 VMA 内部 malloc/aligned_malloc。仓库 VMA 文档明确说明不优雅处理 CPU 分配失败，通常仅 assert（`vk_mem_alloc.h` 的 `general_considerations_features_not_supported`）。虚拟容量耗尽返回失败和 CPU 内存耗尽是不同条件。
- **MSVC Debug 的快照 OOM 不能宣称可恢复。** 实测当前实现和候选均在 `GetWrittenRanges` 期间 terminate。MSVC 14.44 的 `std::vector` 默认构造函数为 noexcept，同时分配 Debug iterator proxy；全局 OOM 注入会在这里终止。故 MSVC 的恢复探针仅注册 Release/RelWithDebInfo/MinSizeRel 配置，Debug 运行逻辑契约测试。保留了终止阶段诊断及独立失败日志，没有修改生产实现来掩盖这一结果。
- 没有替换生产路径，没有执行候选的 Vulkan flush、GPU completion、设备故障或整帧性能测试；CPU 的“授权释放”测试不能证明真实 GPU 已完成。

## 对后续设计的判断

这次容量改善来自**取消每个请求背后的固定块尾浪费**；不是来自去掉锁，实验实际上使用了更粗的锁。VMA 适合承担现成的区间管理职责，但不意味着应该把通用分配器放在每次 bind 的热路径上。

如果继续采用 VMA，值得验证的边界是：按现有录制任务的真实需求申请一段空间，在该段内仅使用游标，完成后按原提交生命周期回收。只有先确定该任务需要多少 descriptor 字节，才能同时减少全局调用次数和固定大块预留浪费；若仍按固定 64 KiB 粗分配，这次两个容量反例不会因此自动消失。

当前每次绑定的字节数来自 `binding_template.resource_descriptor_allocation_size`，但这不等于现有调度器已经提供了整个任务的精确总量。是否可以在现有命令遍历阶段顺带求和、串行路径和 fallback 如何保持一致，是下一步需要核实的具体条件。不能为了规避本实验的开销，未经验证再引入第二套生命周期、任务调度或失败重试状态。

当前决策是保留这个可复现实验，不将逐 bind 的 VMA 候选替换进生产代码。

## 复现与产物

```powershell
cmake -S tools/descriptor_allocator_ab -B build/descriptor-allocator-ab
cmake --build build/descriptor-allocator-ab --config Release --parallel 8
ctest --test-dir build/descriptor-allocator-ab -C Release --output-on-failure
cmake --build build/descriptor-allocator-ab --config Debug --parallel 8
ctest --test-dir build/descriptor-allocator-ab -C Debug --output-on-failure
& build/descriptor-allocator-ab/Release/DescriptorAllocatorVmaExperiment.exe --test
& build/descriptor-allocator-ab/Release/DescriptorAllocatorVmaExperiment.exe --bench > build/descriptor-vma-raw-1.csv
python tools/descriptor_allocator_ab/summarize_vma.py build/descriptor-vma-raw-1.csv build/descriptor-vma-summary-1.json
```

`--bench` 与 `--test` 分开，避免契约测试输出混入 CSV。独立计时三轮；汇总脚本检查完整的 21 组负载、每组 40 对样本，输出中位数、P95、配对比值及 bootstrap 95% 区间。Ninja 单配置构建需设置 `-DCMAKE_BUILD_TYPE=Release`，可执行文件位于构建目录根部。

源码位于 `tools/descriptor_allocator_ab/vma_candidate.{h,cpp}`、`vma_experiment.cpp`、`failure.cpp`、`summarize_vma.py`。本机原始结果位于 `build/descriptor-vma-raw-{1,2,3}.csv`、`build/descriptor-vma-summary-{1,2,3}.json`、`build/descriptor-vma-contracts.log`、`build/descriptor-vma-wrapper-oom.log` 和 `build/descriptor-vma-debug-oom-observation.log`。这些 build 产物是本地证据；上表记录了可保留的结果。
