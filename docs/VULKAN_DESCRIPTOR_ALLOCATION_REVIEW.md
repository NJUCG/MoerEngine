# Descriptor buffer 按需分配：审查与前后对比

本文保留 2026-10-23 对逐 bind 分配路径的历史审查与测量。后续临时描述符已改为[提交前预留与录制游标](VULKAN_DESCRIPTOR_RESERVATION.md)，移除了逐 bind allocator 锁和临时提交的 64 KiB 取整，并将容量不足提前为可恢复拒绝。下文的“当前／新版”均指各节注明的审查版本，不代表现行录制路径；独立 A/B 工具仍测试兼容的 `ReserveRange` 接口。

审查日期：2026-10-23。结论：**容量共享和异常路径有改进，但不能称为全面的性能或健壮性提升。分配快路径变慢，且存在旧版成功、新版因块尾浪费而失败的请求序列。** 初次审查版本的常规集成测试通过；尚未测量整帧性能 A/B。

后续已完成 [VMA Virtual Allocator 替换实验](VULKAN_DESCRIPTOR_VMA_EXPERIMENT.md)：逐请求分配改善了两组容量反例，但明显增加 CPU 开销，因此没有切换生产实现。该实验还确认，下文 CPU OOM 可恢复的证据应限定在已测 Release 配置；MSVC Debug 的 written-ranges 快照会因 noexcept vector 构造中的 iterator-proxy 分配失败而终止。

## 历史代码复核：`d4b5ecb8`（2026-10-23）

两个问题仍然存在。`VulkanDescriptorBufferAllocator.cpp` 相比初次审查的 `7b16bffe` 没有任何变化；调用方 `VulkanCommandList.cpp:437` 仍无条件调用分配器，包括 0 字节申请。池容量仍为 6 MiB，默认块大小仍为 64 KiB。

新提交 `de85a707` 简化了缓存与 lease 句柄，并补充 shared-handle 移交测试；`a305485b` 将 bindless 索引表改为 push constant 中的 buffer device address。它们没有改变 allocation fast path 或块尾处理，不能视为这两项问题的修复，也不能据此推断完整引擎性能没有变化。

重新编译并运行当前生产 allocator 的 Release 微基准（40 对采样）：32 B 单线程中位耗时为旧侧 8.23 ns、当前 12.36 ns，配对耗时比约 1.50；8 线程为旧侧 40.24 ns、当前 66.46 ns，配对耗时比约 1.67。0 B 的 8 线程比约 2.96。多线程倍率受调度影响，先前的 2.55–3.06 倍不是固定常数；本次 2 线程 32 B 组合反而更快，因此结论仍限于单线程与高竞争快路径退步，不能泛化到所有负载。

两组容量反例均再次复现：六个 lease 各 `16 × 40 KiB` 后追加 32 KiB，以及各 `3632 × 288 B` 后追加 288 B，都是旧侧成功、当前失败。当前串行录制路径仍会将这种异常交给 `reject_serial_recording` 并 latch backend fault。

当前 allocator CTest（包含新加入的 `CheckSharedLeaseHandoff`）及 CPU OOM 探针均通过。本轮没有重跑完整引擎/GPU 矩阵；下文的 20/20 GPU 与编辑器冒烟结果属于初次审查版本，不能当作 `d4b5ecb8` 的 GPU 验证。

本次产物：`build/descriptor-recheck-raw.csv`、`build/descriptor-recheck-summary.json`、`build/descriptor-recheck-build.log`、`build/descriptor-recheck-contract-build.log`。统计脚本同时支持 Windows PowerShell 的 UTF-16 重定向和 PowerShell 7 的 UTF-8 输出。

## 范围与对照

| 提交 | 责任 | 审查结果 |
| --- | --- | --- |
| `08b04df5` | CPU 范围分配器、lease、checkpoint、回收 | 未发现重叠分配或回收丢失；发现容量利用率反例和分配开销退步 |
| `ec2f37ad` | 接入录制、提交、完成、flush、失败清理 | 正常与失败路径责任基本清晰；共享池耗尽仍会升级为 backend fault |
| `70af0596` | 并发回收和分配失败压力测试 | CPU 覆盖有效，但不能证明 GPU completion 生命周期，也没有测试 CPU `bad_alloc` |

运行版本为合并后的 `7b16bffe`。完整引擎对比应使用 **`486b3a76` → `7b16bffe`**：两者差异集中于这组三个提交，避免把另一条分支上的 binding layout 预计算等优化混入结论。`08b04df5^` 为原始开发基线，其分配游标算法与 `486b3a76` 相同。

