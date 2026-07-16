# MoerEngine RT/RHI 线程重构续作 Agent 提示词

> 本文件整体就是交给后续 Agent 的中文提示词和状态快照。
>
> 当前功能开发仍冻结在 **Phase 8 完成点**。Phase 9 并行命令录制的前置研究已经完成，当前结论为 `STOP -> conditional GO`：先做串行测量、物理资源版本、不可变录制计划和 recorder ownership，再决定是否进入小型原型；不得直接删除串行边开始并行。
>
> 换机后，请让 Agent 先完整阅读本文件、仓库根目录 `AGENTS.md` 和 `tools/threading/README.md`，再执行“换机接管流程”。不要只截取“下一步”一节，否则容易丢失已经建立的线程与资源所有权约束。
>
> `MoerEngine` 与 `TechRecord` 是本任务的两个必选仓库。每个阶段必须同时交付代码/分支记录和 TechRecord 改进记录；只更新其中一个仓库不能标记阶段完整完成。

---

## 1. 给接手 Agent 的首要指令

你现在接手的是 **MoerEngine Render Thread / RHI Thread 整体重构**，不是一个孤立的 Vulkan bug 修复任务。

当前必须遵守以下指令：

1. 使用中文沟通和记录结论。
2. Phase 5 历史冻结点为 `f55c016e`；当前 Phase 8 功能完成点为 `fd59508c`。Phase 6.0/6.1/7/8 的改动和验证必须与本文件进度区及 TechRecord 对应，不应存在未经记录的新功能实现。
3. 第一个动作必须是核对分支身份、工作区、配置、构建和自动验证基线。不要一上来修改代码。
4. 如果用户只让你“查看状态”或“接手”，先汇报已完成目标、未完成目标和下一阶段计划，等待用户明确授权后再开发。
5. 如果用户同时明确说“继续实现”，也要先完成换机基线复现；基线失败时先定位环境或回归原因，不得把失败状态直接带入下一阶段。
6. 每个阶段都执行小步 `编辑 -> 构建 -> 运行 -> 多配置观察 -> 日志扫描 -> 阶段提交`，并在阶段边界停止汇报。
7. 用户特别要求阶段性成果分阶段提交。只暂存本阶段自己的文件，不得夹带用户的其他工作区变更。
8. 每个阶段都必须把架构决策、实现结果、验证证据、问题复盘和剩余 TODO 同步到 TechRecord，单独提交并推送 TechRecord 当前分支。
9. 不得强推、重写当前分支历史或随意 rebase。若未来 `main` 已前进，先完成当前分支基线，再单独评估合并策略。
10. 不得为了“消除报错”删除断言、忽略 Vulkan 返回值或放宽验证脚本。必须分析首个失效点。
11. 视觉判断要区分“窗口未展开”和“渲染错误”。此前用户已经确认过一次截图只是窗口尺寸不同，画面渲染本身正确。

---

## 2. 仓库与分支身份

### 2.1 固定信息

```text
Repository: https://github.com/NJUCG/MoerEngine.git
Branch: feature/rt-rhi-threading
Base branch: main
Base commit on 2026-07-16: d016d1d8f5f96a8909bc8bfdaa6468e9810ad6a5
Phase 5 historical freeze commit: f55c016e
Phase 6.0 takeover code commit: 402b7c66
Phase 6.1 current functional commit: 8afa2cac
Phase 7 current functional commit: db76cfd6
Phase 8 current functional commit: fd59508c
```

用户最初写的 `feature/rt-thi-threading` 是拼写误差；远端实际分支和当前 upstream 均为 `feature/rt-rhi-threading` / `origin/feature/rt-rhi-threading`。

`f55c016e` 仍是 Phase 5 历史基线，不再是当前功能终点。判断最新功能代码应看 `fd59508c`，判断最新交接文档应看分支 HEAD。

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

### 2.3 必选 TechRecord 仓库

TechRecord 是本任务的第二个必选交付仓库，不是可选参考资料：

```text
Repository: https://github.com/Irk2wd/TechRecord.git
Expected local path: D:\Other_Files\TechRecord
Branch: main
```

换机后必须先确认该仓库已存在并同步到远端最新状态。若本地路径不同，可以使用实际 clone 路径，但不能跳过 TechRecord 阶段记录。

RT/RHI 总路线与实施记录位于：

```text
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/
```

问题或回归复盘位于：

```text
30_问题复盘/BugFix/
```

每个阶段结束前必须完成：

1. 阅读 `00_调研与初步方向.md` 和最新一篇阶段实施记录。
2. 在 `RT_RHI_Threading/` 中继续编号，新建或更新本阶段中文实施记录。
3. 将本阶段完成状态、MoerEngine commit 和实施记录链接追加到 `00_调研与初步方向.md`。
4. 若本阶段发现崩溃、断言、竞态、资源泄漏、性能回归或容易复发的坑，在 `30_问题复盘/BugFix/` 新建或更新专项复盘。
5. 记录真实改动范围、架构决策、被否决方案、构建命令、运行矩阵、视觉结论、日志证据、性能数据、已知限制和下一步。
6. 只记录可长期复用的知识，不把大段临时控制台输出、截图或本机缓存提交到 TechRecord。
7. 在 TechRecord 仓库只暂存本阶段自己的文档，执行 diff 检查、提交并推送其当前分支。
8. 最终汇报必须同时给出 MoerEngine 与 TechRecord 的 commit、远端分支和推送结果。

若 TechRecord 暂时不可访问、认证失败或存在无法安全合并的冲突，必须明确报告该阶段“文档交付未完成”，继续处理同步问题；不得静默跳过，也不得仅更新本文件代替 TechRecord。

当前最近的 TechRecord 阶段记录：

```text
Phase 6.0 TechRecord commit: 845f73e
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/11_Phase6.0换机基线复现与接管回归.md
30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase6.0_换机基线与Swapchain信号量复用复盘.md

Phase 6.1 TechRecord commit: 83bedaf
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/12_Phase6.1_DeviceFault锁存与失败传播.md
30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase6.1_DeviceFault锁存与Swapchain生命周期复盘.md

Phase 7 TechRecord commit: 0f30519
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/13_Phase7_PrepareFrame性能归因与低风险优化.md
30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase7_无变化SunPatch与PrepareFrame更新放大复盘.md

Phase 8 TechRecord commit: 4474339
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/14_Phase8_RasterRenderGraph最小串行骨架.md
30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase8_DepthBuffer物理身份与Graph依赖断链复盘.md
```

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

长期终态还包括具备物理 transient/culling/barrier handoff 的成熟 RenderGraph、并行命令录制和更多队列/backend 的线程化；Phase 8 只完成最小串行骨架，不能把这些终态能力写成已完成。

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

状态：完成，是 Phase 6.0/6.1 的历史基线。

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

