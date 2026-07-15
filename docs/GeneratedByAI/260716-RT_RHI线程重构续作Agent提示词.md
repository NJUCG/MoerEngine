# MoerEngine RT/RHI 线程重构续作 Agent 提示词

> 本文件整体就是交给后续 Agent 的中文提示词和状态快照。
>
> 当前功能开发冻结在 **Phase 5 完成点**。在原电脑上，除了补充本文件并推送分支，不再继续实现下一阶段功能。
>
> 换机后，请让 Agent 先完整阅读本文件、仓库根目录 `AGENTS.md` 和 `tools/threading/README.md`，再执行“换机接管流程”。不要只截取“下一步”一节，否则容易丢失已经建立的线程与资源所有权约束。

---

## 1. 给接手 Agent 的首要指令

你现在接手的是 **MoerEngine Render Thread / RHI Thread 整体重构**，不是一个孤立的 Vulkan bug 修复任务。

当前必须遵守以下指令：

1. 使用中文沟通和记录结论。
2. 当前功能冻结点为 `f55c016e`。该提交之后允许存在交接文档提交，但不应存在未经记录的新功能实现。
3. 第一个动作必须是核对分支身份、工作区、配置、构建和自动验证基线。不要一上来修改代码。
4. 如果用户只让你“查看状态”或“接手”，先汇报已完成目标、未完成目标和下一阶段计划，等待用户明确授权后再开发。
5. 如果用户同时明确说“继续实现”，也要先完成换机基线复现；基线失败时先定位环境或回归原因，不得把失败状态直接带入下一阶段。
6. 每个阶段都执行小步 `编辑 -> 构建 -> 运行 -> 多配置观察 -> 日志扫描 -> 阶段提交`，并在阶段边界停止汇报。
7. 用户特别要求阶段性成果分阶段提交。只暂存本阶段自己的文件，不得夹带用户的其他工作区变更。
8. 不得强推、重写当前分支历史或随意 rebase。若未来 `main` 已前进，先完成当前分支基线，再单独评估合并策略。
9. 不得为了“消除报错”删除断言、忽略 Vulkan 返回值或放宽验证脚本。必须分析首个失效点。
10. 视觉判断要区分“窗口未展开”和“渲染错误”。此前用户已经确认过一次截图只是窗口尺寸不同，画面渲染本身正确。

---

## 2. 仓库与分支身份

### 2.1 固定信息

```text
Repository: https://github.com/NJUCG/MoerEngine.git
Branch: feature/rt-rhi-threading
Base branch: main
Base commit on 2026-07-16: d016d1d8f5f96a8909bc8bfdaa6468e9810ad6a5
Functional freeze commit: f55c016e
```

`main..f55c016e` 一共有 18 个功能/验证提交。交接文档提交位于 `f55c016e` 之后，因此判断功能冻结点时要看 `f55c016e`，判断最新文档时看分支 HEAD。

### 2.2 原电脑未纳入分支的本地内容

原电脑在交接时存在以下未提交内容：

```text
M  CMakeLists.txt
?? .codex_tmp/
?? rhi_docs/
```

这些内容不是本次 RT/RHI 重构提交的一部分：

- `CMakeLists.txt` 是用户原有修改，未被 Agent 暂存或回退。
- `.codex_tmp/` 是本地过程数据。
- `rhi_docs/` 是用户本地未跟踪文档集合。

换机后远端分支没有这些内容是正常现象。不要根据本文件重建或伪造它们，也不要把它们误判为“丢失的功能代码”。

### 2.3 可选外部 TechRecord

原电脑另有独立仓库：

```text
D:\Other_Files\TechRecord
```

其中 RT/RHI 实施记录位于：

```text
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/
```

Phase 5 文档提交为 `71b10ed`。换机后若没有这个独立仓库，不构成阻塞；本文件必须被视为分支内的自包含交接依据。

---

## 3. 整体重构目标

这次重构的长期目标是把原先主要由 Main/Game Thread 同步驱动的渲染循环，演进为边界明确、可回退、可验证的流水线：

```text
Game Thread
  逻辑场景 / ECS / 输入 / Editor UI 状态
  -> 值语义 FramePacket + SceneUpdateBatch + copied UI packet

Render Thread
  RT-owned RenderScene / GpuScene
  Raster 或 Raytracing 单帧渲染
  -> 自包含 CmdSubmit

RHI Thread
  command reorder / barrier preprocess / Vulkan command recording
  vkQueueSubmit2 / Present FIFO

GPU Completion Thread
  timeline wait / allocator 与 presentor 回收
  callback / deferred release / completion marker
```

