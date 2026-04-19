# RHI 提交模式重构设计

## 1. 目标

本次重构目标是把 RHI 提交流程收敛为一条清晰的主链路：

- 调用侧只通过 `RHIExecutor::Get().Submit(...)` / `RHIExecutor::Get().Sync(...)` 与 executor 交互
- `RHIExecutor` 统一负责 preprocess、translate task、submit runtime、interrupt runtime
- `CopyScope` 由 Graphics / Compute `CommandList` 创建；Copy queue 对调用方完全隐式
- `Transition` 显式描述资源在 command list 内部以及跨队列之间的状态切换
- `SubmitInfo` 只服务 submit runtime，不承担 translate 过程语义
- `wait/signal` 的物理 timeline value 延迟到 submit runtime 解析
- 整体执行链固定为：
  - `preprocess -> translate task -> submit assemble -> submission runtime -> interrupt runtime`

约束遵循 [Rule_Codex.md](/f:/Github_Data/MoerEngine/docs/Rule_Codex.md)：

- 不要兜底/兼容实现
- 不要过度设计
- 基于 Test 验证
- 过滤 validation，只关注 error

## 2. 外部接口

### 2.1 `RHIExecutor::Submit` / `Sync`

新的提交入口：

```cpp
enum class ERHIExecSubmitFlags : uint8 {
    None     = 0,
    FlushGPU = 1 << 0,
    FrameEnd = 1 << 1,
};

struct RHIPresentRequest {
    SwapchainRef swapchain;
    TextureView  source;
};

void Submit(
    Array<CommandList&&> command_lists,
    ERHIExecSubmitFlags  flags   = ERHIExecSubmitFlags::FlushGPU,
    RHIPresentRequest*   present = nullptr
);

enum class ERHISyncDepth : uint8 {
    RHI     = 0,
    Present = 1,
};

GraphEventRef Sync(ERHISyncDepth depth = ERHISyncDepth::RHI);
```

语义：

- `None`
  - 仅缓存到 pending batch，不触发 GPU flush
- `FlushGPU`
  - 触发本次 batch 的 preprocess / translate / submit
- `FrameEnd`
  - 标记本次 batch 为 frame tail，触发 interrupt 线程执行帧末回调（帧内资源刷新 / defrag 等）
  - `FrameEnd` 必须与 `FlushGPU` 同时使用，单独使用 `FrameEnd` 是非法的
- `present`
  - 与 command list 同级，由 executor 挂到最后一个 graphics `SubmitInfo` 的尾部阶段处理
- `Sync(RHI)`
  - 返回一个 `GraphEventRef`
  - 表示调用前所有已进入 `RHIExecutor` 的 RHI submit 在 interrupt runtime 中已退休完成
  - 不包含 present completion
- `Sync(Present)`
  - 返回一个 `GraphEventRef`
  - 表示调用前所有已进入 `RHIExecutor` 的 RHI submit 与 present 都已在 interrupt runtime 中退休完成
- 所有提交入口统一经过 `RHIExecutor`，不再允许直接调用 `gfx_queue.Execute` / `gfx_queue.Present`

### 2.2 `CommandList`

`CommandList` 在构造时绑定 `EQueueType`，只允许 `Graphics` 或 `Compute`：

```cpp
explicit CommandList(EQueueType queue_type);
EQueueType GetQueueType() const;
GraphEventRef ReadbackCopy(BufferView src, std::span<byte> dst);
GraphEventRef ReadbackCopy(TextureView src, std::span<byte> dst);
```

`EQueueType` 对调用方可见的合法值：

```cpp
enum class EQueueType : uint8 {
    Graphics = 0,
    Compute  = 1,
    // Copy 不对外暴露，由 executor 从 CopyScope 内部派生
};
```

约束：

- 一个 `CommandList` 只能绑定 `Graphics` 或 `Compute`，不允许直接构造 Copy queue command list
- Copy queue 对调用方完全隐式：调用方通过 `BeginCopyScope()` 嵌入 copy 操作，executor 在 preprocess / translate / submit 阶段自动处理 copy queue 的 command buffer 录制、allocator 分配和 signal/wait（见 §2.3、§5）
- 提交时 executor 按 `GetQueueType()` 决定主 native queue，不再需要外部 wrapper 指定
- Graphics / Compute 的普通命令在对应 queue 上 translate 和 submit
- 显式 readback API 返回 `GraphEventRef`，该事件表示 interrupt runtime 已完成 staging 数据回填

### 2.3 `CopyScope`

`CopyScope` 由 Graphics / Compute `CommandList` 创建，用于在其内部嵌入 copy 操作。Copy queue 对调用方完全不可见，copy 相关的 command buffer 录制、allocator 分配、barrier 生成、signal/wait 全部由 executor 内部处理：

```cpp
class CopyCommandScope {
public:
    void UploadBuffer(...);
    void UploadTexture(...);
    void CopyBuffer(...);
    void CopyTexture(...);
    GraphEventRef ReadbackBuffer(...);
    GraphEventRef ReadbackTexture(...);
    // ~CopyCommandScope() 隐式结束 scope
};

// CommandList 成员（仅 Graphics / Compute CommandList 可调用）：
CopyCommandScope BeginCopyScope();
```

约束：

- 只有 `Graphics` / `Compute` `CommandList` 可以调用 `BeginCopyScope()`
- `CopyCommandScope` 内只允许录制 copy / upload / readback 指令
- 一个 `CommandList` 内可以有多个 `CopyCommandScope`，不允许嵌套
- `CopyScope` 是 executor 内部派生 copy queue submit 的唯一结构化来源
- executor 在 preprocess 阶段从 `CopyScope` 构造 copy queue 的 handoff plan；在 translate 阶段为 `CopyScope` 独立分配 copy queue allocator 并录制 copy queue command buffer；在 submit 阶段自动插入父 queue 和 copy queue 之间的 signal/wait
- readback API 返回的 `GraphEventRef` 在 interrupt runtime 中、且 staging 数据回填到 CPU 目标地址之后才触发

典型 pattern：

```
gfx -> copy -> gfx:
    CommandList(Graphics)
        [普通 Gfx 命令]
        BeginCopyScope()      ← executor 推导: gfx release → copy acquire
            UploadBuffer(...)
        ~CopyCommandScope()   ← executor 推导: copy release → gfx acquire
        [后续 Gfx 命令]

submit 阶段实际产生的 native submit 序列:
    1. gfx submit (前段命令)  → signal gfx_timeline @ N
    2. copy submit            → wait gfx_timeline @ N
                              → signal copy_timeline @ M
    3. gfx submit (后段命令)  → wait copy_timeline @ M

compute -> copy -> compute:
    CommandList(Compute)
        [普通 Compute 命令]
        BeginCopyScope()
            CopyTexture(...)
        ~CopyCommandScope()
        [后续 Compute 命令]
```

### 2.4 `Transition`

`Transition` 必须显式携带 `SrcState`、`DstState`、`SubresourceRange`：

```cpp
struct RHISubresourceRange {
    uint8 mip_level   = 0;
    uint8 mip_count   = kRemainingSubresource;
    uint8 array_layer = 0;
    uint8 array_count = kRemainingSubresource;
};

enum class ERHITransitionFlags : uint8 {
    None    = 0,
    Acquire = 1 << 0,
    Release = 1 << 1,
};

void Transition(
    BufferView view,
    EBufferState src,
    EBufferState dst,
    ERHITransitionFlags flags = ERHITransitionFlags::None,
    EQueueType other_queue = EQueueType::Ignore
);

void Transition(
    TextureView view,
    ETextureState src,
    ETextureState dst,
    RHISubresourceRange range = {},
    ERHITransitionFlags flags = ERHITransitionFlags::None,
    EQueueType other_queue = EQueueType::Ignore
);
```

语义：

- `flags == None`
  - 同队列 transition
  - 只在当前 command list 内部生成局部 barrier
- `flags == Release`
  - 当前 queue 向 `other_queue` 放权
  - 必须在 command list 末尾前完成，不允许在 Release 之后录制访问同资源的命令
- `flags == Acquire`
  - 当前 queue 从 `other_queue` 接权
  - 必须在 command list 开头或确保先于后续访问该资源的命令
- `Acquire | Release`
  - 非法

### 2.5 `BufferOverlap`

对于写-写不构成 hazard 的场景（如 append buffer、streaming write），可以用 overlap 开关跳过 write-after-write barrier：

```cpp
// 开启：后续写操作不对该 buffer 发 write-after-write barrier
void BeginBufferOverlap(BufferView view);

// 关闭：恢复正常 write-after-write barrier 推导
void EndBufferOverlap(BufferView view);
```

约束：

- `BeginBufferOverlap` 到 `EndBufferOverlap` 之间，translate 阶段对该 buffer 的 write-after-write 访问跳过发 barrier
- read-after-write 和 write-after-read 仍然正常发 barrier，不受 overlap 影响
- overlap 范围不跨 command list 边界，跨 command list 自动关闭
- 嵌套 `BeginBufferOverlap` 是非法的

## 3. 资源状态模型

### 3.1 长期状态存储

每个 `Buffer` / `Texture` 持有长期 `SubresourceStates`。

- Buffer v1：按整资源跟踪，不做 offset/size range 粒度
- Texture：按 `SubresourceRange` 跟踪

单个状态单元至少包含：

- `known` — 是否已有已知状态，新资源为 false
- `owner_queue` — 当前 ownership 所在的 queue
- `state` — 当前资源状态
- `last_access_kind`
  - `Read`
  - `Write`

### 3.2 `Unknown` 规则