### 4.10 Phase 6.0：换机基线复现与接管回归

状态：完成。

- 在 RTX 5080、Visual Studio 2022 / MSVC Debug 环境完成换机构建与九场景矩阵。
- 修复 render-finished binary semaphore 未按实际 acquired image index 配对导致的 `VUID-vkQueueSubmit2-semaphore-03868`。
- copied ImGui ready marker 移到 draw packet 的真实消费点；第一启动 dock 目标更新为 `Game`。
- 矩阵统一使用 tracked `template.MoerEngine.toml`，没有覆盖开发者本机根配置。

提交：

```text
402b7c66 fix(vulkan): 修复换机线程基线回归
```

### 4.11 Phase 6.1：Device-fault latch 与失败传播

状态：完成。

- 新增 device 级 `Healthy -> Publishing -> Faulted` first-fault latch 与完整 operation/context record。
- Submit/Present/Acquire 接入 native admission gate；fault 后拒绝新 native submit/present。
- 引入 typed Vulkan outcome、submission-acceptance handshake 和独立 `cpu_settled_frame`，失败不再伪装成 GPU completion。
- success-only callback、allocator/presentor quarantine、command-pool reset skip、queued-work cancel/drain 形成 fail-closed shutdown。
- swapchain recreate 改为临时资源事务，并修复 oldSwapchain retirement、Acquire 有效输出、`VK_INCOMPLETE` 和 present-fence 槽位复用边角。
- `VK_ERROR_SURFACE_LOST_KHR` 因尚无 surface reconstruction 协议而归为 terminal fault。
- 新增严格 `--vulkan-fault-inject=present-submit@3` seam 与独立 `fault` 验证集。

提交：

```text
8afa2cac feat(vulkan): 完成 DeviceFault 锁存与失败传播
```

### 4.12 Phase 7：PrepareFrame 性能归因与低风险优化

状态：完成。

- 新增默认关闭的 `[ThreadingProfile][Prepare]`，统一覆盖 Raster/Raytracing 的同步与 RT 路径。
- 三轮归因确认 Ray scene snapshot 约 `17 ms`，无变化 Sun Patch 放大的 scene update 约 `7.7 ms`。
- 只在 light/node 实际值变化时 Patch，Ray RT0/RT1 PrepareFrame 中位数分别下降约 `28.9%/28.5%`。
- steady dirty/update GPU/geometry snapshot 比例从 `100%` 降到 `0%`；Ray scene snapshot 缓存保持为独立后续性能阶段。

提交：

```text
db76cfd6 perf(threading): 归因并优化 PrepareFrame
```

### 4.13 Phase 8：Raster RenderGraph 最小串行骨架

状态：完成，是当前功能冻结点。

- 重写最小 RenderGraph 契约：显式 pass/resource、import alias、logical transient lifetime、export、RAW/WAR/WAW、稳定拓扑顺序与确定性 dump。
- Raster linear/graph 共用一份 `define_raster_passes(schedule)`；默认 graph 关闭，compile/execute 前置失败按 renderer instance latch 到 linear。
- Phase 8 强制相邻 `serial` 边；RenderGraph 不生成 barrier、不修改 tracked state，RHI command preprocess 仍是唯一真实 barrier owner。
- 新增 `TestRenderGraph`、7 场景 graph matrix、8 场景关键 framebuffer matrix、BMP comparator 和 Raster reload/switch self-exit 生命周期 seam。
- 修复 DepthBuffer wrapper 与底层 Texture identity 不一致导致的 DAG 依赖断链；最终 dump 证明 `Geometry -> UiCombine [RAW:depth]`。
- 最新 graph `7/7`、关键资源 `8/8`、显式 synchronization validation `1/1`、fault `1/1` 全部 PASS。

提交：

```text
fd59508c feat(rendergraph): 建立 Raster 最小串行骨架
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

### 5.8 Device fault 与提交接受边界

- future timeline 已分配不等于 producer 已被 Vulkan queue 接受；consumer 的 GPU wait 必须先通过 submission-acceptance handshake。
- first terminal fault 只能发布一次，并携带 operation、result、queue、timeline/work serial；后续错误不能覆盖首故障。
- fault 发布后不得再调用 native submit/present；正常 shutdown、fault harness 和后续新入口都必须维持 post-fault native call 为 0。
- 不得用 host timeline signal 伪造 producer 的 GPU work 已完成。

### 5.9 CPU settled、GPU completion 与失败资源

- CPU-settled 只说明逻辑 work 已有最终成功/失败结论，不等于 GPU completed。
- success-only callback 只能在真实 GPU success 后执行。
- GPU completion 未证明时，allocator/presentor 必须 quarantine，command pool reset 必须跳过。
- lost device 不等待不可达的 GPU timeline；非 device-lost completion unknown 必须 fail-closed，不能进入可能 UAF 的 Vulkan 清理。
- `VK_ERROR_SURFACE_LOST_KHR` 当前是 terminal fault；在建立 surface owner/reconstruction 协议前，不得改回无消费者的普通 Recreate。

### 5.10 RenderGraph 与 barrier 所有权

- Phase 8 RenderGraph 只拥有 logical access、dependency、稳定串行顺序、lifetime 和诊断，不得直接修改 RHI tracked state。
- 真实 barrier 的唯一 owner 仍是 command reorder / backend preprocess；在建立明确 handoff 前，不得让 graph 与 preprocess 同时生成 barrier。
- texture physical identity 必须 canonical 到 view/barrier 所用的底层 Texture，不能使用任意 typed wrapper 地址。DepthBuffer wrapper 与内部 Texture 不同。
- graph fallback 必须在第一个 pass callback 前决定；未来若允许 Execute 中途失败，必须先引入 typed execution result 或 executed-pass count。
- `processing_image` token 可能代表多个物理纹理，HiZ commit 包含 CPU 侧资源交换；解决物理版本前不得据此安全重排或并行。
- `render_graph=false` 的 linear 路径仍是默认和调试资产，Phase 9 不得删除。

---

## 6. 当前验证基线

### 6.1 构建环境说明

当前接管机器没有可用的 `just`、Ninja/Clang，Phase 6.0-8 使用现有 Visual Studio/MSVC Debug build tree：

```powershell
cmake --build build --config Debug --target TestRenderGraph MoerEditor --parallel 4
```

构建通过，只保留既有 C4244/C4715 warning。

仓库标准环境仍是 Ninja + Clang + C++20。未来具备该工具链时优先执行：

```powershell
just b
```

若 `just` 不可用，再按 `docs/BUILD.md` 配置，并使用等价的 `cmake --build`。当前结论仍只是 MSVC Debug，不得写成标准 Ninja/Clang 或 Release 已通过。

### 6.2 默认配置

受版本管理的 `template.MoerEngine.toml` 保持同步 Raster baseline：

```toml
[engine.threading]
render_thread = false
rhi_thread = false
rhi_bypass = true
max_frame_lag = 0
profile_logging = false