TaskGraph worker 继续承担资源加载、纹理解码、ParallelFor 等通用工作，但不能成为一个没有数据所有权边界的“隐式 Render Thread”。

长期终态还包括 RenderGraph、并行命令录制和更多队列/backend 的线程化，但这些尚未实现，不能在状态汇报中写成已完成。

---

## 4. 已完成目标

### 4.1 Phase 0：线程与配置地基

状态：完成。

- 增加 `render_thread`、`rhi_thread`、`rhi_bypass`、`max_frame_lag` 等配置。
- 修复 TaskGraph/Event/RenderThread 相关地基问题。
- 接入 `RenderThreadService`，明确启动、入队、等待、flush 和停止语义。
- 保留完整同步 baseline，默认配置不启用新线程。

提交：

```text
873634e4 feat(threading): 建立 Render Thread 与 TaskGraph 地基
```

### 4.2 Phase 1 / 2A：Raster 同步 RT 与 FramePacket 边界

状态：完成。

- Raster pass 可在同步 Render Thread 上执行。
- GT 每帧捕获 Raster 配置、相机输入、窗口/UI composition 等值数据。
- `RasterFramePacket` 不借用调用栈和可变 Editor 状态。
- 相机等反馈通过窄化 feedback 返回 GT。

提交：

```text
97fa6dd3 feat(raster): 接入同步 RT 与 FramePacket 边界
```

### 4.3 Phase 2B.1：SceneUpdateBatch 与场景快照

状态：完成。

- GT 负责 Scene/ECS Tick。
- GT 生成 move-only `SceneUpdateBatch` 和值语义场景快照。
- Raster RT 不再直接读取 GT ECS registry 或在 RT 上调用 Scene Tick。

提交：

```text
69f5977a feat(scene): 引入 Render Thread 场景更新批次
abf611d7 fix(imgui): 修正多视口缩放帧缓冲尺寸
```

### 4.4 Phase 2B.2：RenderScene 与 RT-owned GpuScene

状态：完成。

- `Scene` 保持 GT-owned logical/CpuScene 数据。
- Renderer 持有 `RenderScene`，由实际消费帧的一侧创建、更新和销毁 `GpuScene`。
- 全量和增量 GPU scene update 通过 batch 进入 RT 所有权域。
- Raster pass 改为读取 RenderScene/GpuScene 资源，不直接跨线程读取 GT 场景。

提交：

```text
7f52537e feat(scene): 将 GpuScene 所有权迁移至 RenderScene
```

### 4.5 Phase 2B.3：Copied ImGui UI Draw Packet

状态：完成。

- GT 复制主视口和独立平台视口的 vertices、indices、clip rect、texture handle 和 draw offsets。
- RT 消费 `UiDrawFramePacket` 时不读取 ImGui 全局上下文。
- backend、平台 viewport 与 framebuffer GPU 生命周期固定到消费线程和 GPU 完成边界。

提交：

```text
ad26c194 feat(imgui): 引入可复制的 UI 绘制帧包
```

### 4.6 Phase 2B.4 / 2B.5：Raytracing FramePacket、场景快照与同步 RT

状态：完成。

- Raytracing 内部 while loop 上移到 Engine，统一为 `PrepareFrame -> RenderFrame -> ApplyFrameFeedback`。
- `RaytracingFramePacket` 携带场景更新、配置、相机、UI 和 snapshot。
- Raytracing pass 不再直接依赖 `Scene&`；需要的 light/primitive 数据改为值快照或稳定 bindless handle。
- Raytracing renderer 的创建、单帧消费、RenderScene/GpuScene 更新和析构可在同步 RT 执行。

提交：

```text
eb9260e0 feat(raytracing): 引入独立单帧帧包边界
0b7a5266 feat(raytracing): 引入场景帧快照并移除 Pass 场景依赖
b79126c9 feat(threading): 将 Raytracing 迁移至同步 Render Thread
```

### 4.7 Phase 3：One-frame-lag 与跨帧资源生命周期

状态：完成，验证范围为 `max_frame_lag=0/1`。

- 新增 `RenderFrameFence` 和 `BoundedRenderFrameQueue<Feedback>`。
- GT/RT 支持 lag 0 和 lag 1；超过上限时等待最老 frame fence。
- feedback 按 frame id FIFO 退休，不复用 shared latest 状态。
- reload、renderer switch 和 close 前强制 drain。
- 平台 viewport create/resize/destroy 与 UI GPU 资源生命周期串行化到 RT/GPU 边界。

提交：