- 新资源初始 `known = false`
- `src_state` 来自上一次 preprocess 写入的长期状态（跨帧信息通过此持久存储跨帧传递）
- 首次使用时 `known == false`，资源处于 `Unknown` 状态
- `Unknown` 由资源的 `usage` flags 决定 Vulkan 合法路径：
  - 在 Vulkan 实现中对应 `VK_IMAGE_LAYOUT_UNDEFINED` / 无已知 buffer 访问历史
  - translate 阶段遇到 `Unknown -> X` 时，按 Vulkan 合法的"无已知源状态"路径发 barrier（srcAccessMask = 0，oldLayout = UNDEFINED）

### 3.3 状态写入时机

资源状态只允许在 preprocess 阶段写入：

- preprocess 单线程
- translate 只读 preprocess 结果（通过 per-command-list 的 `seed_tracker` 拷贝，不共享可变状态）
- submit / interrupt 不修改全局资源状态

这保证：

- 资源状态推导是单一来源
- translate 可以安全并行
- 不需要额外锁保护资源状态

### 3.4 帧开始 / 帧结束约束

每帧的提交序列遵循以下强约束：

- 帧的第一个 command list 必须是 Graphics queue
- 帧的最后一个 command list（present 前）必须是 Graphics queue
- 所有在 Compute / Copy queue 上使用的资源，在该队列使用完毕后，必须在帧内将 ownership 转回 Graphics queue

即：帧结束时，所有资源的 `owner_queue` 必须是 `Graphics`（或 `Unknown`）。

违反此规则时 preprocess 报错（见 §6.4）。

示例：

```
资源 a, b:
    帧内顺序：gfx(read a) -> copy(write b) -> gfx(write a)
    copy 结束 -> 下一个 gfx 开始前，必须 acquire 资源 b
    前端 CopyScope 的 ~CopyCommandScope() 会自动生成 copy release / gfx acquire
    若使用显式 Transition，则必须在 gfx CommandList 头部显式 Transition(b, Acquire)
```

同样的约束对 Compute 也成立。

## 4. 同步来源

逻辑同步只来源于四类信息：

1. 用户显式录制的 wait/signal intent
2. `Transition(Acquire/Release)` 生成的跨队列 handoff plan
3. `CopyScope` 生成的跨队列 handoff plan
4. `FlushGPU` batch 对前序所有未退休 RHI submits 的 tail 依赖

本次设计引入 `SyncPoint`：

- 每个 `SubmitInfo` 默认导出一个 completion `SyncPoint`
- 后继 `SubmitInfo` 只记录“等待哪个 `SyncPointId`”，不记录具体 `{fence, value}`
- submit runtime 在真正提交时，才把 `SyncPointId` 解析成物理 `WaitEvent`

本次设计不再保留独立的 `ResolveDependencies` 阶段。

原因：

- 全局 hazard resolve 会阻塞 translate 并行
- preprocess 只需要保留逻辑依赖
- 真实 fence/timeline value 只有 submit runtime 才知道

### 4.1 Streaming SyncPoint Resolution

The runtime must not require full batch-wide submission context to resolve waits and signals.
Wait or signal resolution is a streaming operation owned by `SubmissionRuntime`.

Minimal model:

```cpp
using SyncPointId = uint64;

struct SubmitInfo {
    SubmissionKey key;
    EQueueType    queue;
    SyncPointId   signal_syncpoint;
    Array<SyncPointId> wait_syncpoints;
};

struct ResolvedSyncPoint {
    EQueueType    queue;
    uint64        timeline_handle;
    uint64        value;
};

struct QueueStreamState {
    EQueueType queue;
    uint64     next_signal_value;
    uint32     cursor;
};
```

Rules:

- Each `SubmitInfo` exports exactly one `signal_syncpoint`.
- A consumer submit stores only `wait_syncpoints`.
- `SubmissionRuntime` owns the only mutable `SyncPointId -> ResolvedSyncPoint` table.
- A syncpoint becomes resolved only after the producer submit has been emitted successfully.
- Signal values are allocated per queue and remain strictly monotonic.

Streaming resolve algorithm:

1. Build per-queue ordered submit streams.
2. Keep one cursor per queue stream.
3. Run a round-robin scheduler over queue heads.
4. For the current queue head, try to resolve every `wait_syncpoint` from the runtime table.
5. If any required syncpoint is still unresolved, skip this submit for now and poll the next queue head.
6. If all waits are resolved, normalize physical waits, allocate one new signal value on the target queue, emit the native submit, publish the resolved `signal_syncpoint`, and advance only that queue cursor.
7. If one full scheduler pass makes no progress while pending submits still remain, treat it as an internal dependency bug or cycle and fail fast.

Wait normalization:

- If multiple logical waits resolve to the same upstream queue timeline, keep only the maximum value.
- This preserves correctness because one queue timeline is linear, and waiting on the latest required value dominates earlier values on the same timeline.
- Cross-queue ordering is guaranteed only by explicit logical syncpoint dependencies. The runtime must not introduce extra global serialization across unrelated queues.

This model removes the need for a full pre-resolved dependency context at submit time. The runtime only needs:

- the current per-queue stream heads
- the resolved syncpoint table
- the per-queue next signal counter

It does not need a batch-global resolved wait map.

## 5. `CopyScope` 规则

`CopyScope` 是 executor 内部派生 copy queue submit 的唯一结构化来源。Copy queue 的全部生命周期（allocator、command buffer、timeline、barrier）均由 executor 内部管理，不对调用方暴露。

### 5.1 进入 `CopyScope`

preprocess 在 `BeginCopyScope()` 时：

- 若资源当前 `owner_queue != Copy`，记录当前 owner queue -> copy queue 的 release/acquire plan
- copy command list 头部需要的 acquire barrier 记录在 plan 中
- copy submit 需要等待的上游 completion 在 submit 阶段绑定真实 timeline value

### 5.2 离开 `CopyScope`

`~CopyCommandScope()` 时（或显式 End），preprocess 更新：

- 资源 `owner_queue = Copy`
- 资源 `state = copy final state`
- `last_access_kind` 根据 scope 内最后一次访问更新
- 记录 copy -> 父 CommandList queue 的 release/acquire plan（因为帧结束必须回到 Graphics，见 §3.4）

### 5.3 后续消费

CopyScope 结束后，父 CommandList 继续录制时视作已完成 acquire。translate 阶段：

- CopyScope 内命令独立录制到 copy queue native command buffer
- 父 CommandList 剩余命令录制到对应 queue native command buffer
- submit 时自动在 copy submit 和后续 submit 之间插入正确的 wait/signal

## 6. `Transition` 规则

### 6.1 同队列 `Transition(None)`

preprocess 行为：

- 校验 `src_state` 与本地 tracker 当前状态兼容
- 记录局部 transition 点
- 更新 command list 内部的 local state

translate 行为：

- 在对应位置生成局部 barrier
- 不生成 queue wait/signal

### 6.2 跨队列 `Release`

preprocess 行为：

- 校验资源当前 owner 是否为当前 queue
- 记录 release plan
- 将资源标记为"等待被目标 queue acquire"

translate 行为：

- 在 submit 尾部发 release barrier

submit 行为：

- 为该 handoff 分配 completion event（per-queue 递增 timeline value，见 §12）

### 6.3 跨队列 `Acquire`

preprocess 行为：

- 必须匹配先前 release
- 配对键为：
  - 资源
  - subresource range
  - src queue
  - dst queue
- 记录 acquire plan

translate 行为：

- 在 submit 头部发 acquire barrier

submit 行为：

- 解析 acquire 对应 release 的 completion event
- 追加 wait

### 6.4 非法情况

以下情况 preprocess 先 log 原因后 assert：

- 只有 acquire 没有 release
- 只有 release 没有 acquire
- `Acquire | Release` 同时存在
- range 重叠但 handoff 冲突
- `src_state` 与当前已知状态不兼容
- 帧结束时存在 `owner_queue != Graphics` 的资源（见 §3.4）
- `FrameEnd` 未与 `FlushGPU` 同时使用

报错格式：

```cpp
// 统一模式：先 log 详细原因，再 assert
LOG_ERROR("RHI Preprocess error: {reason}, resource={name}, queue={queue}, range={range}");
MOER_ASSERT(false);
```

## 7. 提交流水线

### 7.1 总体流程

```cpp
RHIExecutor::Submit(cmdlists, flags, present):
    assert(!HasFlag(flags, FrameEnd) || HasFlag(flags, FlushGPU))  // FrameEnd 必须伴随 FlushGPU

    lock(submit_mutex)
    pending.Append(std::move(cmdlists), flags, present)
    if (!HasFlag(flags, FlushGPU)):
        unlock
        return
    batch = MoveOut(pending)
    unlock

    translate_infos   = Preprocess(batch)
    translate_results = DispatchTranslateTasks(translate_infos)
    submit_infos      = AssembleSubmitInfos(translate_results, batch)
    SubmitRuntime.Enqueue(std::move(submit_infos), batch.frame_end)
```

### 7.2 Preprocess

职责：

- 单线程
- 读取各资源长期 `SubresourceStates`
- 构建 `TranslateInfo`
- 构建 `Transition(Acquire/Release)` 与 `CopyScope` 的逻辑 handoff plan
- 收集用户 wait/signal intent
- 推导并回写 `end_state_snapshot`
- 记录逻辑依赖，不生成物理 `WaitEvent`

伪代码：

```cpp
Preprocess(batch):
    for cmdlist in batch order:
        translate_info.seed_tracker = ReadSubresourceStates(cmdlist.resources)

        for command in cmdlist:
            if Transition:
                ValidateSrcDstRange(command)

                if flags == None:
                    translate_info.local_barriers += command
                    ApplyLocalState(command)

                else if flags == Release:
                    RecordReleasePlan(command)
                    ApplyReleaseState(command)

                else if flags == Acquire:
                    MatchAcquireWithRelease(command)
                    RecordAcquirePlan(command)
                    ApplyAcquireState(command)

            else if CopyScope:
                RecordCopyScopeHandoffPlan(command)
                ApplyCopyScopeState(command)

            else if UserWait/UserSignal:
                RecordUserSyncIntent(command)  // 只记录 intent，不绑定 timeline

            else if BufferOverlap:
                ApplyOverlapState(command)

            else:
                ApplyRegularResourceAccess(command)

        WriteBackSubresourceStates(local_state)

        if (batch.flags & FrameEnd && IsLastCmdList):
            ValidateAllResourcesOwnedByGraphics(local_state)

        outputs += translate_info

    if (batch.present):
        RecordPresentCandidate(batch.present)

    return translate_infos
```