[engine.render]
default_render_method = "Raster"

[engine.render.raster]
render_graph = false
render_graph_debug_dump = false
```

矩阵以该 template 为 base，并用 `--config` 生成独立场景配置，不应改写根配置或可执行目录的默认配置。当前机器的根 `MoerEngine.toml` 是开发者既有、被忽略的 Raytracing 配置，Phase 6.0-8 均未覆盖。

### 6.3 自动矩阵

关键工具：

- `tools/threading/runtime_verify.py`
- `tools/threading/run_matrix.py`
- `tools/threading/compare_captures.py`
- `tools/threading/README.md`

常用命令：

```powershell
python -m py_compile tools\threading\run_matrix.py tools\threading\runtime_verify.py tools\threading\compare_captures.py
python tools\threading\run_matrix.py --set smoke --base-config template.MoerEngine.toml
python tools\threading\run_matrix.py --set full --continue-on-failure --base-config template.MoerEngine.toml
python tools\threading\run_matrix.py --set rendergraph --continue-on-failure --base-config template.MoerEngine.toml
python tools\threading\run_matrix.py --set rendergraph-resources --continue-on-failure --base-config template.MoerEngine.toml
python tools\threading\run_matrix.py --set fault --base-config template.MoerEngine.toml
python tools\threading\run_matrix.py --set soak --repeat 3 --soak-seconds 300 --base-config template.MoerEngine.toml
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

Phase 6.1 历史最终结果：

```text
Build: PASS
Smoke matrix: 3/3 PASS
Synchronization validation: 1/1 PASS
Fault injection: 1/1 PASS
Full matrix: 9/9 PASS
Full duration: 263.9 s
Full minimum screenshot nonblack ratio: 0.9477
Normal process exit: 9/9
Severe raw-log matches: 0
```

最终证据目录：

```text
target/validation/rt_rhi/phase6_1_fault_final3
target/validation/rt_rhi/phase6_1_smoke_final3
target/validation/rt_rhi/phase6_1_syncval_final
target/validation/rt_rhi/phase6_1_full_final3
```

`fault` 与正常 `full` 必须保持分离。fault harness 只豁免完全锚定且字段正确的唯一 `[VulkanFault][First]` error 行，并要求 Injection/First/Summary 各恰好一次；不得把宽泛的 `device lost` 豁免带入正常矩阵。

Phase 8 最新结果：

```text
Build + TestRenderGraph: PASS
Linear/full matrix: 9/9 PASS, 267.8 s, min nonblack 0.947685
RenderGraph matrix: 7/7 PASS, 223.2 s, min nonblack 0.965670
Key resource matrix: 8/8 PASS, 230.7 s
Synchronization validation: 1/1 PASS, SYNCHRONIZATION_VALIDATION_EXT confirmed
Fault injection: 1/1 PASS
Reload/switch lifecycle: 1/1 PASS, exact 4 drains / 3 Raster / 1 Raytracing destruction
Final graph-off/on: MAE 0.6055, RMSE 1.7544, p99 8, RGB<=2 ratio 0.8831
```

Phase 8 最终证据目录：

```text
target/validation/rt_rhi/phase8_full_linear_ray_20260716
target/validation/rt_rhi/phase8_rendergraph_final_20260716
target/validation/rt_rhi/phase8_key_resources_final_20260716
target/validation/rt_rhi/phase8_syncval_final_20260716
target/validation/rt_rhi/phase8_fault_final_20260716
target/validation/rt_rhi/phase8_reload_switch_final_v2_20260716
target/validation/rt_rhi/phase8_graph_off_on_compare_final_20260716
```

`rendergraph` 场景禁止 `[RenderGraph][Fallback]`；`rendergraph-resources` 的每个进程 PASS 仍不等于 off/on 视觉等价，必须另外保留 comparator JSON。生命周期场景不截图，改为验证正常 self-exit 和精确 drain/destruction 计数。

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

`target/validation/rt_rhi/` 被忽略，不会随分支推送。任何新机器或关键 Vulkan 生命周期改动后都必须生成自己的报告，不能因为本文件记录了 PASS 就跳过复现。

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

### 7.4 Phase 5 当时尚未完成的相关 hardening

Phase 5 节点当时没有完整的 device-fault latch。该缺口已由 Phase 6.1 的 `8afa2cac` 补齐：first-fault、native admission、submission acceptance、CPU-settled、失败资源隔离和 shutdown 已形成统一协议。真实 mid-flight TDR、device/surface recreation 与其他 fault entrypoint 仍属于后续工作。

不能用以下方式“修复”：

- 直接删除 Present 断言。
- 将所有错误统一改成 warning 后继续提交。
- 在 device lost 后继续 reset command pool/allocator。
- 用 `vkDeviceWaitIdle` 试图等待尚未提交到 Vulkan queue 的 CPU FIFO work。

已完成实现与验收见下一节 Phase 6.1 和进度追加区。

---

## 8. 尚未完成的目标

以下目标仍未达到长期终态；其中已完成的阶段性骨架会单独标注，状态汇报不得把阶段完成等同于终态完成。

### 8.1 跨机器/标准工具链验收

- 已在 RTX 5080 接管机器用 Visual Studio 2022 / MSVC Debug 复现 smoke/full matrix，并完成 Phase 6.0/6.1 最终矩阵。
- 尚未在标准 Ninja + Clang Debug/Release 上完成同等矩阵。
- 尚未覆盖 Release、其他 GPU/driver；`WITH_NRD=ON`、CUDA 等可选路径也未形成完整回归。

### 8.2 Device-fault 恢复与跨入口覆盖

- Phase 6.1 已完成 device-level first-fault latch、typed outcome、submission acceptance、CPU-settled、失败资源隔离和 fail-closed shutdown。
- 当前 synthetic seam 只覆盖预排空后的第三次 Present submit；真实 mid-flight TDR、Execute/Copy submit、external signal、OOM 与 driver 返回时序尚未覆盖。
- 尚未实现 device recreation、`VkSurfaceKHR` reconstruction 或通用 WSI recovery state machine；`SURFACE_LOST` 当前为 terminal fault。
- D3D12 backend 尚未迁移到同等 fault protocol。

### 8.3 Raytracing PrepareFrame 性能归因

- Phase 7 已完成稳定子阶段归因和单一低风险优化；无变化 Sun Patch 导致的 steady scene update 已消除。
- 当前最大剩余成本是每帧重建的 Ray scene snapshot，约 `17.1~17.8 ms`。
- snapshot cache/增量 invalidation 尚未设计，不能破坏 FramePacket 值语义或把 Scene/ECS 借用带入 RT。

### 8.4 RenderGraph