```text
d23be8ba feat(threading): 引入有界单帧延迟调度
a2c31992 fix(imgui): 串行化平台视口资源生命周期
3ed3c74b refactor(imgui): 将 GPU 资源生命周期迁移至 RT
```

### 4.8 Phase 4：Vulkan Graphics RHI Thread 与提交 FIFO

状态：完成。

- `CmdSubmit` 中 upload bytes、descriptor arrays、pipeline metadata 和 scope name 均改为跨线程自包含数据。
- `rhi_thread && !rhi_bypass` 为 effective threaded mode。
- Vulkan graphics queue 的 reorder、barrier preprocess、command recording、submit 和 Present 进入同一个 RHI FIFO。
- timeline 在 enqueue 时预留；Execute、Present、空命令和 acquire 过渡分支都必须最终 signal。
- threaded Present 使用非阻塞 acquire，不能无限占住 submission worker。
- RHI submission worker 与 GPU completion worker 分离。
- `Sync()` 覆盖 RHI CPU work、GPU timeline、callback、allocator/presentor recycle 和 deferred release。

提交：

```text
3aca524e refactor(rhi): 使命令提交数据跨线程自包含
a8a0635a feat(threading): 将 Vulkan 提交迁移至 RHI FIFO
```

### 4.9 Phase 5：自动回归、性能基线与稳定性修复

状态：完成，是当前冻结点。

- 增加 Windows 自动验证器和九场景矩阵。
- 支持 `MoerEditor --config <path>`，每个场景使用独立 TOML，不修改根配置。
- 增加 RT/RHI 一秒稳态窗口与末尾窗口聚合。
- RHI 指标拆分为 Execute/Present caller、queue wait、backend work。
- 修复同一 `VkQueue` handle 上 submit/present/wait idle 缺少统一 host synchronization 的问题。
- 使用 Crash Diagnostic Layer 定位到 Raster Geometry Culling 的 GPU invalid read。
- 修复 descriptor ring 由 Execute/Present 共用 timeline 错误推进而提前复用槽位的问题。

提交：

```text
723d3cd3 test(threading): 固化 RT RHI 运行验证矩阵
500bc65b feat(threading): 增加 RT RHI 稳态性能指标
558f4055 fix(vulkan): 修复异步队列与描述符环并发
f55c016e feat(threading): 拆分 Execute 与 Present 性能指标
```

---

## 5. 当前不能破坏的架构不变量

后续改动必须显式检查以下不变量：

### 5.1 GT / RT 数据边界

- GT 拥有 Scene/ECS、输入、Editor 可变状态和逻辑帧推进。
- RT 不直接读取 GT ECS registry、GT 栈引用或可变 `EditorConfig`。
- FramePacket、SceneUpdateBatch、UI packet 必须值拥有、移动拥有或使用生命周期明确的 typed ref。
- RT-owned RenderScene/GpuScene 只能在 drain/Sync 后销毁。

### 5.2 UI 边界

- GT 捕获 copied ImGui draw packet。
- RT 消费 packet，不读取 ImGui 全局 draw data。
- 平台 viewport GPU 资源 create/resize/destroy 与 present 必须保持 FIFO 和线程归属。
- 不要把未最大化窗口截图误判成渲染错误；要检查窗口状态和实际像素内容。

### 5.3 RHI packet ownership

- `Execute()` 返回后，RHI packet 的 CPU payload 仍必须独立有效。
- 不能从任意裸 `RHIResource*` 反推并构造通用 `CountableRef`。这曾导致无效 `VkBuffer 0xdfdf...`。
- 只对 owner 协议明确的 typed ref 做强持有，其余依赖 renderer drain、queue Sync 和 deferred release。

### 5.4 FIFO 与 timeline

- Execute 与 Present 必须保持同一个 graphics RHI FIFO 顺序。
- enqueue 时一旦公开 timeline 值，所有空命令、acquire failure、resize/out-of-date 过渡分支都必须推进该值。
- RHI submission worker 不能阻塞等待 GPU completion，也不能在 threaded Present 中无限 acquire。

### 5.5 descriptor ring

- descriptor ring 只按实际消费 descriptor storage 的 Execute serial 推进。
- 严禁重新使用包含 Present 的 graphics `_timeline % ring_size` 选槽。
- 当前三个 descriptor/command allocator in-flight 槽只由单 RHI worker 串行推进；未来并行录制时必须重新设计每个 recorder 的 arena 和退休条件。

### 5.6 VkQueue host synchronization