### 7.3 TranslateTask

职责：

- 每个 `TranslateInfo` 对应一个 `TranslateTask`
- `TranslateTask.Dispatch()` 返回 `GraphEventRef`
- 任务内部录制 native cmd buffer / recorded submit payload
- 当前可同步执行，但接口保持 task 形状
- translate 只读 preprocess 输出，不修改全局资源状态

关于 `VulkanAllocator` 并发 pop：

- `LockFreeQueueBase<VulkanAllocator>` 的模板参数控制的是存储值类型（值 vs 指针），与线程安全无关
- translate task 需要并发从同一 queue 的 pool 中 pop allocator，包括 copy queue 的 allocator pool（由 executor 内部持有，调用方不可见）
- pool 实现必须支持并发 pop（lock-free 或加锁），translate 任务之间不得共用同一 allocator

伪代码：

```cpp
DispatchTranslateTasks(translate_infos):
    for translate_info in translate_infos:
        event = TranslateTask::Dispatch(translate_info)
        results += {translate_info.key, event, queue, recorded_submit_payload, metadata}
    return results
```

### 7.4 Submit 组装

职责：

- 只消费 `TranslateResult`
- 组装 submit runtime 真正需要的 `SubmitInfo`
- 在这里做同 queue 合包
- 在这里建立 `SubmitInfo` 之间的 `SyncPoint` 逻辑依赖
- `SubmitInfo` 不携带 translate-only 状态

伪代码：

```cpp
AssembleSubmitInfos(translate_results, batch):
    submit_infos = MergeByQueueSubmissionSemantic(translate_results)

    for submit_info in submit_infos:
        submit_info.signal_syncpoint = AllocateSyncPointId()
        submit_info.wait_syncpoints = BuildLogicalSyncWaits(submit_info)

    AttachBatchRootPrerequisites(submit_infos, previous_flush_rhi_tails)
    AttachPresentToLastGraphicsSubmitInfo(submit_infos, batch.present)

    return submit_infos
```

### 7.5 Submission Runtime

职责：

- 只消费 `SubmitInfo`
- 按 queue stream 流式提交，不做全量 submission context resolve
- 只在这里解析真实 `wait/signal` timeline value
- 保证每个 queue 的 signal value 连续递增
- 以 streaming 方式 resolve `SyncPoint`

原因：

- 先以稳定可跑通 validation 为目标
- `SyncPoint` 的物理值只有 submit runtime 才知道
- translate 已经并行化，submit 再激进并行收益有限但排障成本高

伪代码：

```cpp
SubmissionThreadLoop():
    while (running):
        made_progress = false

        for queue in RoundRobinQueues():
            submit_info = PeekQueueHead(queue)
            if (!submit_info):
                continue

            physical_waits = TryResolveSyncPoints(submit_info.wait_syncpoints)
            if (!physical_waits.ready):
                continue

            physical_waits = CollapseWaitsByTimelineMax(physical_waits)
            signal_value   = AllocateTimelineSignal(submit_info.queue)
            completion     = NativeSubmit(submit_info, physical_waits.values, signal_value)
            PublishResolvedSyncPoint(
                submit_info.signal_syncpoint,
                {submit_info.queue, QueueTimeline(submit_info.queue), signal_value}
            )
            PopQueueHead(queue)
            made_progress = true

            if (submit_info.present):
                present_completion = ExecutePresent(submit_info.present, completion)

            interrupt.Enqueue(RetireTaskFrom(submit_info, completion, present_completion))

        if (!made_progress && HasPendingSubmits()):
            FailFastOnBrokenSyncpointGraph()
```

### 7.6 Interrupt Runtime

职责：

- 处理 host wait
- 回收 allocator / presentor
- 执行 callback / signal
- 触发 readback `GraphEventRef`
- 触发 `RHIExecutor::Sync(depth)` 返回的 `GraphEventRef`
- 处理 frame-end marker

伪代码：

```cpp
InterruptThreadLoop():
    while (running):
        task = PopInterruptTask()

        WaitFenceValues(task.waits)
        RecycleAllocatorOrPresentor()
        RunCallbacks()
        NotifySignals()
        TriggerReadbackEvents()
        TriggerSyncEvents()

        if task is FrameEndMarker:
            TriggerFrameEndCallbacks()  // 资源刷新、defrag 等
            EmitFrameEndStats()
```

## 8. Present 规则

`Present` 与 command list 同级，但内部实现上挂到最后一个 graphics `SubmitInfo` 的尾部阶段。

不再允许调用方直接调用 `gfx_queue.Present(...)` / `gfx_queue.Execute(...)`。

### 8.1 Present 提交流程

```cpp
ExecutePresentTask(present_stage, rhi_completion):
    assert(present_stage.queue == Graphics)

    swapchain.WaitReusableSlot()
    acquire = swapchain.AcquireNextImage()
    if (!acquire.valid):
        return

    begin present cmd buffer
    tracker.ResetTransient()

    // source 的 ownership 此时必须已在 Graphics
    tracker.RecordRead(present_stage.source, TRANSFER)
    tracker.RecordWrite(acquire.swapchain_image, TRANSFER)
    EmitBarriers()

    CopyTexture(source -> swapchain_image)

    tracker.RecordState(acquire.swapchain_image, PRESENT_SRC_KHR)
    EmitBarriers()
    end cmd buffer

    native_queue.WaitAll(rhi_completion)        // 只等待本 SubmitInfo 的 RHI completion
    native_queue.Wait(acquire.ready_semaphore)  // binary semaphore（见 §8.2）
    native_queue.Signal(swapchain.render_finished_binary_semaphore)
    native_queue.Submit(present_cmd_buffer)
    vkQueuePresentKHR(...)
```

约束：

- `SubmitInfo` 的 completion `SyncPoint` 表示 RHI submit 完成，不包含 present retirement
- present completion 只参与 `Sync(Present)`，不参与 `Sync(RHI)`
- flush 间自动依赖只覆盖前序 RHI submits，不隐式覆盖 present

### 8.2 Vulkan Present 特殊约束

这是本次实现必须遵守的硬性规则：

- Vulkan 的 present 路径只能使用 binary semaphore 做 queue 同步
- 不能使用 timeline semaphore 做 present queue 同步

原因：

- `vkQueuePresentKHR` 的同步模型本身围绕 binary semaphore 设计
- swapchain acquire / render-finished 这条链路应保持 binary semaphore 语义
- timeline semaphore 不应进入 present acquire/render-finished 这条同步路径

明确要求：

- `AcquireNextImage` 返回的 ready semaphore 必须是 binary semaphore
- present 前等待 swapchain image ready 只能等该 binary semaphore
- render finished 只能 signal 给 present 使用的 binary semaphore
- queue 之间与 present 相关的 handoff，不允许把 timeline semaphore 直接接入 `vkQueuePresentKHR`

补充说明：

- 普通 submit 之间仍可继续使用 timeline fence / timeline-like completion event
- 但一旦进入 present 这条链路，必须切换到 binary semaphore 模型

> Vulkan present 只能用 binary semaphore 做 queue 同步，不能用 timeline semaphore。

## 9. 实现拆分

### 9.1 RHI 层

变更：

- `CommandList` 构造绑定 `EQueueType`，只允许 `Graphics` / `Compute`（`Copy` 不对外暴露）
- 新增 `CopyCommandScope`（由 Graphics / Compute CommandList 的 `BeginCopyScope()` 创建）
- 新增显式 `Transition`
- 新增 `BeginBufferOverlap` / `EndBufferOverlap`
- 删除旧 `RHISubmitCmdList` / `RHIPresentOp` / `RHIExecOp`
- 删除旧 `QueueTransferCmd`（由 `Transition(Acquire/Release)` 和 `CopyScope` 替代）
- `CommandList::Submit()` 改为移动语义：executor 调用后 command list 内部数据被 move 走，调用方复用同一对象录制下一帧是合法的（内部已清空）

### 9.2 Executor 层

变更：

- 重写 `RHIExecutor` 的 pending batch 模型，接口改为 `Array<CommandList&&>`（只接收 Graphics / Compute command list）
- executor 内部维护 copy queue 的 allocator pool 和 timeline，对外不暴露
- `VulkanSubmissionExecutor` 收敛成三块核心结构：
  - `TranslateInfo` / `TranslateResult`
  - `SubmitInfo`
  - `SyncPoint` / `InterruptTask`
- 增加 `RHIExecutor::Sync(ERHISyncDepth)`，返回 executor 级退休完成事件
- flush batch 自动等待前序所有未退休的 RHI submits
- readback API 返回的 `GraphEventRef` 在 interrupt runtime 中触发
- 不再以 `QueueSubmissionInfo` / `PresentInfo` 作为长期内部平台单元
- 废弃 `VkCommandQueue::ExecuteThread()` 后台线程；其承担的 allocator 回收、callback 执行、fence wait 职责全部迁移至 `RHIExecutor` 的 interrupt 线程
- 废弃 `VkCopyQueue` 的独立 IO 线程（`IOThreadLoop` / `RHIThreadLoop`）；copy queue 的提交统一纳入 `RHIExecutor` 的 preprocess / translate / submit 流水线

### 9.3 Vulkan Translate 层

变更：

