# 描述符预留：验证与对比

设计及责任边界见 [Vulkan 临时描述符](../../docs/VULKAN_DESCRIPTOR_RESERVATION.md)。

## 正确性

```powershell
cmake --build build --config Debug --target MoerEditor TestVulkanDescriptorBufferAllocator TestVulkanDescriptorBackend TestRHIParallelRecordVulkan --parallel 12
ctest --test-dir build -C Debug -R '^(VulkanDescriptorBufferAllocatorContract|VulkanDescriptorPolicyContract|ShaderConstantsContract)$' --output-on-failure
python tools/descriptor_backends/run_matrix.py --validation-layer-dir build/descriptor-validation/layer --output build/descriptor-reservation-backends
```

新增 CPU 用例验证：跨空洞预留、单次 bind 不跨段、游标耗尽、失败不推进游标、整批失败不泄漏空间、flush atom 隔离、独立游标并发、重录复用、4,000 步随机分配/乱序回收，以及旧新方式的容量差异。旧的逐次分配及并发回收用例继续运行。

容量对照使用同一个 64 KiB 池和 32 B 请求：旧路径首次请求保留整个 64 KiB，只容纳一个 lease；新路径按 256 B atom 隔离，可容纳 256 个 lease。这是小提交尾部浪费的受控对照，不代表一般场景获得 256 倍容量。

GPU 压力与失败回退使用真实 shader 回读：

```powershell
$env:VK_LAYER_PATH = (Resolve-Path build/descriptor-validation/layer).Path
$env:DISABLE_VULKAN_OBS_CAPTURE = '1'
$env:MOER_VULKAN_DESCRIPTORS = 'buffer' # 同样执行 heap
$env:MOER_VULKAN_PUSH_DESCRIPTORS = '0'
Push-Location target/bin/Debug
try {
    ./TestVulkanDescriptorBackend.exe --parallel --reservation-pressure
    ./TestVulkanDescriptorBackend.exe --parallel --inject-worker-failure
    ./TestRHIParallelRecordVulkan.exe --parallel --inject-worker-failure
} finally { Pop-Location }
```

压力测试先用独立 lease 占满池，再提交包含“污染输入的 upload + bindless dispatch”的事务。检查完成回调一次、成功回调零次、signal fence 为 Rejected 而非 Failed；释放占位 lease 后的完整回读仍须看到未污染的输入。测试会产生预期的容量拒绝及依赖拒绝日志。Worker 注入测试须在日志中出现 `serial-fallback-worker-failure`，并通过完整回读，防止仅开启选项却未走并行路径。

2026-10-24 本机 Debug、RTX 4080 SUPER、Khronos validation layer 下：

- 3 项 CPU/shader CTest 通过。
- 16 项后端矩阵全部通过，验证错误为零。
- heap/buffer 的容量耗尽及 worker 回退共 4 项通过，未出现 Vulkan validation 错误。
- buffer 模式的 RHI 并行集成测试通过，覆盖乱序录制、跨队列、提交拒绝和退休回收。
- Raster 和 Raytracing 均在就绪后继续运行至少 65 秒，正常退出；未发现验证错误。Raytracing 的通用矩阵脚本因缺少旧的 `[ThreadingProfile][RHI]`、`[ThreadingProfile][RHIRecord]` 两个日志标记而报告 FAIL；其进程退出码为 0、截图检查通过、forbidden-log 匹配为空。Raster 使用直接 runtime verifier，报告 PASS。

证据保存在 `build/descriptor-reservation-backends/`、`build/descriptor-reservation-editor/`、`build/descriptor-reservation-pressure-{heap,buffer}.log`、`build/descriptor-reservation-fallback-{heap,buffer}.log` 和 `build/descriptor-reservation-rhi-integration.log`。

编辑器和上述测试目标构建成功。额外执行全量 `cmake --build build` 时，两个未修改的测试构建路径仍有问题：`TestRHICmdReorderer` 缺少 `BufferView(Buffer*, EPixelFormat)` 的 DLL 导出，`TestVulkanTemporaryBufferAllocator` 的 Windows `max` 宏与 `numeric_limits::max()` 冲突。没有为本次描述符改动扩展修复范围。另一次 work-estimator 目标的 `rc.exe` 链接失败，在单独重建后通过。全量构建日志为 `build/descriptor-reservation-full-build.log`，不能把本次相关目标通过解释成全仓库构建通过。

## CPU 分配微基准

```powershell
cmake -S tools/descriptor_reservation -B build/descriptor-reservation-benchmark
cmake --build build/descriptor-reservation-benchmark --config Release
./build/descriptor-reservation-benchmark/Release/DescriptorReservationBenchmark.exe
```

同一个 allocator 比较兼容的逐 bind `ReserveRange` 路径和 `PrepareRecordings + Cursor` 路径。256 条命令、每次绑定 128 B、500 批提交，使用预先创建的线程；两边校验相同 offset 总和。总时间包括 lease 创建、预留、两次线程 rendezvous、录制分配和回收。只测 CPU 存储管理，不含 Vulkan 调用、descriptor 写入、队列中的 pipeline 需求遍历和命令查找表构造，因此不能代替整帧性能测试。

本机 MSVC Release，连续 5 次运行的中位数；单位为 500 批的累计毫秒：

| Workers | 每命令 binds | 旧总时间 | 新总时间 | 旧录制阶段 | 新录制阶段 |
|---:|---:|---:|---:|---:|---:|
| 1 | 4 | 12.829 | 16.384 | 12.622 | 10.460 |
| 1 | 64 | 107.213 | 65.612 | 106.631 | 47.528 |
| 4 | 4 | 20.696 | 20.531 | 20.411 | 13.271 |
| 4 | 64 | 288.290 | 77.432 | 287.647 | 56.683 |

绑定较少时，新增元数据成本可以抵消或超过录制阶段的节省；绑定密集的四线程用例中，总分配时间约为旧路径的 27%。这些结论限于本微基准。原始数据在 `build/descriptor-reservation-benchmark/repeat-*.jsonl`。