- Vulkan 外部同步以实际 `VkQueue` handle 为单位，不以引擎的逻辑 queue wrapper 名称为单位。
- 同一 handle 上的 `vkQueueSubmit2`、`vkQueuePresentKHR`、`vkQueueWaitIdle` 和 queue debug label 必须使用同一 canonical mutex。

### 5.7 回退语义

```cpp
rhi_thread_enabled = rhi_thread && !rhi_bypass;
```

- `rhi_thread=false`：同步 baseline。
- `rhi_thread=true, rhi_bypass=true`：显式同步 bypass，不创建 RHI worker。
- `rhi_thread=true, rhi_bypass=false`：graphics RHI FIFO threaded mode。
- 回退路径是调试资产，不能为了简化新代码而删除。

---

## 6. 当前验证基线

### 6.1 构建环境说明

原电脑没有可用的 `just`，最终使用现有 Visual Studio/MSVC Debug build tree：

```powershell
cmake --build build --config Debug --target MoerEditor --parallel 4
```

构建通过，只保留既有 C4244/C4715 warning。

仓库标准环境仍是 Ninja + Clang + C++20。换机后优先执行：

```powershell
just b
```

若 `just` 不可用，再按 `docs/BUILD.md` 配置，并使用等价的 `cmake --build`。不要把“MSVC 已通过”误写成“标准 Ninja/Clang 已通过”。

### 6.2 默认配置

正常开发结束后，根 `MoerEngine.toml` 必须保持：

```toml
[engine.threading]
render_thread = false
rhi_thread = false
rhi_bypass = true
max_frame_lag = 0
profile_logging = false

[engine.render]
default_render_method = "Raster"
```

矩阵会用 `--config` 生成独立场景配置，不应改写根配置或可执行目录的默认配置。

### 6.3 自动矩阵

关键工具：

- `tools/threading/runtime_verify.py`
- `tools/threading/run_matrix.py`
- `tools/threading/README.md`

常用命令：

```powershell
python -m py_compile tools\threading\run_matrix.py tools\threading\runtime_verify.py
python tools\threading\run_matrix.py --set smoke
python tools\threading\run_matrix.py --set full --continue-on-failure
python tools\threading\run_matrix.py --set soak --repeat 3 --soak-seconds 300
```

完整矩阵包含：

| 场景 | Renderer | RT | lag | RHI |
|---|---|---:|---:|---|
| `raster_sync` | Raster | off | 0 | off |
| `raster_rhi_bypass` | Raster | off | 0 | bypass |
| `raster_rhi_gt` | Raster | off | 0 | threaded |
| `raster_rt0_rhi` | Raster | on | 0 | threaded |
| `raster_rt1_rhi` | Raster | on | 1 | threaded |
| `raster_rt1_rhi_off` | Raster | on | 1 | off |
| `ray_rhi_gt` | Raytracing | off | 0 | threaded |
| `ray_rt0_rhi` | Raytracing | on | 0 | threaded |
| `ray_rt1_rhi` | Raytracing | on | 1 | threaded |

Phase 5 最终结果：

```text
Build: PASS
Raytracing targeted probe: PASS
Full matrix: 9/9 PASS
Duration: 187.1 s
Minimum screenshot nonblack ratio: 0.7994
Normal process exit: 9/9
Severe raw-log matches: 0
```

严重错误扫描至少覆盖：

```text
assertion failed
VUID-
device lost
vkQueueSubmit2 FAILED
tracked buffer
remaining allocation
access violation
```

原电脑的 `target/validation/rt_rhi/` 被忽略，不会随分支推送。换机后必须生成自己的报告，不能因为本文件记录了 PASS 就跳过基线复现。

### 6.4 性能基线解释

Phase 5 单轮尾窗口的代表性数据：

```text
Raster synchronous RHI caller: 约 3.128 ms
Raster threaded RHI caller: 约 0.003 ms
Ray threaded RHI caller: 约 0.007-0.008 ms
Ray RT lag 1 PrepareFrame: 约 46.929 ms
Ray RT lag 1 Render: 约 6.609 ms
Ray RT lag 1 GT wait: 约 0.037 ms
```

这些数字来自原机器、Debug Editor、Sponza 场景和单轮尾窗口，只是回归/归因基线：

- threaded caller 降到微秒级，说明提交已离开调用线程，但 backend wait/work 并没有消失。
- Ray lag 1 已显著降低 GT wait，当前更明显的瓶颈是 `PrepareFrame`。
- 不得把单轮结果宣称为正式百分比性能收益。
- 换机后比较趋势和线程拓扑是否一致，不要求绝对毫秒值完全相同。

---

## 7. Phase 5 断言事故的结论

这一结论必须保留，避免后续再次围绕错误症状修改交换链。