本次 CPU 微基准直接编译当前生产 `VulkanDescriptorBufferAllocator.cpp`；旧侧复现 `486b3a76` 的原子游标函数，保留 acquire/acq_rel 内存序。**这不是两个版本的完整 VulkanHeap 或完整引擎 A/B。** 旧槽位获取、等待、Vulkan descriptor 写入、flush、GPU 执行都不在计时范围内。

## 从约束看设计

真实约束是：录制线程不能写重叠字节；GPU 使用结束前不能复用；非一致性内存必须正确 flush；不同 queue family 的访问要满足共享或所有权转移要求。固定三个槽、每槽 1 MiB、64 KiB 分配块都是设计选择。

| 条件 | 旧机制 | 新机制与代价 |
| --- | --- | --- |
| 同一提交的多个 recorder 不重叠 | atomic 游标 | lease mutex；每次绑定都竞争同一把锁 |
| 多个提交不重叠 | Graphics / Compute 各三个固定槽 | 全局 free ranges；取新块及回收时持全局锁 |
| GPU 结束前不能复用 | completion 回收槽 | 仍由 completion 回收 lease；没有新增独立完成时间线 |
| 放弃并行录制后重录 | worker join 后退回游标 | join 后恢复 checkpoint，释放新增块；原有前缀保留 |
| 非一致性内存不能跨 lease flush | 一整段槽内范围 | 块边界同时满足 descriptor alignment 和 memory atom 对齐，只 flush 各块写入部分 |
| 空闲槽不足时如何前进 | 等待该队列的槽，跨队列预算隔离 | lease 创建不等待；真正耗尽时返回失败；不再保证每队列独立预算 |

GPU buffer 的物理容量前后都是 **6 MiB**。新版本节省的是被在途提交预留但未使用的容量，不能解释为已经降低显存分配。