- `VkCommandQueue::Translate` — 改为纯录制函数，不做全局依赖推导
- 新增 copy queue 内部 translate 路径：preprocess 从 CopyScope plan 生成 copy queue submit 描述，translate 阶段为每个 CopyScope 独立分配 copy queue allocator 并录制 copy queue native command buffer（与父 queue translate 可并行）
- `VkTracker` 增加：
  - `InitFromSeed(const TrackerSnapshot&)` — 用 preprocess 生成的 seed 初始化本地 tracker
  - `EmitLocalTransition(...)` — 录制同队列 transition barrier
  - `EmitAcquirePlan(...)` — 录制 acquire barrier（submit 头部）
  - `EmitReleasePlan(...)` — 录制 release barrier（submit 尾部）
- Allocator pool（包括 copy queue pool）的并发 pop 支持（参见 §7.3）

### 9.4 删除内容

- 删除独立 `ResolveDependencies` 阶段
- 删除 submit 后 restore-to-preferred-state 路径
- 废弃 `gfx_queue.Execute(...)` / `gfx_queue.Present(...)` 直接调用路径（renderer 侧全部改为 `RHIExecutor::Get().Submit(...)`）

### 9.5 迁移指导

**`ImportResourcesFromQueue` / `ExportResourcesToQueue` → 显式 `Transition`：**

```cpp
// 旧写法
cmd_list.ExportResourcesToQueue(EQueueType::Copy, {tex}, {buf});
// copy cmd list
cmd_list.ImportResourcesFromQueue(EQueueType::Graphics, {tex}, {buf});

// 新写法
// Graphics CommandList 末尾：
cmd_list.Transition(tex, ETextureState::ShaderRead, ETextureState::CopyDst,
                    {}, ERHITransitionFlags::Release, EQueueType::Copy);
// Copy CommandList 开头：
cmd_list.Transition(tex, ETextureState::CopyDst, ETextureState::CopyDst,
                    {}, ERHITransitionFlags::Acquire, EQueueType::Graphics);
// 或者直接使用 CopyScope（推荐，自动处理 acquire/release）
```

**`gfx_queue.Execute(cmd_list.Submit().Signal(...))` → `RHIExecutor::Submit`：**

```cpp
// 旧写法（RasterRenderer）
gfx_queue.Execute(cmd_list.Submit().Signal(timeline, time).DeleteResources());
gfx_queue.Present(swapchain, default_output_texture);

// 新写法
RHIExecutor::Get().Submit(
    {std::move(cmd_list)},
    ERHIExecSubmitFlags::FlushGPU | ERHIExecSubmitFlags::FrameEnd,
    &RHIPresentRequest{swapchain, default_output_texture}
);
```

**`device.GetCopyQueue().Execute(...)` → `CopyScope`：**

```cpp
// 旧写法
auto copy_evt = device.GetCopyQueue().Execute(scene_cmd_list.copy_queue_cmd_list.Submit());
device.GetCopyQueue().Sync(copy_evt.timeline);
gfx_queue.Execute(scene_cmd_list.gfx_queue_cmd_list.Submit());

// 新写法（在 Graphics CommandList 内）
{
    auto copy_scope = gfx_cmd_list.BeginCopyScope();
    copy_scope.UploadBuffer(...);
    copy_scope.CopyTexture(...);
} // 自动 release/acquire，executor 处理 signal/wait
RHIExecutor::Get().Submit({std::move(gfx_cmd_list)}, ERHIExecSubmitFlags::FlushGPU);
```

**`Barriers(...)` 调用 → `Transition`（光追路径）：**

```cpp
// 旧写法
cmd_list.Barriers(src_queue, dst_queue, pass_type, read_textures, write_textures);

// 新写法：按实际语义拆成逐资源 Transition
cmd_list.Transition(tex_a, ETextureState::ShaderRead, ETextureState::UnorderedAccess);
cmd_list.Transition(tex_b, ETextureState::UnorderedAccess, ETextureState::ShaderRead);
```

## 10. 验证计划

### 10.1 Validation 开关

Preprocess 的状态校验（`src_state` 兼容性检查、acquire/release 配对检查等）支持可选开关：

```cpp
// 编译期：
#define MOER_RHI_VALIDATION_ENABLED 1  // Debug/RelWithDebInfo 默认开启，Release 可关闭

// 运行期（可选）：
RHIExecutor::Get().SetValidation(bool enabled);
```

开关关闭时跳过所有校验逻辑，只保留核心 translate / submit 路径，用于 profiling / shipping 构建。

### 10.2 RHITest

覆盖以下场景：

- `CopyScope` upload -> Compute read
- `CopyScope` upload -> Graphics sample -> Present
- readback copy 返回 `GraphEventRef`，不依赖 `Flush()` + `WaitIdle()`
- `Sync(RHI)` 只等待 RHI submit retirement，不等待 present
- `Sync(Present)` 等待 RHI submit 与 present retirement
- 后一个 `FlushGPU` 自动等待前面所有未退休的 RHI submits
- Graphics release -> Compute acquire（显式 Transition）
- Compute release -> Graphics acquire（显式 Transition）
- 同队列 transition with subresource range
- Unknown 初始状态首次使用（Vulkan UNDEFINED layout 路径）
- acquire/release 不匹配（preprocess 报错验证）
- range 重叠冲突（preprocess 报错验证）
- 多个 cmdlist 并行 translate 时 allocator/tracker 不串扰
- `BeginBufferOverlap` / `EndBufferOverlap` 跳过 write-after-write barrier
- 帧结束时资源未回到 Graphics ownership（preprocess 报错验证）
- Bindless array 更新后 shader 读取（验证三个内部 buffer 的 barrier 正确生成）
- Bindless 资源未提前 Transition 到 SRV 状态时 preprocess 报错验证
- BLAS build + TLAS update 的跨命令依赖（验证 BLAS→TLAS barrier 正确插入）
- TLAS update 时 instance buffer inline barrier（scatter-copy → AS_BUILD 顺序正确）
- BLAS / TLAS buffer 帧末未归还 Graphics ownership（preprocess 报错验证）
- GPU profiler：单 CommandList 嵌套 Event 的 query 配对和解析（见 §16.11 场景 1）
- GPU profiler：多 CommandList 跨 queue 的 completion 绑定和独立解析（见 §16.11 场景 2）
- GPU profiler：Begin/End 不配对时的 validation 报错

### 10.3 Editor 验收

验收标准：

- Vulkan validation 开启
- 运行 `MoerEditor.exe`
- 加载默认场景
- 触发 scene copy / graphics / compute / present
- 只关注 validation `error`

warning / info：

- 允许存在
- 不作为本次 blocker

## 11. 默认决策

- preprocess 是整批次单线程
- translate 只读 preprocess 结果（通过 `seed_tracker` 拷贝，并发安全）
- submit 保持全局顺序
- flush batch 的 root submits 自动等待前序所有未退休的 RHI submits
- Copy queue 对调用方完全隐式，只能通过 `CopyScope` 间接使用
- Buffer v1 不做 offset/size range tracking
- Texture 默认 range = all mips / all layers
- 不保留旧接口兼容层
- Vulkan 是唯一行为验收目标
- `FrameEnd` 必须与 `FlushGPU` 同时使用
- 帧开始和帧结束（present 前）必须是 Graphics queue
- `Sync(RHI)` 不包含 present，`Sync(Present)` 包含 present
- Validation 开关：Debug 默认开，Release 可关

## 12. Timeline Value 策略

每个 native queue（Graphics / Compute / Copy）维护独立的单调递增 timeline 计数器。其中 Copy queue 的 timeline 由 executor 内部管理，调用方不可见：

```cpp
struct PerQueueTimeline {
    VulkanFence* fence;      // Vulkan timeline semaphore
    uint64       next_value; // 下一次 signal 使用的值，提交后自增
};
```

分配规则：

- 每次 `NativeSubmit` 调用 `AllocateTimelineSignal(queue)` 时，取 `next_value` 作为本次 signal value，然后 `next_value++`
- 每个 queue 的 signal 必须连续
- 每个 `SubmitInfo` 的 completion `SyncPoint` 在 submit 成功后解析成 `{queue_timeline_fence, signal_value}`
- 后继 `SubmitInfo` 只保存逻辑 `SyncPointId`，submit runtime 再把它解析成真实 wait value

跨队列依赖解析：

```
gfx submit  → resolve SP0 = {gfx_timeline, N}
copy submit → wait SP0 -> {gfx_timeline, N}
            → resolve SP1 = {copy_timeline, M}
gfx submit  → wait SP1 -> {copy_timeline, M}
```

Present 不使用 timeline semaphore（见 §8.2），只在 present cmd buffer submit 前等待所属 `SubmitInfo` 的 RHI completion，然后切换到 binary semaphore 路径。

interrupt runtime 在 host wait 时等待 `{fence, value}` 对，完成后触发回收、callback、readback event 与 `Sync(depth)` 事件。

## 13. Reorder 策略

### 13.1 现状与本次边界

现有代码中 `CmdReorderer` 实现了基于拓扑分层的 reorder 逻辑（layer assignment），允许同一 queue 内无依赖关系的命令并行录制到不同 layer，降低 GPU 串行等待。

**本次重构的明确态度：**

- `CmdReorderer` 的层级分配逻辑是 translate 阶段的内部实现细节，不影响 preprocess / submit 的接口
- preprocess 阶段不做 reorder：按 batch 原始顺序推导状态、记录 handoff plan
- translate 阶段可以保留 reorder 能力，但以"无 reorder"作为初始实现目标，reorder 作为后续可插拔优化层
- 本次重构完成后 reorder 接口应保持可接入（`Translate(CmdSubmit&&, const CmdReorderer*)` 签名保留），但默认传 `nullptr`

### 13.2 Reorder 与新 preprocess 的关系

旧设计中 `CmdReorderer` 在 `VkCommandQueue::Execute` 内部隐式运行（调用方无感知）。新设计中：