### 7.1 表面症状

```text
VulkanSwapChain.cpp
assert(false && "Error presenting to swapchain.")
```

### 7.2 首故障

Crash Diagnostic Layer 的 all-command instrumentation 定位到：

```text
DeviceFault: Instruction Pointer Fault + Invalid Read
Last completed command: vkCmdPushConstants
First incomplete command: vkCmdDispatch(86, 1, 1)
Label stack:
  Graphics Exec
  -> Begin Layers
  -> Raster GeometryPass
  -> Raster Geometry Culling
  -> Culling
```

因此 Present 断言是 GPU 已 device lost 后的二次错误，不是首故障。

### 7.3 根因与修复

graphics timeline 同时为 Execute 和 Present 递增，但 Present 不消费 renderer descriptor ring。旧代码用 `_timeline % 3` 选 descriptor 槽，导致 timeline `556` 和 `559` 的 Execute 在前者仍运行时碰撞同一槽。

`558f4055` 增加 execute-only `descriptor_submission`，只为有 command 的 Execute 推进 descriptor ring。同时修复了同一 `VkQueue` 的 host synchronization。

修复后完成五次 Raster 定向运行、一次 Ray 探针、一次 synchronization validation 和最终九场景矩阵，未再复现。

### 7.4 尚未完成的相关 hardening

当前没有完整的 device-fault latch。若未来出现真实 device lost，completion/present/allocator 路径仍可能继续执行并制造二次断言或 reset VUID。

不能用以下方式“修复”：

- 直接删除 Present 断言。
- 将所有错误统一改成 warning 后继续提交。
- 在 device lost 后继续 reset command pool/allocator。
- 用 `vkDeviceWaitIdle` 试图等待尚未提交到 Vulkan queue 的 CPU FIFO work。

正确方向见下一节 Phase 6。

---

## 8. 尚未完成的目标

以下项目都没有完成，状态汇报时必须明确标注。

### 8.1 跨机器/标准工具链验收

- 尚未在另一台机器复现 smoke/full matrix。
- 尚未在标准 Ninja + Clang Debug/Release 上完成同等矩阵。
- `WITH_NRD=ON`、CUDA 等可选路径未形成完整回归。

### 8.2 Device-fault 状态传播

- 没有 device-level 或 graphics-queue-level first-fault latch。
- `Submit`、`Present`、fence `HostWait`、completion 和 shutdown 没有统一失败传播协议。
- device lost 后如何停止新 work、解除 CPU wait、跳过不安全回收仍需设计和验证。

### 8.3 Raytracing PrepareFrame 性能归因

- 已知道 Ray lag 1 的 PrepareFrame 约为 `46.9 ms`，但尚未把 Scene snapshot、light preprocess、UI packet、资源同步等子阶段分别计时。
- 尚未实施有证据支持的优化。

### 8.4 RenderGraph

- 尚未实现 RenderGraph/FrameGraph。
- pass/resource 依赖仍主要由现有线性 pass 调度和 command reorder/barrier preprocess 表达。
- transient resource lifetime、pass culling、graph compile 和 graph debug view 均未实现。

原调研曾把 RenderGraph 称为 Phase 5；实际执行中 Phase 5 被用于稳定性回归和性能基线。后续统一将 RenderGraph 顺延，避免编号混乱。

### 8.5 并行命令录制

- 当前只有单个 RHI submission worker 串行 command recording。
- 尚未为每个 worker 建立独立 command pool、allocator、descriptor arena 和 profiler storage。
- 尚未基于显式 pass DAG 做并行录制。

### 8.6 更多 RHI queue 与 backend

- 当前 threaded RHI 只覆盖 Vulkan graphics queue。
- compute/copy queue 保持既有同步或辅助线程路径。
- D3D12 backend 没有迁移到同等 RHI Thread 模型。

### 8.7 更通用的资源所有权系统

- pipeline/resource lifetime 仍依赖 typed ref、RT drain、queue Sync 和 deferred release。
- 尚未建立统一 typed RHI ownership registry。
- `max_frame_lag` 只验证到 1，不支持无约束扩大。

---

## 9. 推荐的后续阶段顺序

不得把下面几个阶段合并成一次大改。

### Phase 6.0：换机基线复现

目标：证明远端分支在新环境上仍是可工作的冻结点。

工作内容：

1. checkout 正确分支和 submodule。
2. 核对默认同步 Raster 配置。
3. 完成构建。
4. 运行 smoke matrix。
5. 运行 full matrix。
6. 保存新机器的工具链、GPU、driver、构建类型和报告路径。

验收：