新增跨 queue-family `VK_SHARING_MODE_CONCURRENT` 与分块 flush 方向正确。规范要求 flush 按整个 `nonCoherentAtomSize` 单元参与同步；共享资源也必须满足相应 queue-family 规则。参见 [Vulkan memory](https://github.khronos.org/Vulkan-Site/spec/latest/chapters/memory.html)、[VkSharingMode](https://docs.vulkan.org/refpages/latest/refpages/source/VkSharingMode.html)。实际非一致性内存上的 GPU 验证仍需专门硬件/内存类型测试。

## 两个需要处理的退步

### P2：每次绑定都获取共享 mutex

位置：`VulkanDescriptorBufferAllocator.cpp:55–69`，调用方 `VulkanCommandList.cpp:415`。

包括 0 字节的 bindless-only 绑定，以及已有块尾部有空间的常见分配，都先取 lease mutex。旧侧只更新 atomic 游标。单线程已经有稳定退步，八线程同 lease 压力下更明显。

Release 微基准三次独立运行，每个组合预热 3 对、采样 40 对，固定种子随机交换旧/新顺序。下表是三次运行的配对中位耗时比范围，`1` 表示相同，越大越慢：

| 请求字节数 | 线程数 | 当前 / 旧版 |
| --- | ---: | ---: |
| 0 | 1 | 1.40–1.43 |
| 32 | 1 | 1.49–1.51 |
| 256 | 1 | 1.43–1.46 |
| 0 | 8 | 4.06–4.98 |
| 32 | 8 | 2.55–3.06 |
| 256 | 8 | 2.23–2.73 |

32 字节单线程中位耗时约从 **8.4 ns 增至 12.5–12.7 ns**。2/4 线程结果随调度变化，部分组合接近相同，不能用“所有线程数都更慢”概括。

此测试连续分配、没有中间 descriptor 写入和录制工作，竞争强于普通场景；纳秒结果还包含共同的 barrier/调度摊销。因此它足以否定“分配快路径已优化”，但不能推导 FPS 或整帧录制时间的变化。

建议先让不需要 descriptor 空间的绑定绕开分配路径，并用实际录制耗时决定是否调整非零分配快路径。若改为 recorder 独占子范围，必须由子范围的独占所有权保证不重叠，块生命周期仍属于 submission lease；不能简单移除 mutex，也不应为这几个纳秒先引入新的完成或回收系统。

### P2：块尾空间被搁置，旧版可成功的请求也可能失败

位置：`VulkanDescriptorBufferAllocator.cpp:64–80`。

分配只使用最后一个块；放不下就申请完整的新块。此前各块的尾部不再参与分配，也不跨相邻已归本 lease 所有的块连续使用空间。

可复现序列一：保持六个 lease 存活，每个分配 `16 × 40 KiB`：

- 旧版：每槽使用 640 KiB，六槽合计 3.75 MiB。
- 新版：每个 40 KiB 请求单占一个 64 KiB 块；96 个块全被预留，但有效使用仍仅 3.75 MiB。
- 此时第一个 lease 再要 32 KiB：**旧版成功，新版失败**。它不是 GPU 总容量不足，而是当前分块策略导致的内部浪费。

这不仅发生于超大布局：每个 lease 分配 `3632 × 288 B`，六个 lease 共使用 **6,276,096 B**；每块搁置 160 B。再申请 288 B 也是旧版成功、新版失败。这个小布局例子发生在接近满载时，不能描述为普通轻载就必然触发。

失败经 `BindShaderArguments` 抛异常；串行路径的 `reject_serial_recording` 会 latch backend fault。并行路径可以先回退串行，但没有自动消除这个容量问题。因此代价是可用性退步，而不只是多分配一点内存。

建议把这两个反例变成容量验收条件，再决定是否允许邻接块合并、降低尾部浪费或明确支持的容量预算。若仅选择接受碎片，就应把 `used_bytes` 与 `reserved_bytes` 一同暴露到诊断中，明确这是容量换实现简单性的权衡。

## 已确认的收益和健壮性

| 场景 | 旧版 | 新版 |
| --- | ---: | ---: |
| 空提交的 descriptor 预留 | 1 MiB | 0 |
| 只需 32 B 的提交预留 | 1 MiB | 64 KiB（减少 93.75%） |
| 空池中单提交申请 2 MiB | 失败 | 成功 |
| 仅考虑池容量的小 lease 数 | 6 个固定槽（每队列 3 个） | 96 个非空单块 lease |

最后一行只比较 descriptor allocator 的容纳能力；并不意味着执行器真的会同时调度 96 个提交。旧侧超出槽数会等待，不应标记成分配错误。

新增 `OnScopeExit` 覆盖尚未移交给提交对象的 lease。成功移交后本地 shared_ptr 为空，回收责任转到 completion；等待失败时保留 descriptor 占用直到 heap teardown，避免把“请求结束”误当作“GPU 已经停止读取”。CPU 测试和代码检查未发现重复回收或丢失空闲范围。

额外 `bad_alloc` 探针验证了原测试没有覆盖的 CPU OOM：创建 lease、首次分配、扩展块列表、生成 written-ranges 快照均可注入失败；失败不改变原有占用，随后可恢复全容量；checkpoint 回滚和 release 在禁止 CPU 分配时仍能完成。这是明确的异常安全收益证据。

## 实测范围、环境与产物

- 主机：AMD Ryzen 9 9950X，16 核 / 32 线程；NVIDIA RTX 4080 SUPER。
- 仓库现有 build 实际为 Visual Studio 2022 / MSVC（不是指南默认的 Ninja / Clang），本次沿用现有配置。完整引擎为 Debug，独立微基准为 Release、无 LTO，两侧相同编译选项。
- `cmake --build build --parallel 30` 成功。
- 8 项相关 CTest 全通过：allocator、ExternalCpuJoin、GPU completion、record diagnostics、parallel record、recording handoff、submission dispatch、wave scheduler。
- Vulkan runner **20/20 PASS**，启用 strict GPU gate：真实 NVIDIA GPU、Vulkan 1.3+、validation 开启、不允许用 skip 当成功。覆盖串行、并行、worker fallback、翻译失败、部分提交、readback、query 等现有场景。
- 沿用根配置 `Raytracing + Sponza` 启动编辑器，记录到首帧提交并存活 60 秒；输出日志未检出 error、VUID、device-lost 或 descriptor allocation failure，随后主动结束测试进程。此项是启动冒烟，不是画面人工验收或正常退出测试。
- CPU 容量对比、三轮微基准、真实 `bad_alloc` 注入均执行。容量报告中的失败是本次发现的行为，程序退出 0 表示完成观察，不表示不存在容量退步。
- 尚未做完整引擎旧版/新版 FPS A/B，也未执行真实 GPU 上的 descriptor 池耗尽与延迟 completion 专项。现有 20 项测试不能替代这些测试。

源码：`tools/descriptor_allocator_ab/`。本机原始证据：

- `build/descriptor-review-build.log`、`build/descriptor-review-ctest.log`
- `build/descriptor-review-gpu/summary.json` 及同目录逐项日志
- `build/descriptor-ab-raw.csv`、`descriptor-ab-raw-2.csv`、`descriptor-ab-raw-3.csv`
- `build/descriptor-ab-summary.json`、`descriptor-ab-summary-2.json`、`descriptor-ab-summary-3.json`
- `build/descriptor-ab-oom.log`
- `build/descriptor-review-editor.stdout.log`、`build/descriptor-review-editor.stderr.log`

上述 build 文件为本机生成产物，不是已提交的长期报告数据；本节及前面的表格记录本次结果，以下命令可重新生成。

## 复现命令

从仓库根目录运行；独立工程不会更改生产 allocator：

```powershell
cmake -S tools/descriptor_allocator_ab -B build/descriptor-allocator-ab
cmake --build build/descriptor-allocator-ab --config Release
& build/descriptor-allocator-ab/Release/DescriptorAllocatorFailureProbe.exe
& build/descriptor-allocator-ab/Release/DescriptorAllocatorAB.exe > build/descriptor-ab-raw.csv
python tools/descriptor_allocator_ab/summarize.py build/descriptor-ab-raw.csv build/descriptor-ab-summary.json
```

Ninja 单配置构建需配置 `-DCMAKE_BUILD_TYPE=Release`，可执行文件直接位于构建目录。统计脚本输出中位数、P95、40 对样本的中位耗时比及 bootstrap 95% 区间；独立运行之间的变化仍需单独比较，不把单轮置信区间当成跨机器保证。

```powershell
ctest --test-dir build -C Debug -R 'VulkanDescriptorBufferAllocator|RHIParallelRecordContract|ExternalCpuJoin|RHIGpuCompletion|RHIExecutorRecording|VulkanSubmissionRequest|VulkanTranslateWave|RHIRecordDiagnostics' --output-on-failure
python tools/threading/run_parallel_record_vulkan_test.py --executable target/bin/Debug/TestRHIParallelRecordVulkan.exe --outdir build/descriptor-review-gpu --strict-gpu-gate
```

## 后续完整 A/B 验收设计（本次未执行部分）

用独立构建目录分别构建 `486b3a76` 和 `7b16bffe`，同编译器、同 Release 选项、同配置和资源缓存。性能测试关闭 validation；正确性矩阵开启 validation。不要在测试过程中覆盖现有工作区的运行库。

| 场景 | 需要验证的条件 | 必须记录 |
| --- | --- | --- |
| 空绑定、32/256/288 B 普通布局，串行及 2/4/8 worker | 快路径变化是否影响实际录制 | bind 次数、record wall、worker join、allocator 时间、CPU 帧 P50/P95/P99 |
| 32 KiB 小提交与单提交 >1 MiB | 小提交节省预留；突破旧单槽上限后 GPU 输出正确 | used/reserved 峰值、分配失败、每提交独立 payload 的 GPU readback |
| 六个在途 lease 加块尾反例 | 碎片是否让旧版成功负载失败 | 首个失败请求、各 lease used/reserved、故障状态 |
| G/C 混合，部分旧提交等待外部 timeline gate | 未完成 payload 不被复用；另一队列容量竞争结果明确 | completion 前后的 GPU readback、占用、callback 恰好一次、超时 |
| worker 写入 descriptor 后失败并串行 fallback | join 后才能 rewind；前缀保留，废弃字节无泄漏 | serial golden/readback 一致、回滚前后占用、全部退休后满容量恢复 |
| 实际 ReserveRange 耗尽、flush 错误、submit 拒绝 | 失败不提交坏命令，已接受的前缀仍正确退休 | request outcome、accepted submit、GPU completion、资源回收分别计数 |
| 成功提交后注入 HostWait 失败 | 请求失败不释放 GPU 可能仍读取的块 | 不出现提前地址复用，fault 后不再分配，退出不死锁 |
| 非一致性内存 + G/C 不同 family | atom 级 flush 隔离及 concurrent sharing | memory type、atom size、family index、validation、readback |
| 固定相机 Sponza，Raster 与 Raytracing 分开 | 局部节省是否抵消锁和多段 flush 开销 | 预热后至少 1000 帧，ABBA 顺序，至少 5 对，CPU/GPU 帧时间与峰值占用 |

延迟完成测试应由 CPU 可释放的 timeline gate 驱动，不能依赖需要先申请 descriptor 的“解锁提交”，否则测试自身会制造循环等待。大于 6 MiB 的请求两边都应有有界失败结果；旧槽获取的等待与新分配失败必须分别报告。

验收：相同负载先通过 GPU 输出及生命周期检查，再讨论性能；小提交预留降低与单槽上限突破可以分别算收益。只有完整 A/B 的帧时间或录制时间改善超过测量波动时，才宣称实际渲染性能提升。已复现的容量退步必须修复或被明确接受为设计边界。