- Phase 8 已完成 Raster 最小串行骨架：pass/resource declaration、graph compile、logical transient lifetime、import/export、alias、确定性 dump 和旧路径回退。
- 当前仍无 pass culling、物理 transient allocator/alias reuse、subresource barrier generation、graph UI 或 parallel recording。
- graph 只表达 logical dependency；command reorder/barrier preprocess 仍拥有真实状态和 barrier。
- `processing_image` token、HiZ CPU swap 和物理资源版本尚未达到安全重排条件。

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

状态：已完成，提交 `402b7c66`；详细证据见进度追加区与 TechRecord `11_Phase6.0换机基线复现与接管回归.md`。

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

状态：已完成，提交 `8afa2cac`；详细证据见进度追加区与 TechRecord `12_Phase6.1_DeviceFault锁存与失败传播.md`。

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

状态：已完成，提交 `db76cfd6`；详细证据见进度追加区与 TechRecord `13_Phase7_PrepareFrame性能归因与低风险优化.md`。

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

状态：已完成，提交 `fd59508c`；详细证据见进度追加区与 TechRecord `14_Phase8_RasterRenderGraph最小串行骨架.md`。

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

状态：当前下一阶段，尚未开始。先完成设计/研究和门槛核对，不要把“删除 serial edge”当作实现方案。

目标：基于已经显式化的 pass DAG 并行录制独立 pass/batch。

必须先解决：

- 每个 worker 独立 command pool/allocator。
- descriptor arena/ring 的 recorder ownership 和 GPU 退休条件。
- pipeline/cache/profiler 的并发访问。
- secondary/primary command buffer 合并策略。
- graph dependency 与 TaskGraph job 的完成关系。
- graph physical version/subresource 与现有 barrier preprocess 的明确 handoff。
- `processing_image` 多物理纹理、HiZ CPU swap 和部分 Execute 失败的表达。

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

### 10.1 获取 MoerEngine 分支

新 clone：

```powershell
git clone --recurse-submodules https://github.com/NJUCG/MoerEngine.git
Set-Location MoerEngine
git switch --track origin/feature/rt-rhi-threading
git submodule update --init --recursive
```

已有 clone：

```powershell
git status --short --branch
git fetch origin
git switch feature/rt-rhi-threading
git pull --ff-only origin feature/rt-rhi-threading
git submodule update --init --recursive
```

### 10.2 获取并同步 TechRecord

新 clone：

```powershell
git clone https://github.com/Irk2wd/TechRecord.git D:\Other_Files\TechRecord
Set-Location D:\Other_Files\TechRecord
git switch main
git pull --ff-only origin main
```

已有 clone：

```powershell
Set-Location D:\Other_Files\TechRecord
git status --short --branch
git fetch origin
git switch main
git pull --ff-only origin main
```

如果 TechRecord 已有用户工作区修改，不得为了 pull 而 reset 或 checkout。先运行 `git status`，识别并保护修改；必要时在不丢失用户内容的前提下解决同步问题。

### 10.3 两个仓库身份核对

MoerEngine：

```powershell
Set-Location <MoerEngine路径>
git status --short --branch
git merge-base --is-ancestor d016d1d8f5f96a8909bc8bfdaa6468e9810ad6a5 HEAD
git log --oneline --decorate d016d1d8f5f96a8909bc8bfdaa6468e9810ad6a5..HEAD
git show --stat f55c016e
```

TechRecord：

```powershell
Set-Location <TechRecord路径>
git status --short --branch
git log -5 --oneline --decorate
Test-Path '20_技术文档\引擎架构\MoerEngine\RT_RHI_Threading\10_Phase5稳定性回归与线程性能基线.md'
Test-Path '30_问题复盘\BugFix\MoerEngine_RT_RHI_Phase5_描述符环复用与Present二次断言复盘.md'
```

预期：

- 当前分支是 `feature/rt-rhi-threading`。
- `f55c016e` 存在且是交接文档之前的功能冻结点。
- TechRecord 当前分支是 `main`，Phase 5 两篇记录存在。
- 两个工作区在开始新任务前都已核对；若不干净，先识别并保护用户改动。

### 10.4 必读文件

```text
# MoerEngine
AGENTS.md
MoerEngine.toml
docs/GeneratedByAI/260716-RT_RHI线程重构续作Agent提示词.md
tools/threading/README.md
source/runtime/Engine.cpp
source/runtime/render/RenderThread.h
source/runtime/render/rhi/vulkan/VulkanQueue.h
source/runtime/render/rhi/vulkan/VulkanQueue.cpp

# TechRecord
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/00_调研与初步方向.md
20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/10_Phase5稳定性回归与线程性能基线.md
30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase5_描述符环复用与Present二次断言复盘.md
```

### 10.5 基线执行

```powershell
just b
python -m py_compile tools\threading\run_matrix.py tools\threading\runtime_verify.py
python tools\threading\run_matrix.py --set smoke
python tools\threading\run_matrix.py --set full --continue-on-failure
```

若没有 `just`，按 `docs/BUILD.md` 配置后执行等价构建。

### 10.6 基线失败时

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

### RenderGraph 与 Raster 接入

- [`source/runtime/render/rendergraph/RenderGraph.h`](../../source/runtime/render/rendergraph/RenderGraph.h)
- [`source/runtime/render/rendergraph/RenderGraph.cpp`](../../source/runtime/render/rendergraph/RenderGraph.cpp)
- [`source/runtime/render/renderer/raster/RasterRenderer.h`](../../source/runtime/render/renderer/raster/RasterRenderer.h)
- [`source/runtime/render/renderer/raster/RasterRenderer.cpp`](../../source/runtime/render/renderer/raster/RasterRenderer.cpp)
- [`source/test/RenderGraphTest.cpp`](../../source/test/RenderGraphTest.cpp)

### 配置与验证

- [`source/runtime/core/include/config/GlobalConfig.h`](../../source/runtime/core/include/config/GlobalConfig.h)
- [`source/runtime/core/include/config/ConfigManager.h`](../../source/runtime/core/include/config/ConfigManager.h)
- [`tools/threading/README.md`](../../tools/threading/README.md)
- [`tools/threading/run_matrix.py`](../../tools/threading/run_matrix.py)
- [`tools/threading/runtime_verify.py`](../../tools/threading/runtime_verify.py)
- [`tools/threading/compare_captures.py`](../../tools/threading/compare_captures.py)

---

## 12. 阶段开发、双仓库记录与提交标准

每个新阶段必须执行：