- preprocess 已经完成跨队列 handoff plan 和 local barrier 点的记录
- `seed_tracker` 拷贝已经携带了正确的初始状态
- 如果 reorder 需要在 translate 阶段重排命令顺序，必须在 `seed_tracker` 已固定的前提下仅重排 **同队列、同 commandlist 内部** 的命令顺序
- 跨 command list、跨队列的顺序由 submit 层保证，reorder 不得破坏

### 13.3 Reorder 边界约束（translate 阶段可选启用时）

1. **跨 CopyScope 边界禁止重排**：CopyScope 的 enter/exit 是 preprocess 已经固化的同步点，reorder 不能把 CopyScope 前后的命令跨 scope 移动
2. **显式 Transition 是硬边界**：`Transition(Release)` 和 `Transition(Acquire)` 是 preprocess 记录的同步边界，reorder 不能越过
3. **Query / Scope 边界**：timestamp query 和 debug scope 的 begin/end 不能被重排
4. **BufferOverlap 范围内**：overlap 范围内的命令在 reorder 时视 write-after-write 为无依赖，可以重排；其余 hazard 照常约束

### 13.4 `CmdReorderer` 状态跟踪（保持现有，与新 preprocess 不重叠）

`CmdReorderer` 跟踪的是 **layer 依赖关系**（命令间的先后约束），不跟踪 Vulkan 具体的 access mask / image layout。这两层职责分开：

- `CmdReorderer` → 命令顺序（layer 分配）
- `VkTracker` + preprocess seed → Vulkan barrier 内容（access/layout）

两者不共享数据结构，reorder 完成后 translate 仍使用 `seed_tracker` 初始化 `VkTracker`，走正常 barrier 推导路径。

## 14. Bindless Array 状态跟踪

### 14.1 问题描述

`UpdateBindlessArrayCmd` 在现有实现中只跟踪了三个内部管理 buffer 的状态（`bindless_array_buffer`、`bindless_buffer_descs`、`bindless_texture_descs`），**未跟踪被注册进 bindless array 的各个 texture / buffer 资源本身的状态**。

这在新设计中存在以下问题：

1. preprocess 的长期 `SubresourceStates` 需要知道 bindless array 的"写入"对下游 shader 读取意味着什么
2. 新设计删除了独立的 `ResolveDependencies` 阶段，无法在 preprocess 之外隐式补 barrier
3. 被注册进 bindless 的资源，其 `owner_queue` 和 `state` 必须在 preprocess 中明确

### 14.2 设计决策

**Bindless array 更新（`UpdateBindlessArray`）本身不跟踪被注册资源的具体状态。**

理由：

- bindless array 是一个间接寻址层，被注册的资源状态由资源自身的提交路径负责（Transition / CopyScope）
- `UpdateBindlessArray` 只负责把 descriptor handle 写入 descriptor table buffer，这个操作本身需要的 barrier 只涉及三个内部 buffer
- 下游 shader 通过 bindless array 读取资源时，资源状态必须已经由调用方在同一 command list 内显式 Transition 到 `ShaderRead` / `SRV` 状态

**约束（调用方必须保证）：**

- 在调用 `UpdateBindlessArray` 或在后续 draw/dispatch 通过 bindless 访问某资源之前，该资源必须已经通过 `Transition` 切换到正确的 shader read 状态（`owner_queue == Graphics` 或当前 dispatch 所在 queue）
- Validation 开启时，preprocess 可检查：被注册进 bindless array 的资源，在该 command list 内最后记录的访问状态是否为 SRV-compatible（`known==true` 且 `state` 在 Vulkan 侧对应 `SHADER_READ_ONLY_OPTIMAL` 或 `GENERAL`）
- 不满足此约束时，preprocess log error + assert

### 14.3 Bindless Array 自身 buffer 的长期状态

三个内部 buffer（`bindless_array_buffer`、`bindless_buffer_descs`、`bindless_texture_descs`）纳入正常长期状态跟踪：

- 初始 `known = false`，首次 `UpdateBindlessArray` 时从 `Unknown` 推导
- 每次 preprocess 处理 `UpdateBindlessArray` 后，记录终态为 `state=UAV_WRITE, owner_queue=Graphics`（或 Compute，取决于 CommandList queue type）
- 帧内后续命令（draw / dispatch）通过 `HandleBindless` 路径读取这三个 buffer 时，preprocess 推导 `UAV_WRITE → SHADER_READ` 的局部 barrier

### 14.4 `HasUpdates()` 语义明确

现有 `HasUpdates()` 使用 `!array_indices_dat.empty()` 判断——即使只有 buffer/texture descriptor 更新（`HasBufferUpdates()`/`HasTextureUpdates()` 为 true），只要 `array_indices_dat` 为空仍会跳过。

新设计中 preprocess 侧的处理规则：

```
if HasUpdates() || HasBufferUpdates() || HasTextureUpdates():
    记录三个内部 buffer 各自的 UAV-write 状态
else:
    该 UpdateBindlessArrayCmd 视为 no-op，不更新任何状态
```

## 15. Raytracing Scene 状态跟踪

### 15.1 涉及资源总览

一次 `BuildAccelerationStructures` + `UpdateRaytracingScene` 涉及以下资源，分属两层：

**BLAS 层（`BuildAccelerationStructuresCmd`，Compute queue）：**

| 资源 | 访问类型 | 状态 |
|------|---------|------|
| 各 BLAS 底层 buffer | Write | `AS_WRITE / AS_BUILD` |
| scratch buffer（自动分配） | Read+Write | `AS_WRITE\|AS_READ / AS_BUILD` |
| vertex buffer（每个 BLAS 几何） | Read | `SHADER_READ / AS_BUILD` |
| index buffer（每个 BLAS 几何） | Read | `SHADER_READ / AS_BUILD` |

**TLAS 层（`UpdateRaytracingSceneCmd`，Compute queue）：**

| 资源 | 访问类型 | 状态（有脏实例时） | 状态（ForceUpdate 无脏实例时） |
|------|---------|---------|---------|
| instance buffer | Write→Read | 先 `SHADER_WRITE/COMPUTE`（scatter-copy），后 inline barrier → `AS_READ/AS_BUILD` | 直接 `AS_READ/AS_BUILD` |
| scratch buffer | Read+Write | `AS_WRITE\|AS_READ / AS_BUILD` | 同左 |
| TLAS 底层 buffer | Write | `AS_WRITE / AS_BUILD` | 同左 |
| 各相关 BLAS 底层 buffer | Read（from BLAS write） | `AS_READ / AS_BUILD` | 同左 |

### 15.2 长期状态跟踪设计

**BLAS 底层 buffer 和 TLAS 底层 buffer** 纳入正常长期 `SubresourceStates`：

- 类型为 `Buffer`，按整资源跟踪（Buffer v1 不做 range）
- 初始 `known = false`
- 每次 preprocess 处理 `BuildAccelerationStructuresCmd` 后：
  - 各 BLAS buffer：`state = AS_WRITE, last_access_kind = Write, owner_queue = Compute`
- 每次 preprocess 处理 `UpdateRaytracingSceneCmd` 后：
  - TLAS buffer：`state = AS_WRITE, last_access_kind = Write, owner_queue = Compute`
  - 各相关 BLAS buffer：`state = AS_READ, last_access_kind = Read, owner_queue = Compute`（已被 TLAS build 消费）
- 帧末回到 Graphics ownership 时（§3.4），BLAS/TLAS buffer 需要在 Compute CommandList 末尾显式 `Transition(Release)` → Graphics `Transition(Acquire)`

**Instance buffer 和 scratch buffer** 视为 executor 内部资源，不纳入调用方可见的长期状态：

- 由 `UpdateRaytracingSceneCmd` 内部持有句柄
- preprocess 在处理该命令时读取当前 tracker 的局部状态推导 barrier，但不写入长期 `SubresourceStates`（这两个 buffer 不跨帧复用状态）

**Vertex buffer / Index buffer**：

- 这些是普通 mesh buffer，已纳入正常长期状态跟踪
- `BuildAccelerationStructuresCmd` preprocess 处理时，记录它们当前帧内的 `SHADER_READ / AS_BUILD` 访问
- 不影响它们在同帧其他 pass 中作为 VB/IB 被使用，因为 `SHADER_READ` 是幂等的 read 状态，不产生 write 冲突

### 15.3 BLAS → TLAS 依赖的跨命令通信机制

现有实现通过 `VkTracker::write_blas_states`（`Set<uint64>`）传递 BLAS→TLAS 的构建依赖。

新设计中该机制迁移到 preprocess 阶段的 per-commandlist 结果中：

```cpp
struct PreprocessCmdResult {
    // ... 其他字段 ...
    // BLAS 构建结果：记录本 commandlist 内写入的 BLAS 资源句柄
    // UpdateRaytracingSceneCmd 的 preprocess 读取此集合决定 BLAS→TLAS barrier
    UnorderedSet<uint64> written_blas_handles;
};
```

规则：

- preprocess 处理 `BuildAccelerationStructuresCmd` 时，把各 BLAS 底层 buffer 句柄加入 `written_blas_handles`
- preprocess 处理 `UpdateRaytracingSceneCmd` 时：
  - 若 `ForceUpdate == true` 或 `RelatedGeometries` 为空：依赖 `written_blas_handles` 中**所有** BLAS
  - 否则：只依赖 `written_blas_handles` 中与 `HasGeometry(handle)` 匹配的 BLAS
  - 对每个匹配的 BLAS buffer：记录 `AS_WRITE → AS_READ` 的局部 transition，在 translate 阶段生成对应 barrier

### 15.4 Inline Barrier 的处理

现有 `VkCmdVisitor::Visit(UpdateRaytracingSceneCmd*)` 中有一个手动构造的 inline barrier（instance buffer 从 `SHADER_WRITE/COMPUTE` → `AS_READ/AS_BUILD`，位于 scatter-copy dispatch 和 `vkCmdBuildAccelerationStructuresKHR` 之间）。