- smoke 全通过。
- full `9/9 PASS`，或对环境差异有明确、可复现的解释。
- 视觉结果正确，原始严重错误为 0。
- 基线失败前不开始下一阶段代码改动。

### Phase 6.1：Device-fault latch 与失败传播

目标：首个 Vulkan/device 失败发生后，停止制造 Present、allocator、command pool 等二次错误，并让 Sync/shutdown 有可证明的退出路径。

建议先研究并写小计划，再修改以下边界：

- `VulkanDevice`
- `VkCommandQueue` / `VkNativeQueue`
- `VkSwapchain`
- `VulkanFence::HostWait`
- `VulkanAllocator::Complete`
- `VulkanPresentor::Complete`
- completion worker 与 `Sync()`

设计要求：

- 原子保存 first fault，只记录第一处 operation、`VkResult`、queue、timeline/work serial。
- 区分 `VK_ERROR_DEVICE_LOST`、正常 out-of-date/suboptimal 和可重试 acquire 状态。
- fault 后拒绝新 submit/present，并解除或失败完成 CPU 侧等待，不能形成 shutdown deadlock。
- 不在 device lost 后 reset 依赖 GPU 完成状态的 command pool/allocator。
- 保留足够诊断信息，不能只返回 `false`。
- 最好通过可控 fault-injection seam 测失败传播，不要故意制造非法 GPU 地址。

验收：

- 正常路径 build + smoke + full matrix 不回归。
- synchronization validation 无新增 VUID。
- fault injection 能只报告一次首故障，进程不死锁，不出现 Present/command-pool 级联噪声。
- 单独阶段提交，并更新本文件的进度记录。

### Phase 7：PrepareFrame 性能归因与低风险优化

目标：先知道时间花在哪里，再做局部优化。

建议顺序：

1. 将 Raster/Raytracing `PrepareFrame` 拆成稳定子指标。
2. 重复运行固定场景，至少采集三轮尾窗口。
3. 找到最大子阶段并提出不破坏所有权边界的优化。
4. 一次只优化一个来源，保留 off/on 或前后对照。

验收：

- 指标不会引入明显热路径开销，默认 `profile_logging=false`。
- 给出重复样本而非单轮百分比。
- full matrix 与视觉结论不回归。

### Phase 8：RenderGraph 最小骨架

目标：先把现有线性 pass 迁入显式 graph，初版不追求并行。

建议范围：

- 先在 Raster 建立 pass/resource declaration 和 graph compile。
- 初版按原顺序串行执行，输出必须与现有路径一致。
- 明确 graph 与现有 command reorder/barrier preprocess 的职责，避免两套系统重复或冲突地产生 barrier。
- 建立 transient resource lifetime、外部资源 import/export 和 graph debug dump。
- 保留旧线性路径作为配置回退，直到矩阵稳定。

验收：

- graph off/on 的 Raster 截图和关键 buffer/texture 结果一致。
- resize、reload、renderer switch、lag 0/1、RHI off/bypass/threaded 都通过。
- 不在本阶段同时引入 parallel command recording。

### Phase 9：并行命令录制

目标：基于已经显式化的 pass DAG 并行录制独立 pass/batch。

必须先解决：

- 每个 worker 独立 command pool/allocator。
- descriptor arena/ring 的 recorder ownership 和 GPU 退休条件。
- pipeline/cache/profiler 的并发访问。
- secondary/primary command buffer 合并策略。
- graph dependency 与 TaskGraph job 的完成关系。

验收必须包含高压力重复矩阵和 synchronization validation，不能只看平均帧时间。

### Phase 10：扩展 queue/backend 与 typed ownership

在 graphics + RenderGraph + parallel recording 稳定后，再评估：

- Vulkan compute/copy RHI worker。
- D3D12 对等线程模型。
- typed resource lifetime registry。
- 更高 frame lag。

这些项目不能提前穿插到 Phase 6/7。

---

## 10. 换机接管流程

### 10.1 获取分支

新 clone：

```powershell
git clone --recurse-submodules https://github.com/NJUCG/MoerEngine.git
Set-Location MoerEngine
git switch --track origin/feature/rt-rhi-threading
git submodule update --init --recursive
```

已有 clone：

```powershell
git fetch origin
git switch feature/rt-rhi-threading
git pull --ff-only origin feature/rt-rhi-threading
git submodule update --init --recursive
```

### 10.2 身份核对

```powershell
git status --short --branch
git merge-base --is-ancestor d016d1d8f5f96a8909bc8bfdaa6468e9810ad6a5 HEAD
git log --oneline --decorate d016d1d8f5f96a8909bc8bfdaa6468e9810ad6a5..HEAD
git show --stat f55c016e
```