1. 在 MoerEngine 和 TechRecord 分别执行 `git status`，保护两个仓库的用户现有变更。
2. 写出该阶段目标、触点、风险和验收标准。
3. 小步编辑，不混入无关重构。
4. 优先 `just b`，否则使用等价 CMake 构建。
5. 运行受影响 renderer；线程、资源生命周期或 runtime 行为变化必须运行 Editor。
6. 至少覆盖 feature off/on、Raster/Raytracing 相关组合、lag 0/1、窗口 resize/restore 和正常退出。
7. 扫描 assertion、VUID、device lost、submit failure 和资源残留。
8. 视觉检查主窗口和平台 viewport，不能只依赖 exit code。
9. 运行 `git diff --check` 和脚本语法检查。
10. 在 MoerEngine 只暂存自己的功能和验证文件，创建中文语义清楚的阶段提交。
11. 更新本文件“进度追加区”，记录 MoerEngine commit、构建、运行证据、TechRecord 路径和下一步。
12. 在 TechRecord 新增或更新阶段实施记录，并更新 `00_调研与初步方向.md`；发现 bug/regression 时同步更新 `30_问题复盘/BugFix/`。
13. TechRecord 记录必须引用准确的 MoerEngine commit，并包含架构决策、验证证据、性能观察、已知限制和剩余 TODO。
14. 在 TechRecord 只暂存本阶段自己的文档，执行 `git diff --cached --check`，提交并推送 TechRecord 当前分支。
15. 推送 MoerEngine 当前 feature branch，不直接推送 MoerEngine main。
16. 用远端 hash 核对两个仓库均已推送；只有两个仓库都完成远端交付，阶段才算完整完成。

禁止事项：

- 不要提交 `target/`、`.codex_tmp/`、shader cache、Crash Diagnostic dump 或本地截图。
- 不要删除同步/bypass 回退路径。
- 不要同时实现 Device fault、RenderGraph 和 parallel recording。
- 不要在没有验证的情况下增大 `max_frame_lag`。
- 不要让 RHI submission worker 承担阻塞式 GPU completion。
- 不要把 raw pointer 扫描包装成通用资源保活。
- 不要恢复 `_timeline % 3` descriptor 选槽。
- 不要因为最后出现 Present 断言就把交换链当作首故障。
- 不要用本文件的“进度追加区”代替 TechRecord 正式实施记录和 BugFix 复盘。
- 不要把 TechRecord 中其他人的索引、assets 或研究资料顺带暂存到当前阶段提交。

---

## 13. 新 Agent 第一次回复模板

第一次接手时，回复至少包含以下内容：

```text
1. 已确认 MoerEngine 与 TechRecord 两个仓库、各自分支和远端同步状态。
2. 已确认 MoerEngine base commit、Phase 5 历史冻结点、Phase 8 当前功能完成点 `fd59508c`，以及 TechRecord Phase 8 实施记录和 BugFix 复盘存在。
3. 两个工作区是否干净；若不干净，哪些属于用户变更。
4. Phase 0-8 已完成目标的简要确认。
5. 尚未完成：Clang/Ninja/Release 与其他 GPU 交叉验证、真实 mid-flight fault/device-surface recovery、Ray snapshot cache、物理 transient/culling/barrier handoff、并行录制、更多 queue/backend。
6. 本机将使用的编译器、构建配置和 GPU/driver。
7. 先执行 build + TestRenderGraph + linear/graph smoke 基线，并保留独立 sync/fault seam；基线通过后再研究 Phase 9 的资源版本、barrier handoff 和 recorder ownership。
8. 已确认每个阶段都要同步更新、提交和推送 TechRecord，这是必选 Definition of Done。
9. 如果用户尚未明确授权继续开发，在这里停止并等待，不修改功能代码。
```

---

## 14. 进度追加区

后续 Agent 在每个阶段完成后，按以下格式追加，不要重写历史状态：