新设计中此 inline barrier 的处理方式：

- `UpdateRaytracingSceneCmd` 是一个**复合命令**，内部逻辑固定分两步：scatter-copy dispatch + TLAS build，中间需要一个不可省略的 barrier
- preprocess 层对此命令记录的状态只关心 instance buffer 的入口状态（来自长期状态）和出口状态（`AS_READ / AS_BUILD`）
- translate 阶段在录制 `UpdateRaytracingSceneCmd` 时，将 inline barrier 视为该命令 **内部实现**，直接调用 `vkCmdPipelineBarrier2` 插入，不经过 `VkTracker`（和现有做法一致，调用 `FlushSrcState` 更新 tracker src state）
- 此 inline barrier 不出现在 preprocess 的 plan 中，也不参与 seed_tracker 的初始状态

### 15.5 Raytracing Scene 状态跟踪的 Validation 规则

Validation 开启时（见 §10.1），preprocess 对 `UpdateRaytracingSceneCmd` 额外检查：

- `UpdateRaytracingSceneCmd` 的 `owner_queue` 必须是 Compute
- 在 `UpdateRaytracingSceneCmd` 之前，所有被引用的 BLAS buffer（通过 `written_blas_handles` 匹配）必须已经被本帧的 `BuildAccelerationStructuresCmd` 写入（`last_access_kind == Write, state == AS_WRITE`）；如果未找到对应的 BLAS write，log error + assert
- 帧末 Compute CommandList 必须通过 `Transition(Release)` 将 BLAS / TLAS buffer 的 ownership 交还给 Graphics，否则在帧末 `ValidateAllResourcesOwnedByGraphics` 时报错

## 16. GPU Profiler 设计

### 16.1 目标

设计一套与新 RHI 重构深度集成的 GPU Profiler 系统，支持：

- CommandList 级别的 GPU 执行时间跟踪（整个 command list 的 GPU work，由 Backend 自动插入）
- Event 级别的嵌套 scope 跟踪（CommandList 内部的 pass / stage，如 ShadowPass、GeometryPass）
- 多 queue 支持（Graphics / Compute，Copy queue 不参与 profiling）
- 延迟解析（timestamp query 结果在 interrupt 线程中解析，不阻塞 submit）
- 与现有 MoerProfiler 工具无缝集成（通过 TCP 19090 端口发送 trace event）
- Frame boundary 跟踪（通过 FrameBound event 标记帧边界）

### 16.2 核心数据结构

#### `GPUEvent`（存储在 CommandList 内部）

```cpp
struct GPUEvent {
    enum class EType {
        BeginCommandList,   // Backend 自动插入
        EndCommandList,     // Backend 自动插入
        BeginEvent,         // 用户手动插入
        EndEvent,           // 用户手动插入
        FrameBound          // Backend 在 FrameEnd 时自动插入
    };

    EType           type;
    std::string     name;         // BeginCommandList/BeginEvent 需要，其他为空
    QueryToken      query;        // 所有 event 都使用 EndQuery（不区分 Begin/End）
    uint32          depth;        // 嵌套深度（0 = CommandList 级别）
    uint64          cpu_time_ns;  // 录制时的 CPU 时间戳（用于 fallback）
};
```

#### `CommandList` 扩展（本地存储 GPU events）

```cpp
class CommandList {
public:
    // ... 现有接口 ...

    // GPU Profiler 接口
    void PushGPUEvent(GPUEvent&& event);
    Array<GPUEvent> StealGPUEvents();  // 提交时 flush 到 GPUEventStream

private:
    Array<GPUEvent> gpu_events;  // 本地存储，提交时移出
    uint32          event_depth = 0;  // 当前嵌套深度
};
```

#### `GPUEventStream`（全局单例，收集所有 CommandList 的 events）

```cpp
class GPUEventStream {
public:
    // 提交时从 CommandList 收集 events
    void RegisterSubmit(Array<GPUEvent>&& events, EQueueType queue, WaitEvent completion);

    // Interrupt 线程调用：解析已完成的 timeline 对应的所有 query
    void ResolveCompleted(uint64 timeline_value);

    // 发送到 MoerProfiler
    void FlushToProfiler();

private:
    struct PendingSubmit {
        Array<GPUEvent> events;
        EQueueType      queue;       // Graphics / Compute
        WaitEvent       completion;  // {fence, timeline_value}
    };

    Queue<PendingSubmit>       pending_submits;
    Array<ResolvedGPUEvent>    resolved_events;  // 已解析但未发送的 event
    std::mutex                 stream_mutex;
};

struct ResolvedGPUEvent {
    std::string name;
    EQueueType  queue;
    uint32      depth;
    uint64      timestamp_ns;  // GPU timestamp (已转换为 ns)
    bool        is_frame_bound;  // 是否为 FrameBound event
};
```

### 16.3 录制流程（CPU 侧）

#### 宏接口（调用方使用）

```cpp
// Event 级别（CommandList 内部的 scope，支持嵌套）
#define GPU_PROFILE_EVENT_BEGIN(cmd_list, name) \
    (cmd_list).PushGPUEvent({.type = GPUEvent::BeginEvent, .name = name, \
                             .query = (cmd_list).EndTimestampQuery(), \
                             .depth = ++(cmd_list).event_depth, \
                             .cpu_time_ns = SteadyNowNs()})

#define GPU_PROFILE_EVENT_END(cmd_list) \
    (cmd_list).PushGPUEvent({.type = GPUEvent::EndEvent, .name = "", \
                             .query = (cmd_list).EndTimestampQuery(), \
                             .depth = (cmd_list).event_depth--, \
                             .cpu_time_ns = SteadyNowNs()})

// RAII 封装（推荐）
#define GPU_PROFILE_EVENT_SCOPE(cmd_list, name) \
    GPUEventScope _gpu_event_scope(cmd_list, name)

// RAII helper
class GPUEventScope {
public:
    GPUEventScope(CommandList& cmd, std::string_view name) : cmd_list(cmd) {
        GPU_PROFILE_EVENT_BEGIN(cmd_list, name);
    }
    ~GPUEventScope() {
        GPU_PROFILE_EVENT_END(cmd_list);
    }
private:
    CommandList& cmd_list;
};
```

**注意**：CommandList 级别的 BeginCommandList / EndCommandList 由 Backend（Vulkan）自动插入，调用方无需手动调用。

#### 典型用法（RasterRenderer 示例）

```cpp
void RasterRenderer::RunSingle(...) {
    CommandList cmd_list(EQueueType::Graphics);

    // 注意：BeginCommandList / EndCommandList 由 Backend 自动插入，无需手动调用

    {
        GPU_PROFILE_EVENT_SCOPE(cmd_list, "ShadowPass");
        shadow_depth_pass->Process(cmd_list, ...);
    }

    {
        GPU_PROFILE_EVENT_SCOPE(cmd_list, "GeometryPass");
        geometry_pass->Process(cmd_list, ...);
    }

    {
        GPU_PROFILE_EVENT_SCOPE(cmd_list, "LightingPass");
        lighting_pass->Process(cmd_list, ...);
        {
            GPU_PROFILE_EVENT_SCOPE(cmd_list, "DirectionalShadowMask");
            directional_shadow_mask_pass->Process(cmd_list, ...);
        }
    }

    {
        GPU_PROFILE_EVENT_SCOPE(cmd_list, "PostProcess");
        ao_pass->Process(cmd_list, ...);
        ssr_pass->Process(cmd_list, ...);
        aa_pass->Process(cmd_list, ...);
    }

    // 提交（无需传递 gpu_events 参数，Backend 会自动从 CommandList 中提取）
    RHIExecutor::Get().Submit(
        {std::move(cmd_list)},
        ERHIExecSubmitFlags::FlushGPU | ERHIExecSubmitFlags::FrameEnd,
        &RHIPresentRequest{swapchain, default_output_texture}
    );
}
```

### 16.4 Backend 自动插入 CommandList Events

Backend（Vulkan）在 translate 阶段自动为每个 CommandList 插入 BeginCommandList / EndCommandList events：

```cpp
VulkanRecordedSubmit VulkanBackend::Translate(CmdSubmit&& submit) {
    VulkanRecordedSubmit result;

    for (CommandList& cmd_list : submit.command_lists) {
        // 1. 自动插入 BeginCommandList event（在头部）
        QueryToken begin_query = AllocateTimestampQuery();
        cmd_list.gpu_events.insert(0, GPUEvent{
            .type        = GPUEvent::BeginCommandList,
            .name        = "CommandList",  // 默认名称，可从 cmd_list metadata 获取
            .query       = begin_query,
            .depth       = 0,
            .cpu_time_ns = SteadyNowNs()
        });

        // 2. 录制 begin timestamp query 到 native command buffer
        vkCmdWriteTimestamp2(native_cmd_buffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                             begin_query.pool, begin_query.index);

        // 3. 正常 translate CommandList 的所有 commands
        for (const Command& cmd : cmd_list.commands) {
            TranslateCommand(native_cmd_buffer, cmd);
        }

        // 4. 自动插入 EndCommandList event（在尾部）
        QueryToken end_query = AllocateTimestampQuery();
        cmd_list.gpu_events.push_back(GPUEvent{
            .type        = GPUEvent::EndCommandList,
            .name        = "",
            .query       = end_query,
            .depth       = 0,
            .cpu_time_ns = SteadyNowNs()
        });

        // 5. 录制 end timestamp query 到 native command buffer
        vkCmdWriteTimestamp2(native_cmd_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                             end_query.pool, end_query.index);

        // 6. 如果是 FrameEnd，在最后一个 CommandList 尾部插入 FrameBound event
        if (HasFlag(submit.flags, FrameEnd) && &cmd_list == &submit.command_lists.back()) {
            QueryToken frame_bound_query = AllocateTimestampQuery();
            cmd_list.gpu_events.push_back(GPUEvent{
                .type        = GPUEvent::FrameBound,
                .name        = "",
                .query       = frame_bound_query,
                .depth       = 0,
                .cpu_time_ns = SteadyNowNs()
            });
            vkCmdWriteTimestamp2(native_cmd_buffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                 frame_bound_query.pool, frame_bound_query.index);
        }

        result.native_cmd_buffers.push_back(native_cmd_buffer);
    }

    return result;
}
```