预期：

- 当前分支是 `feature/rt-rhi-threading`。
- `f55c016e` 存在且是交接文档之前的功能冻结点。
- 工作区在开始新任务前干净；若不干净，先识别并保护用户改动。

### 10.3 必读文件

```text
AGENTS.md
MoerEngine.toml
docs/GeneratedByAI/260716-RT_RHI线程重构续作Agent提示词.md
tools/threading/README.md
source/runtime/Engine.cpp
source/runtime/render/RenderThread.h
source/runtime/render/rhi/vulkan/VulkanQueue.h
source/runtime/render/rhi/vulkan/VulkanQueue.cpp
```

### 10.4 基线执行

```powershell
just b
python -m py_compile tools\threading\run_matrix.py tools\threading\runtime_verify.py
python tools\threading\run_matrix.py --set smoke
python tools\threading\run_matrix.py --set full --continue-on-failure
```

若没有 `just`，按 `docs/BUILD.md` 配置后执行等价构建。

### 10.5 基线失败时

按以下顺序排查：

1. 确认 `--config` override 日志存在，场景没有误读根 TOML。
2. 确认 scene asset、submodule、shader compiler、Vulkan layer 和 GPU driver 完整。
3. 确认 ready marker 是 `Copied ImGui frame includes`，不要只等待 HWND 出现。
4. 检查 stdout/stderr 的第一个严重错误，不要从最后一个断言倒推根因。
5. 视觉异常先核对窗口是否最大化、resize 是否完成、截图非黑比例和实际场景内容。
6. 若是 device lost，优先 synchronization validation、Crash Diagnostic 或 RenderDoc，不要先改 Present。
7. 将环境问题和代码回归分开提交；不要在同一提交里顺手重构架构。

---

## 11. 关键代码地图

### GT / RT 调度

- [`source/runtime/Engine.cpp`](../../source/runtime/Engine.cpp)
- [`source/runtime/render/RenderThread.h`](../../source/runtime/render/RenderThread.h)
- [`source/runtime/render/RenderThread.cpp`](../../source/runtime/render/RenderThread.cpp)

### FramePacket 与 UI packet

- [`source/runtime/render/renderer/raster/RasterFramePacket.h`](../../source/runtime/render/renderer/raster/RasterFramePacket.h)
- [`source/runtime/render/renderer/raytracing/RaytracingFramePacket.h`](../../source/runtime/render/renderer/raytracing/RaytracingFramePacket.h)
- [`source/runtime/render/renderer/common/UIRenderer.h`](../../source/runtime/render/renderer/common/UIRenderer.h)

### Scene / RenderScene / GpuScene

- [`source/runtime/render/scene/Scene.h`](../../source/runtime/render/scene/Scene.h)
- [`source/runtime/render/scene/SceneTickSync.cpp`](../../source/runtime/render/scene/SceneTickSync.cpp)
- [`source/runtime/render/scene/GpuSceneUpdate.h`](../../source/runtime/render/scene/GpuSceneUpdate.h)
- [`source/runtime/render/scene/RenderScene.h`](../../source/runtime/render/scene/RenderScene.h)
- [`source/runtime/render/scene/RenderScene.cpp`](../../source/runtime/render/scene/RenderScene.cpp)
- [`source/runtime/render/scene/GpuScene.h`](../../source/runtime/render/scene/GpuScene.h)
- [`source/runtime/render/scene/GpuScene.cpp`](../../source/runtime/render/scene/GpuScene.cpp)

### RHI packet 与 Vulkan backend

- [`source/runtime/render/rhi/RHICommandList.cpp`](../../source/runtime/render/rhi/RHICommandList.cpp)
- [`source/runtime/render/rhi/RHIImpl.h`](../../source/runtime/render/rhi/RHIImpl.h)
- [`source/runtime/render/shader/ShaderPipeline.h`](../../source/runtime/render/shader/ShaderPipeline.h)
- [`source/runtime/render/rhi/vulkan/VulkanQueue.h`](../../source/runtime/render/rhi/vulkan/VulkanQueue.h)
- [`source/runtime/render/rhi/vulkan/VulkanQueue.cpp`](../../source/runtime/render/rhi/vulkan/VulkanQueue.cpp)
- [`source/runtime/render/rhi/vulkan/VulkanDevice.h`](../../source/runtime/render/rhi/vulkan/VulkanDevice.h)
- [`source/runtime/render/rhi/vulkan/VulkanDevice.cpp`](../../source/runtime/render/rhi/vulkan/VulkanDevice.cpp)
- [`source/runtime/render/rhi/vulkan/VulkanSwapChain.cpp`](../../source/runtime/render/rhi/vulkan/VulkanSwapChain.cpp)
- [`source/runtime/render/rhi/vulkan/VulkanAllocator.cpp`](../../source/runtime/render/rhi/vulkan/VulkanAllocator.cpp)
- [`source/runtime/render/rhi/vulkan/VulkanRHIResource.cpp`](../../source/runtime/render/rhi/vulkan/VulkanRHIResource.cpp)