```markdown
### YYYY-MM-DD：Phase X 名称

- 目标：
- 主要改动：
- MoerEngine 提交：`<hash> <message>`
- 构建：
- 运行矩阵：
- 视觉结论：
- 错误扫描：
- TechRecord 实施记录：`<path>`
- TechRecord 问题复盘：`<path 或 无>`
- TechRecord 提交：`<hash> <message>`
- 双仓库推送：
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
- TechRecord 实施记录：`20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/10_Phase5稳定性回归与线程性能基线.md`。
- TechRecord 问题复盘：`30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase5_描述符环复用与Present二次断言复盘.md`。
- TechRecord 提交：`71b10ed docs(threading): 记录 Phase 5 稳定性与性能基线`，已同步到 `origin/main`。
- 默认配置：同步 Raster。
- 当前状态：停止继续开发，等待换机后先执行 Phase 6.0 基线复现。
- 下一步：基线通过并获得用户授权后，独立实施 Phase 6.1 device-fault latch；不要直接跳到 RenderGraph。

### 2026-07-16：Phase 6.0 换机基线复现与接管回归

- 目标：在新机器复现 Phase 5 构建、运行矩阵与视觉基线，修复换机暴露的回归，再进入新的功能阶段。
- 主要改动：render-finished binary semaphore 改为按实际 acquired swapchain image index 配对；copied ImGui ready marker 移到 draw packet 被渲染消费之后；第一启动 dock 目标更新为 `Game`；矩阵默认视觉稳定等待改为 `12 s`。
- MoerEngine 提交：`402b7c66 fix(vulkan): 修复换机线程基线回归`。
- 构建：`cmake --build build --config Debug --target MoerEditor --parallel 4` PASS；脚本 `py_compile` PASS。当前机器没有可用的 `just`、Ninja/Clang，因此本阶段是 Visual Studio 2022 / MSVC Debug 基线。
- 运行矩阵：`target/validation/rt_rhi/phase6_0_takeover_full_final_20260716`，`9/9 PASS`，总耗时 `264.9 s`，九个进程均正常退出；最低非黑像素比例 `0.9475`。
- 视觉结论：Raster/Raytracing Sponza 在最大化、resize、restore 与键盘输入后均正确；`raster_rt1_rhi_off` 默认 `12 s` 定向复测非黑比例 `0.9676`，此前暗帧是 ProbeGI 初始化期采样。
- 错误扫描：assertion、VUID、device lost、queue submission failure、access violation 与 tracked resource residue 均为 0；不再出现 `VUID-vkQueueSubmit2-semaphore-03868`。
- 配置保护：本机根 `MoerEngine.toml` 未覆盖；矩阵使用 tracked `template.MoerEngine.toml` 生成独立场景配置。
- TechRecord 实施记录：`20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/11_Phase6.0换机基线复现与接管回归.md`。
- TechRecord 问题复盘：`30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase6.0_换机基线与Swapchain信号量复用复盘.md`。
- TechRecord 提交：`845f73e docs(threading): 记录 Phase 6.0 换机基线`。
- 双仓库推送：MoerEngine `origin/feature/rt-rhi-threading` 已包含 `402b7c660a803405c92fae1d42d785a8cde5e623`；TechRecord `origin/main=845f73e6e0b8db697fbd10bc337a95f71ac50382`。
- 已知限制：尚未覆盖 Clang/Ninja、Release、其他 GPU/driver，也未做故意 device-lost 注入；`12 s` 仅是当前 Sponza + ProbeGI 的自动截图稳态等待，不是引擎同步协议。
- 下一步：独立实施 Phase 6.1 device-fault latch，优先完成 first-fault record、typed submit/acquire/present outcome、CPU settled timeline 与失败资源隔离；不要同阶段混入 PrepareFrame、RenderGraph 或 parallel recording。

### 2026-07-16：Phase 6.1 Device-fault latch 与失败传播

- 目标：首个 Vulkan terminal failure 后只发布一次完整诊断，拒绝后续 native submit/present，失败完成 CPU 依赖，隔离未证明 GPU completion 的资源，并让 Sync/shutdown 有界退出。
- 主要改动：新增 `VulkanFault.h` 与 device first-fault 状态机；Submit/Present/Acquire typed outcome；native admission gate；submission-acceptance handshake；独立 `cpu_settled_frame`；success-only callback；allocator/presentor quarantine；command-pool reset skip；queued-work cancel/drain；严格 fault injection 与验证断言。
- Swapchain hardening：recreate 改为临时资源事务；按规范处理 oldSwapchain 即使 create 失败也已 retired；`VK_INCOMPLETE` 有界重试；Acquire 仅在成功状态输出 image；present fence 精确槽位复用；`SURFACE_LOST` 因无 surface reconstruction 协议而 fail-closed terminal。
- MoerEngine 提交：`8afa2cac feat(vulkan): 完成 DeviceFault 锁存与失败传播`。
- 构建：`cmake --build build --config Debug --target MoerEditor --parallel 4` PASS，仅保留既有 C4244/C4715 warning；`py_compile` 与 `git diff --check` PASS；非法计数和重复 fault 参数均以 exit code `2` 拒绝。
- 故障注入：`target/validation/rt_rhi/phase6_1_fault_final3`，`1/1 PASS`，Injection/First/Summary 各一次；First=`PresentSubmit/VK_ERROR_DEVICE_LOST/timeline=17/work_serial=17/injected=true/predrained=true`；Summary 证明 post-fault native submit/present=`0/0`、rejected=`148/49`、quarantine=`2`、queue Sync=`3/3`。
- 正常回归：`phase6_1_smoke_final3` 为 `3/3 PASS`；显式 `VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` 的 `phase6_1_syncval_final` 为 `1/1 PASS`；`phase6_1_full_final3` 为 `9/9 PASS`，总耗时 `263.9 s`，九个进程正常退出，最低非黑比例 `0.9477`。
- 视觉结论：Raster/Raytracing Sponza 在最大化、resize、restore 与键盘输入后均正确；fault 帧非黑比例 `0.8916`，一秒 Raytracing 累积噪声符合注入时机，没有黑屏或资源破坏迹象。
- 错误扫描：正常矩阵 assertion、VUID、unexpected device lost、queue submission failure、access violation 与 tracked resource residue 均为 0；fault 集仅豁免完全锚定且字段正确的唯一 First error 行，没有 Present/command-pool 级联噪声。
- 配置保护：本机根 `MoerEngine.toml` 未覆盖；所有矩阵使用 tracked `template.MoerEngine.toml` 生成独立场景配置。
- TechRecord 实施记录：`20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/12_Phase6.1_DeviceFault锁存与失败传播.md`。
- TechRecord 问题复盘：`30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase6.1_DeviceFault锁存与Swapchain生命周期复盘.md`。
- TechRecord 提交：`83bedaf docs(threading): 记录 Phase 6.1 故障锁存`。
- 双仓库推送：MoerEngine `origin/feature/rt-rhi-threading` 包含 `8afa2cac` 及本交接更新；TechRecord `origin/main` 包含 `83bedaf`。
- 已知限制：synthetic seam 在注入前预排空，只证明失败传播，不等同于真实 mid-flight TDR；尚未覆盖 Execute/Copy/external signal 等其他 fault entrypoint、device/surface recreation、Clang/Ninja、Release、其他 GPU/driver 或 D3D12。
- 下一步：进入 Phase 7 PrepareFrame 性能归因。先拆稳定子指标并采集至少三轮重复样本，再做单一、低风险优化；不要同时启动 RenderGraph 或 parallel recording。

### 2026-07-16：Phase 7 PrepareFrame 性能归因与低风险优化

- 目标：在不破坏 GT/RT/RHI 所有权边界的前提下，把 Raster/Raytracing `PrepareFrame` 拆成可重复归因的稳定阶段，并只优化一个被数据证明的来源。
- 指标：新增独立 `[ThreadingProfile][Prepare]`，覆盖同步与 RT 模式；拆分 window、scripting/test/UI hook、camera/test、config snapshot、scene update、Ray scene snapshot、UI composition/draw packet 和 other，同时记录 scene dirty、GPU update、geometry/snapshot build 与 UI workload。
- 测量隔离：profile state 只在 `profile_logging=true` 时单独分配，默认关闭不取时钟、不加锁、不格式化、不分配，并避免与 RT frame state cache-line 伪共享；`phase7_profile_off` 为 `1/1 PASS` 且无 Prepare marker。
- 工具口径：`run_matrix.py` 解析/聚合 Prepare 尾窗口并输出 JSON/Markdown；Prepare 使用自己的 `samples`。旧 RT 日志补 `prepare_samples/gt_wait_samples`，Prepare/Render/GT wait 各自按正确分母跨窗口加权；短 fault 场景不强制等待一秒 Prepare marker。
- 优化前基线：`target/validation/rt_rhi/phase7_prepare_baseline` 为 `12/12 PASS`。三轮中位数 Raster sync/RT1 为 `0.554/0.526 ms`；Ray RT0/RT1 为 `25.170/25.180 ms`，其中 scene snapshot 约 `17 ms`、scene update 约 `7.6~7.7 ms`。
- 根因：Ray 每帧无条件 Patch 同一主方向光与节点；`Scene::Patch` 无条件 MarkDirty，使每个 steady frame 都生成 Logical/CpuScene 更新、GPU update 与 geometry snapshot。
- 单一优化：每帧仍从当前 Ray config 计算目标 Sun 值，但比较 Scene 中实际 `CLightDirectional/CNode` 后只在真实变化时 Patch。没有只缓存上一帧 config，因此外部场景修改仍会在下一帧被恢复；所有比较仍在 GT。
- 优化后对照：`target/validation/rt_rhi/phase7_prepare_optimized` 为 `6/6 PASS`。RT0/RT1 三轮中位数降至 `17.890/18.013 ms`，分别下降约 `28.9%/28.5%`；scene update 降至 `0.037/0.035 ms`，steady dirty/update GPU/geometry 比例从 `100%` 降至 `0%`。
- 剩余瓶颈：Ray scene snapshot 仍每帧构建，约 `17.1~17.8 ms`，成为最大阶段；其缓存/增量 invalidation 留给独立性能阶段，不混入 Phase 8。
- MoerEngine 提交：`db76cfd6 perf(threading): 归因并优化 PrepareFrame`。
- 构建与脚本：最终 `cmake --build build --config Debug --target MoerEditor --parallel 4`、`py_compile`、新旧聚合合成断言与 `git diff --check` 均 PASS。
- 回归：最终 smoke `phase7_smoke_final` 为 `3/3 PASS`；显式 sync validation `phase7_syncval` 为 `1/1 PASS`；fault `phase7_fault` 为 `1/1 PASS`；full `phase7_full` 为 `9/9 PASS`、`262.4 s`、九个进程正常退出、最低非黑 `0.9475`。
- 视觉结论：Raster/Raytracing Sponza 在最大化、resize、restore 与键盘输入后均正确，无黑屏、陈旧 framebuffer 或 swapchain 停滞；正常日志未匹配 assertion、VUID、unexpected device lost、submit failure、access violation 或 tracked resource residue。
- 配置保护：根 `MoerEngine.toml` 未覆盖；所有矩阵继续从 tracked `template.MoerEngine.toml` 生成独立配置。
- TechRecord 实施记录：`20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/13_Phase7_PrepareFrame性能归因与低风险优化.md`。
- TechRecord 问题复盘：`30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase7_无变化SunPatch与PrepareFrame更新放大复盘.md`。
- TechRecord 提交：`0f30519 docs(threading): 记录 Phase 7 PrepareFrame 优化`。
- 已知限制：固定场景已覆盖稳定值跳过路径，但尚无自动化用例修改 `sun_direction/exposure` 后断言“恰好一帧 dirty”；尚未覆盖 Clang/Ninja、Release、其他 GPU/driver 或 D3D12。
- 下一步：进入 Phase 8 Raster RenderGraph 最小串行骨架，先建立 pass/resource declaration、旧路径回退与 off/on 对照；不要同时引入 snapshot cache 或 parallel recording。

### 2026-07-16：Phase 8 Raster RenderGraph 最小串行骨架

- 目标：把现有 Raster 线性 pass 迁入显式 graph declaration/compile，同时保持稳定串行行为、现有 RHI barrier owner 和可配置 linear 回退。
- 主要改动：重写 `RenderGraph.h/.cpp`；支持 import alias、logical transient、export、Read/Write/ReadWrite、RAW/WAR/WAW、显式/serial edge、稳定拓扑、one-shot execute 和确定性 dump；Raster linear/graph 共用 `define_raster_passes(schedule)`；删除旧死 graph 抽象。
- 所有权决策：RenderGraph 不生成 barrier、不修改 tracked state；command reorder/backend preprocess 仍是唯一真实 barrier owner。默认 `render_graph=false`。
- 物理身份修复：DepthBuffer wrapper 与内部 Texture 地址不同；现统一 canonicalize 到 `TextureView::GetTexture()`，并在 alias invariant 失败时于首个 callback 前 latch linear。最终 dump 证明 `depth_nearest_sampler/selected_framebuffer` alias depth，且 `Geometry -> UiCombine [RAW:depth]`。
- MoerEngine 提交：`fd59508c feat(rendergraph): 建立 Raster 最小串行骨架`。
- 构建：`cmake --build build --config Debug --target TestRenderGraph MoerEditor --parallel 4` PASS；`TestRenderGraph.exe`、`py_compile`、`git diff --check` PASS。
- 运行矩阵：linear/full `9/9 PASS`（`267.8 s`）；最新 graph `7/7 PASS`（`223.2 s`）；关键资源 `8/8 PASS`；显式 synchronization validation `1/1 PASS`；fault `1/1 PASS`。
- 生命周期：Raster→Raster reload→Raytracing→Raster 正常 self-exit；精确 4 次 queue drain、4 次 renderer destroy 包装、Raster 3 次、Raytracing 1 次析构完成。
- 视觉结论：最终 Sponza graph-off/on MAE `0.6055`、RMSE `1.7544`、p99 `8`、RGB<=2 ratio `0.8831`，通过阈值；BaseColor/Normal/LinearDepth 四组配对全部 PASS，人工检查无黑屏、陈旧 framebuffer 或几何错位。
- 错误扫描：正常 graph/sync 场景 assertion、VUID、SYNC-HAZARD、unexpected device lost、submit failure、access violation、tracked resource residue 和 graph fallback 均为 0；fault 仅保留严格锚定的唯一 First error。
- 配置保护：根 `MoerEngine.toml` 的开发者 Raytracing 设置未覆盖；矩阵继续从 tracked template 生成隔离配置。
- TechRecord 实施记录：`20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/14_Phase8_RasterRenderGraph最小串行骨架.md`。
- TechRecord 问题复盘：`30_问题复盘/BugFix/MoerEngine_RT_RHI_Phase8_DepthBuffer物理身份与Graph依赖断链复盘.md`。
- TechRecord 提交：`4474339 docs(threading): 记录 Phase 8 RenderGraph 骨架`。
- 双仓库推送：MoerEngine `origin/feature/rt-rhi-threading` 包含 `fd59508c` 及本交接更新；TechRecord `origin/main=4474339`。
- 已知限制：无 pass culling、物理 transient allocator/alias reuse、subresource barrier generation 或并行录制；`processing_image` token 与 HiZ CPU swap 尚不能表达安全重排所需的物理版本；只覆盖 RTX 5080 + MSVC Debug。
- 下一步：Phase 9 先研究并固化 physical resource version、barrier handoff、每 recorder command pool/allocator/descriptor arena、secondary/primary 合并和部分执行失败语义；不得直接删除 Phase 8 serial edge。

### 2026-07-16：Phase 9 并行命令录制前置设计与可行性

- 目标：在不修改功能源码、不删除 Phase 8 `serial` policy 的前提下，审计 RenderGraph、RHI command packet、Vulkan recorder/descriptor/tracker、TaskGraph join 和 completion retirement，并形成可实施的 Stop/Go 方案。
- 只读结论：当前串行 allocator/pool 所有权合法，但 graph 只有 pointer identity/字符串 range，Raster callback 共享 `cmd_list/context`，`processing_image` 与 HiZ host swap 未物理化；Vulkan bind 会改写全局 descriptor cursor 和共享 PSO binder，多个 tracker 也无法独立推进全局 layout。
- 当前决策：`STOP -> conditional GO`。不能直接并行 graph callback 或现有 visitor；先完成 typed physical version/subresource、immutable `RasterFramePlan`/segments、recorder-local descriptor/query lease、typed `RecordOutcome` 和整批 timeline retirement。
- 首版推荐：`CmdReorderer layer + ordered multiple primary command buffers`。Graph callback 保持串行；每 worker 独占 pool/CB/arena，唯一 RHI coordinator 录制 barrier primary，并以稳定数组顺序一次 `vkQueueSubmit2`；secondary + dynamic rendering inheritance 后置。
- 实施门禁：Phase 9.0 串行测量/golden -> 9.1 物理版本与 FramePlan -> 9.2 合成 recorder substrate -> 由三轮数据决定是否在 9.3 接入 `HiZBuild + DirectionalShadowMask` 条件白名单。parallel 默认关闭，linear、RHI off/bypass 和串行 recorder 全部保留。
- MoerEngine 功能源码：无改动；功能冻结点仍为 `fd59508c`。本阶段是 docs-only 研究交付，因此未重复执行 Phase 8 构建与运行矩阵。
- TechRecord 研究记录：`20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/15_Phase9_并行命令录制前置设计与可行性.md`。
- TechRecord 提交：`9e91d56 docs(threading): 规划 Phase 9 并行命令录制`；方案收敛提交：`20ea618 docs(threading): 收敛 Phase 9 recorder 边界`，`origin/main=20ea618`。
- 下一步：等待用户确认；确认后必须使用 `implement-moer-feature` 工作流从 Phase 9.0 开始，小步构建、运行、观察与阶段提交。若 eligible recording 收益空间不足，则停在诊断/契约阶段。

### 2026-07-16：Phase 9.0 串行录制测量与契约冻结

- 目标：只在现有 Vulkan 串行 recorder 上测量真实 CPU 录制成本并冻结可自动验证的 serial golden；用 Release 数据决定是否值得进入 Phase 9.1/9.2，不实现真实并行 command buffer，也不删除 Phase 8 serial edge。
- 主要改动：为全部 `21` 种 RHI command 建立显式 capability 表，当前全部保持 `SerialOnly`；新增 dedicated `ExternalCpuJoinPool`、逐 command/layer 诊断与保守收益预测、来自实际提交录制点的 command/layer/barrier/descriptor/query semantic golden、fail-closed unresolved/opaque 统计和 opt-in strict verifier；同时修复 reorderer 的嵌套 scope、subresource range、hazard/layer、barrier 清理和 descriptor 边界等串行契约问题。External join 仅是经过单测的后续基础设施，本阶段没有接入真实 recorder worker。
- MoerEngine 功能提交：`ec0dfdf4f6bae3fef6c0b517cda63b35f86fed42`（`feat(threading): 冻结 Phase 9.0 串行录制契约`），已推送到 `origin/feature/rt-rhi-threading`。
- 构建：本机无 `just`，等价执行 Debug `cmake --build build --config Debug --target TestExternalCpuJoin TestRHIRecordDiagnostics TestRHICmdReorderer TestRenderGraph TestTaskGraph MoerEditor --parallel 8` 和 Release `cmake --build build --config Release --target MoerEditor --parallel 8`，均 PASS；`py_compile`、runner dry-run/list、strict 合成断言和 `git diff --check` 均 PASS。
- 定向测试：`TestExternalCpuJoin`、`TestRHIRecordDiagnostics`、`TestRHICmdReorderer`、`TestRenderGraph`、`TestTaskGraph` 全部 PASS，覆盖 join 重入/异常/drain/reuse、21 类 capability、golden 稳定性与 reorderer hazard/barrier 契约。
- Debug 回归：full `9/9 PASS`、RenderGraph `7/7 PASS`、关键资源 `8/8 PASS`、fault `1/1 PASS`、显式 synchronization validation `1/1 PASS`；最终 threaded profile-off `target/validation/rt_rhi/phase9_0/debug_threaded_profile_off` 为 `1/1 PASS`，且 `[ThreadingProfile]` 匹配为 `0`，证明 profile-off 仍走无新增诊断时钟、哈希、分配、格式化和输出的串行路径。
- 最终 Release Gate：固定 Sponza Raster、RT/RHI thread on、lag 1、每次取最后 `5` 个一秒稳态 tail window；`target/validation/rt_rhi/phase9_0/release_linear_timingfix` 与 `target/validation/rt_rhi/phase9_0/release_graph_timingfix` 各独立运行 `3` 次，六轮全部 PASS。共 `1975/1975` 个所选 tail submission golden complete，incomplete/unresolved/opaque 均为 `0`；每次为 `51` layers、`64` commands、`45` 个 measurement candidates、`0` 个 safe candidates，仅 `1` 层具并行形状且只有 `2` 个 candidate。
- 性能结论：六轮 eligible work 仅 `0.0128~0.0156 ms`，约为 recorder wall 的 `6.7%~7.4%`，远低于 `0.2 ms` 和 `15%` 双门槛；加入保守 join 成本后 projected net 全部为负，因此结论为 **STOP**，不是进入真实 parallel CB 的正确性阻塞。
- Golden 摘要：Command=`1ace03aa109a61b8`、Layer=`c611607ad03e3ff7`、Barrier=`97cc4f20495e0b46`、Descriptor=`a466bd73913bcc89`、Query=`99ddfe3a44fd2539`、Combined=`4069ad6829e2562b`、Manifest=`0951b1e00ae81063`；同一 scenario 的 repeat 与每次所选 tail-window manifest 均稳定，linear/graph 之间也经显式对照一致。
- 视觉与错误扫描：配对资源中 BaseColor 与 LinearDepth exact，Normal MAE=`0.106220`、RMSE=`1.35616`、p99=`6`、RGB<=2 ratio=`0.968126`，通过阈值；正常矩阵未发现 assertion、VUID、SYNC-HAZARD、unexpected device lost、submit failure 或 tracked-resource residue。根 `MoerEngine.toml` 的开发者 Raytracing 配置未覆盖，所有运行均使用隔离生成配置。
- TechRecord 实施记录：`20_技术文档/引擎架构/MoerEngine/RT_RHI_Threading/16_Phase9.0串行录制测量与契约冻结.md`，并同步更新 `00_调研与初步方向.md`。
- TechRecord 提交：`9ba9ff9a9b9840ceb0c258b4560dbc321562db07`（`docs(threading): 记录 Phase 9.0 串行录制契约`），已推送到 `origin/main`。
- 已知限制：`1975/1975` 完整性只适用于固定 Raster 场景所选的最后五个稳态 tail window；启动/过渡 submission 与 Raytracing/Custom 路径仍可能按设计 fail closed，未宣称全路径 100% complete；尚未做其他 GPU/driver/backend 的交叉验收。
- 下一步：保持 **STOP**。不要进入 Phase 9.1/9.2，不要实现真实 parallel CB，也绝不能删除 Phase 8 serial edge。只有工作负载实质变化、重新采集至少三轮 Release 数据并同时超过 `0.2 ms` eligible work 与 `15%` recorder wall 两个门槛后，才重新评估后续阶段。