### 16.5 提交与解析流程

#### Submit 接口（无需 gpu_events 参数）

```cpp
// RHIExecutor::Submit 接口保持不变（无需新增参数）
void Submit(
    Array<CommandList&&> command_lists,
    ERHIExecSubmitFlags  flags   = ERHIExecSubmitFlags::FlushGPU,
    RHIPresentRequest*   present = nullptr
);
```

Backend 在 translate 后自动从 CommandList 中提取 gpu_events 并注册到 GPUEventStream。

#### Preprocess 阶段（不处理 GPU events）

GPU events 不参与 preprocess 的状态推导，直接保留在 CommandList 内部。

#### Translate 阶段（Backend 自动处理）

```cpp
VulkanBackend::Translate(CmdSubmit&& submit):
    for cmd_list in submit.command_lists:
        // 1. 自动插入 BeginCommandList / EndCommandList / FrameBound events
        // 2. 录制所有 timestamp queries 到 native command buffer
        // 3. 提取 gpu_events 并注册到 GPUEventStream
        Array<GPUEvent> events = cmd_list.StealGPUEvents();
        GPUEventStream::Get().RegisterSubmit(
            std::move(events),
            cmd_list.GetQueueType(),
            completion_event  // 该 CommandList 的 timeline completion
        );
```

#### Interrupt 线程解析

```cpp
InterruptThreadLoop():
    while (running):
        task = PopInterruptTask()

        if task is HostWait:
            WaitFenceValues(task.waits)
            // 等待完成后，触发 GPU event 解析
            for wait in task.waits:
                GPUEventStream::Get().ResolveCompleted(wait.timeline_value)
                GPUEventStream::Get().FlushToProfiler()

        // ... 其他 task 处理 ...
```

#### `GPUEventStream::ResolveCompleted`

```cpp
void GPUEventStream::ResolveCompleted(uint64 timeline_value) {
    std::lock_guard lock(stream_mutex);

    // 找到所有 completion.timeline_value <= timeline_value 的 pending submit
    Array<PendingSubmit> completed;
    for (auto it = pending_submits.begin(); it != pending_submits.end(); ) {
        if (it->completion.value <= timeline_value) {
            completed.push_back(std::move(*it));
            it = pending_submits.erase(it);
        } else {
            ++it;
        }
    }

    // 解析每个 submit 的 GPU events
    for (PendingSubmit& submit : completed) {
        Stack<GPUEvent*> begin_stack;  // 用于配对 Begin/End

        for (GPUEvent& event : submit.events) {
            uint64 timestamp_ns = event.query.Get();  // 所有 event 都解析 query

            if (event.type == BeginCommandList || event.type == BeginEvent) {
                // Begin event：压栈，等待配对
                begin_stack.push(&event);
                event.resolved_timestamp = timestamp_ns;  // 暂存

            } else if (event.type == EndCommandList || event.type == EndEvent) {
                // End event：配对 Begin，生成 resolved event
                if (begin_stack.empty()) {
                    LOG_ERROR("GPU Profiler: Unmatched End event");
                    continue;
                }

                GPUEvent* begin_event = begin_stack.top();
                begin_stack.pop();

                uint64 begin_ns = begin_event->resolved_timestamp;
                uint64 end_ns = timestamp_ns;

                // 构造 resolved event（配对成功）
                resolved_events.push_back({
                    .name           = begin_event->name,
                    .queue          = submit.queue,
                    .depth          = begin_event->depth,
                    .timestamp_ns   = begin_ns,  // 使用 begin 时间作为 event 起点
                    .duration_ns    = end_ns - begin_ns,
                    .is_frame_bound = false
                });

            } else if (event.type == FrameBound) {
                // FrameBound event：单独记录，不配对
                resolved_events.push_back({
                    .name           = "FrameBound",
                    .queue          = submit.queue,
                    .depth          = 0,
                    .timestamp_ns   = timestamp_ns,
                    .is_frame_bound = true
                });
            }
        }

        // 检查配对完整性
        if (!begin_stack.empty()) {
            LOG_ERROR("GPU Profiler: {} unmatched Begin events", begin_stack.size());
        }
    }
}
```

#### `GPUEventStream::FlushToProfiler`

```cpp
void GPUEventStream::FlushToProfiler() {
    std::lock_guard lock(stream_mutex);

    for (const ResolvedGPUEvent& evt : resolved_events) {
        if (evt.is_frame_bound) {
            // FrameBound event：发送为 instant event（无 duration）
            uint32 track_id = MakeGpuQueueTrackId(0, evt.queue);
            std::string track_name = evt.queue == Graphics ? "GPU0/Queue(Graphics)" : "GPU0/Queue(Compute)";

            Moer::Trace::EmitInstant(
                "FrameBound",
                evt.timestamp_ns,
                track_id,
                track_name.c_str()
            );
        } else {
            // 普通 event：发送为 scope event（有 duration）
            uint32 track_id = MakeGpuQueueTrackId(0, evt.queue);
            std::string track_name = evt.queue == Graphics ? "GPU0/Queue(Graphics)" : "GPU0/Queue(Compute)";

            Moer::Trace::EmitScope(
                evt.name.c_str(),
                evt.timestamp_ns,
                evt.timestamp_ns + evt.duration_ns,
                track_id,
                evt.depth,
                track_name.c_str()
            );
        }
    }

    resolved_events.clear();
}
```

### 16.6 与 RHI 重构的集成点

#### Preprocess 阶段

GPU events 不参与 preprocess：

- 不影响资源状态推导
- 不影响 barrier plan
- 保留在 CommandList 内部，直接传递到 translate 阶段

#### Translate 阶段

Backend（Vulkan）在 translate 时：

1. 自动在 CommandList 头部插入 BeginCommandList event + timestamp query
2. 录制所有用户插入的 Event 对应的 timestamp queries
3. 自动在 CommandList 尾部插入 EndCommandList event + timestamp query
4. 如果是 FrameEnd，在最后一个 CommandList 尾部插入 FrameBound event + timestamp query
5. 提取 CommandList 的 gpu_events 并注册到 GPUEventStream

#### Submit 阶段

每个 `TranslateResult` 的 gpu_events 在组装成 `SubmitInfo` 后，与其所属 queue submit 的 RHI completion 绑定：

```cpp
SubmissionThreadLoop():
    submit_info = PopNextReadySubmitInfo()
    completion  = NativeSubmit(submit_info, ...)

    for translated in submit_info.translated_payloads:
        events = translated.StealGPUEvents()
        if (!events.empty()):
            GPUEventStream::Get().RegisterSubmit(
                std::move(events),
                translated.queue,
                completion
            )
```

#### Interrupt 阶段

在 `HostWait` 完成后，调用 `ResolveCompleted` 解析 query 结果，然后 `FlushToProfiler` 发送到 profiler tool。

### 16.7 多 CommandList 场景

如果一帧内有多个 CommandList（如 Compute CommandList + Graphics CommandList），每个 CommandList 独立跟踪：

```cpp
// Compute pass
CommandList compute_cmd(EQueueType::Compute);
{
    GPU_PROFILE_EVENT_SCOPE(compute_cmd, "BLAS Build");
    compute_cmd.BuildAccelerationStructures(...);
}
{
    GPU_PROFILE_EVENT_SCOPE(compute_cmd, "TLAS Update");
    compute_cmd.UpdateRaytracingScene(...);
}

// Graphics pass
CommandList gfx_cmd(EQueueType::Graphics);
{
    GPU_PROFILE_EVENT_SCOPE(gfx_cmd, "GeometryPass");
    geometry_pass->Process(gfx_cmd, ...);
}

// 提交（Backend 自动处理每个 CommandList 的 gpu_events）
RHIExecutor::Get().Submit(
    {std::move(compute_cmd), std::move(gfx_cmd)},
    ERHIExecSubmitFlags::FlushGPU | ERHIExecSubmitFlags::FrameEnd,
    nullptr
);
```

Backend 会自动：
1. 为 compute_cmd 插入 BeginCommandList / EndCommandList
2. 为 gfx_cmd 插入 BeginCommandList / EndCommandList
3. 为 gfx_cmd（最后一个 CommandList）插入 FrameBound（因为有 FrameEnd flag）
4. 分别注册两个 CommandList 的 events 到 GPUEventStream，各自绑定自己的 timeline completion

### 16.8 CopyScope 的 Profiling 策略

Copy queue 不参与 GPU profiling（§11 默认决策）。

理由：

- Copy queue 的 command buffer 由 executor 内部自动生成，调用方不可见
- CopyScope 的 GPU 时间已隐式包含在父 CommandList 的总时间中（因为父 CommandList 的 submit 会 wait copy completion）
- 如果需要单独跟踪 copy 时间，可以在 CopyScope 前后手动插入 CPU timestamp 记录（通过现有 `TRACE_SCOPE`），但不生成 GPU timestamp query

### 16.9 Query Pool 管理

#### 现有 `VulkanQueryRuntime` 的复用

新 GPU profiler 复用现有 `VulkanQueryRuntime` 的 query pool 和 lifecycle 管理：

- `EndTimestampQuery()` 返回 `QueryToken`（所有 event 都使用 EndQuery，不区分 Begin/End）
- `QueryToken::Get()` 阻塞等待 query 结果（在 interrupt 线程中调用，不阻塞 submit）
- `VulkanQueryRuntime::ResolveCompleted(timeline)` 按 timeline value 批量解析