### 配置与验证

- [`source/runtime/core/include/config/GlobalConfig.h`](../../source/runtime/core/include/config/GlobalConfig.h)
- [`source/runtime/core/include/config/ConfigManager.h`](../../source/runtime/core/include/config/ConfigManager.h)
- [`tools/threading/README.md`](../../tools/threading/README.md)
- [`tools/threading/run_matrix.py`](../../tools/threading/run_matrix.py)
- [`tools/threading/runtime_verify.py`](../../tools/threading/runtime_verify.py)

---

## 12. 阶段开发与提交标准

每个新阶段必须执行：

1. `git status`，保护用户现有变更。
2. 写出该阶段目标、触点、风险和验收标准。
3. 小步编辑，不混入无关重构。
4. 优先 `just b`，否则使用等价 CMake 构建。
5. 运行受影响 renderer；线程、资源生命周期或 runtime 行为变化必须运行 Editor。
6. 至少覆盖 feature off/on、Raster/Raytracing 相关组合、lag 0/1、窗口 resize/restore 和正常退出。
7. 扫描 assertion、VUID、device lost、submit failure 和资源残留。
8. 视觉检查主窗口和平台 viewport，不能只依赖 exit code。
9. 运行 `git diff --check` 和脚本语法检查。
10. 只暂存自己的文件，创建中文语义清楚的阶段提交。
11. 更新本文件“进度追加区”，记录 commit、构建、运行证据和下一步。
12. 推送当前 feature branch，不直接推送 main。

禁止事项：

- 不要提交 `target/`、`.codex_tmp/`、shader cache、Crash Diagnostic dump 或本地截图。
- 不要删除同步/bypass 回退路径。
- 不要同时实现 Device fault、RenderGraph 和 parallel recording。
- 不要在没有验证的情况下增大 `max_frame_lag`。
- 不要让 RHI submission worker 承担阻塞式 GPU completion。
- 不要把 raw pointer 扫描包装成通用资源保活。
- 不要恢复 `_timeline % 3` descriptor 选槽。
- 不要因为最后出现 Present 断言就把交换链当作首故障。

---

## 13. 新 Agent 第一次回复模板

第一次接手时，回复至少包含以下内容：

```text
1. 已确认仓库、分支、base commit 和功能冻结点。
2. 当前工作区是否干净；若不干净，哪些属于用户变更。
3. Phase 0-5 已完成目标的简要确认。
4. 尚未完成：跨机基线、device-fault latch、PrepareFrame 归因、RenderGraph、并行录制、更多 queue/backend。
5. 本机将使用的编译器、构建配置和 GPU/driver。
6. 先执行 build + smoke + full matrix；基线通过后再开始 Phase 6.1。
7. 如果用户尚未明确授权继续开发，在这里停止并等待，不修改功能代码。
```

---

## 14. 进度追加区

后续 Agent 在每个阶段完成后，按以下格式追加，不要重写历史状态：

```markdown
### YYYY-MM-DD：Phase X 名称

- 目标：
- 主要改动：
- 提交：`<hash> <message>`
- 构建：
- 运行矩阵：
- 视觉结论：
- 错误扫描：
- 已知限制：
- 下一步：
```

当前最后记录：

### 2026-07-16：Phase 5 冻结与换机交接

- 功能冻结点：`f55c016e`。
- 构建：MSVC Debug PASS。
- 定向回归：Raster 五次、Raytracing 一次、synchronization validation 均 PASS。
- 完整矩阵：`9/9 PASS`，九个进程正常退出。
- 视觉结论：Raster/Raytracing Sponza 场景正确；此前差异来自窗口未展开，不是渲染错误。
- 错误扫描：assertion、VUID、device lost、submit failure、access violation、资源残留均为 0。
- 默认配置：同步 Raster。
- 当前状态：停止继续开发，等待换机后先执行 Phase 6.0 基线复现。
- 下一步：基线通过并获得用户授权后，独立实施 Phase 6.1 device-fault latch；不要直接跳到 RenderGraph。