#### Query Pool 容量规划

每帧的 query 数量 = `CommandList 数量 * 2 + Event 数量 * 2 + FrameBound 数量`。

假设：
- 每帧 2 个 CommandList（Graphics + Compute）
- 每个 CommandList 平均 10 个 Event
- 1 个 FrameBound
- 总计：`2 * 2 + 20 * 2 + 1 = 45` 个 query/frame

Query pool 容量建议：`max_frame_in_flight * 64`（预留余量）。

### 16.10 与现有 ProfilerStorage 的关系

现有 `VkCommandQueue::profiler_storage` 维护 13 帧滚动窗口的 GPU sample。

新设计中：

- **废弃 `ProfilerStorage` 的 GPU sample 存储**：新 profiler 通过 `GPUEventResolver` 直接发送到 MoerProfiler tool，不在 queue 内部缓存
- **保留 `ProfilerStorage` 的 CPU timing**：`GetProfilerEntry()` 返回的 `ProfileData.cpu_entries` 仍由 `ProfilerStorage` 提供（用于 CPU-side 的 queue execute 时间统计）
- **简化 `ProfilerStorage`**：移除 `BeginProfilerSession` / `EndProfilerSession` / `CollectProfiling` 等 GPU 相关方法，只保留 CPU timing 统计

### 16.10 与现有 ProfilerStorage 的关系

现有 `VkCommandQueue::profiler_storage` 维护 13 帧滚动窗口的 GPU sample。

新设计中：

- **废弃 `ProfilerStorage` 的 GPU sample 存储**：新 profiler 通过 `GPUEventStream` 直接发送到 MoerProfiler tool，不在 queue 内部缓存
- **保留 `ProfilerStorage` 的 CPU timing**：`GetProfilerEntry()` 返回的 `ProfileData.cpu_entries` 仍由 `ProfilerStorage` 提供（用于 CPU-side 的 queue execute 时间统计）
- **简化 `ProfilerStorage`**：移除 `BeginProfilerSession` / `EndProfilerSession` / `CollectProfiling` 等 GPU 相关方法，只保留 CPU timing 统计

### 16.11 逻辑验证

#### 场景 1：单 CommandList，嵌套 Event

```
录制（用户代码）：
    CommandList cmd(Graphics);
    GPU_PROFILE_EVENT_BEGIN(cmd, "Pass1")       → query_0 (EndQuery)
    GPU_PROFILE_EVENT_END(cmd)                  → query_1 (EndQuery)
    GPU_PROFILE_EVENT_BEGIN(cmd, "Pass2")       → query_2 (EndQuery)
        GPU_PROFILE_EVENT_BEGIN(cmd, "SubPass2a") → query_3 (EndQuery)
        GPU_PROFILE_EVENT_END(cmd)              → query_4 (EndQuery)
    GPU_PROFILE_EVENT_END(cmd)                  → query_5 (EndQuery)

Backend 自动插入：
    BeginCommandList("CommandList")             → query_6 (EndQuery, 插入到头部)
    EndCommandList("")                          → query_7 (EndQuery, 插入到尾部)
    FrameBound("")                              → query_8 (EndQuery, FrameEnd 时插入)

最终 gpu_events 顺序：
    [0] BeginCommandList, query_6, depth=0
    [1] BeginEvent("Pass1"), query_0, depth=1
    [2] EndEvent, query_1, depth=1
    [3] BeginEvent("Pass2"), query_2, depth=1
    [4] BeginEvent("SubPass2a"), query_3, depth=2
    [5] EndEvent, query_4, depth=2
    [6] EndEvent, query_5, depth=1
    [7] EndCommandList, query_7, depth=0
    [8] FrameBound, query_8, depth=0

解析（interrupt 线程）：
    query_6 = 1000ns, query_7 = 5000ns → CommandList: [1000, 5000], duration=4000ns, depth=0
    query_0 = 1100ns, query_1 = 2000ns → Pass1: [1100, 2000], duration=900ns, depth=1
    query_2 = 2100ns, query_5 = 4900ns → Pass2: [2100, 4900], duration=2800ns, depth=1
    query_3 = 2200ns, query_4 = 3000ns → SubPass2a: [2200, 3000], duration=800ns, depth=2
    query_8 = 5000ns                   → FrameBound: instant event at 5000ns

发送到 MoerProfiler：
    Track: "GPU0/Queue(Graphics)"
    - CommandList  [1000, 5000] depth=0
    - Pass1        [1100, 2000] depth=1
    - Pass2        [2100, 4900] depth=1
    - SubPass2a    [2200, 3000] depth=2
    - FrameBound   instant at 5000ns
```

#### 场景 2：多 CommandList，跨 queue

```
录制（用户代码）：
    Compute CommandList:
        GPU_PROFILE_EVENT_BEGIN(compute_cmd, "BLAS")  → query_0
        GPU_PROFILE_EVENT_END(compute_cmd)            → query_1

    Graphics CommandList:
        GPU_PROFILE_EVENT_BEGIN(gfx_cmd, "Geometry")  → query_2
        GPU_PROFILE_EVENT_END(gfx_cmd)                → query_3

Backend 自动插入：
    Compute CommandList:
        BeginCommandList("CommandList")  → query_4 (头部)
        EndCommandList("")               → query_5 (尾部)

    Graphics CommandList:
        BeginCommandList("CommandList")  → query_6 (头部)
        EndCommandList("")               → query_7 (尾部)
        FrameBound("")                   → query_8 (FrameEnd 时插入)

提交：
    RHIExecutor::Get().Submit({compute_cmd, gfx_cmd}, FrameEnd, nullptr)
    → Backend 分别注册两个 CommandList 的 events：
        GPUEventStream::RegisterSubmit(compute_events, Compute, compute_completion)
        GPUEventStream::RegisterSubmit(gfx_events, Graphics, gfx_completion)

解析：
    当 compute_completion 完成时：
        - 解析 compute_cmd 的所有 queries
        - 生成 resolved events 并发送到 MoerProfiler

    当 gfx_completion 完成时：
        - 解析 gfx_cmd 的所有 queries
        - 生成 resolved events 并发送到 MoerProfiler
        - FrameBound event 标记帧边界
```

**关键点**：每个 CommandList 的 events 与其自己的 timeline completion 绑定，避免跨 queue 的 completion 依赖问题。

### 16.12 MoerProfiler 工具的适配

现有 MoerProfiler 已支持：

- 多 track 显示（CPU / GPU queue）
- depth-based 嵌套布局
- 通过 `Trace::EmitScope` 接收 GPU event
- 通过 `Trace::EmitInstant` 接收 instant event（如 FrameBound）

**无需修改 MoerProfiler**，新 GPU profiler 通过现有 `Trace::EmitScope` 和 `Trace::EmitInstant` API 发送 event，MoerProfiler 自动识别 track ID 和 depth 进行渲染。

FrameBound event 在 profiler 中显示为垂直分隔线，标记帧边界。

### 16.13 Validation 与 Debug

#### Query 配对检查

在 `GPUEventStream::ResolveCompleted` 中，如果 Begin/End 不配对（stack 不为空或 underflow），log error：

```cpp
if (!begin_stack.empty()) {
    LOG_ERROR("GPU Profiler: {} unmatched Begin events", begin_stack.size());
}
```

#### Query 结果异常处理

如果 `QueryToken::Get()` 返回失败（query 未就绪或 query pool 耗尽），使用 `cpu_time_ns` 作为 fallback：

```cpp
uint64 timestamp_ns = event.query.Get();
if (timestamp_ns == QUERY_INVALID) {
    timestamp_ns = event.cpu_time_ns;  // fallback to CPU timestamp
    LOG_WARN("GPU Profiler: Query failed for {}, using CPU timestamp", event.name);
}
```

#### FrameBound 解析约束

FrameBound event 必须在其所在 CommandList 的 timeline completion 之后才能解析。由于 FrameBound 插入在最后一个 CommandList 尾部，其 query 会在该 CommandList 的 completion 时一起解析。

### 16.14 性能开销

#### CPU 侧

- `GPU_PROFILE_EVENT_BEGIN` / `END`：每次调用 1 次 `EndTimestampQuery()`（轻量，只是 push 到 command list 的 query_tokens 数组）
- `CommandList::StealGPUEvents()`：move 语义，O(1)
- 每帧额外内存：`sizeof(GPUEvent) * event_count`（约 80 bytes/event）

#### GPU 侧

- 每个 event：1 个 timestamp query（Vulkan 中为 `vkCmdWriteTimestamp2`，成本极低）
- Query pool readback：批量 `vkGetQueryPoolResults`，在 interrupt 线程中执行，不阻塞渲染

#### 开关控制

```cpp
// 编译期：
#define MOER_GPU_PROFILER_ENABLED 1  // Debug 默认开，Release 可关

// 运行期：
GPUEventStream::Get().SetEnabled(bool enabled);
```

关闭时所有宏展开为空操作，零开销。

### 16.15 设计要点总结

1. **CommandList 本地存储**：gpu_events 存储在 CommandList 内部，提交时 flush 到 GPUEventStream，避免 GPUEventStream 对 CommandList 的硬依赖
2. **Backend 自动插入**：BeginCommandList / EndCommandList / FrameBound 由 Backend 自动插入，调用方无需手动管理
3. **统一 EndQuery**：所有 timestamp query 都使用 EndQuery（不区分 Begin/End），简化 API 和实现
4. **FrameBound 标记**：FrameEnd 时自动插入 FrameBound event，用于标记帧边界，便于 profiler 工具分帧显示
5. **延迟解析**：query 结果在 interrupt 线程中解析，不阻塞 submit 线程
6. **Per-CommandList completion**：每个 CommandList 的 events 与其自己的 timeline completion 绑定，避免跨 queue 依赖问题
