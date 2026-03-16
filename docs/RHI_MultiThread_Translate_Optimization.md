# RHI 多线程 Translate 优化（现状同步，排除 Trace）

## 1. 文档目的
本文用于同步当前 `RHI` 提交链路的实际落地状态，重点是 `RHIExecutor` 与 Vulkan 平台侧自动同步路径。

范围约束：
- 包含：提交顺序、资源状态预处理、跨队列依赖解析、提交执行与 Present 校验。
- 不包含：任何新增 Trace/Profiling 相关实现细节。

## 2. 外部接口与调用约束（保持不变）
当前对调用侧的主接口不变：
- `RHISubmitCmdList`
- `RHIPresentOp`
- `RHIExecOp = variant<RHISubmitCmdList, RHIPresentOp>`
- `RHIExecutor::Submit(Array<RHIExecOp>&&)`

调用约束：
1. `frame_ops` 输入顺序就是全局执行顺序来源。
2. `Present` 之后不允许再出现 `Submit`。
3. `Present` 目标纹理最终写入队列必须是 `Graphics`。

## 3. 当前提交入口
### 3.1 `RHIExecutor::Submit`
- `RHIExecutor::Submit(Array<RHIExecOp>&&)` 持有全局 `submit_mutex`，保证入口串行。
- 当 `RHIType == Vulkan` 时，转入 `VulkanSubmissionExecutor::Execute(std::move(ops))`。
- 非 Vulkan 走 fallback 顺序执行路径（不含 Vulkan 这套自动依赖规划器）。

### 3.2 平台层职责下沉
与 Queue/Submission 强相关的解析与执行逻辑已放到 Vulkan 平台层（`VulkanSubmissionExecutor.cpp` 内部），而不是放在 RHI 通用层。

## 4. Vulkan Stage1B 已落地能力

### 4.1 内部核心结构
当前 Vulkan 提交执行包含以下内部结构：
- `ResourceAccessDigest`：单个 `CmdSubmit` 的资源读写摘要。
- `CmdSubmitPreprocessResult`：`initial_state_snapshot`、`last_state_snapshot`、`digest`。
- `ExecutePreprocessStore`：保存整次 `Execute` 的 preprocess 结果，并提供 `SubmissionKey -> result` 查询。
- `FrameStateStore`：本次 `Execute` 生命周期内的全局 `last_state` 快照存储。
- `QueueSubmissionInfo`：平台提交单元（含 wait 依赖 key、原始 `CmdSubmit`、preprocess 指针）。
- `PresentInfo`：平台 Present 单元（含校验状态与错误信息）。
- `PlatformExecOp = variant<QueueSubmissionInfo, PresentInfo>`。

### 4.2 Phase A：Preprocess（资源状态预处理）
入口：`PreprocessFrameOps(const Array<RHIExecOp>&, FrameStateStore&)`

处理流程：
1. 按 `frame_ops` 生成 `op_seq`。
2. 对每个 `RHISubmitCmdList` 内的 `submits` 按 `submit_idx` 顺序处理。
3. 收集每个 `CmdSubmit` 的 `ResourceAccessDigest`（覆盖 upload/copy/draw/dispatch/barrier/queue_transfer/raytracing/custom 等命令类型）。
4. `initial_state_snapshot` 来自链式状态：
   - 同一个 `RHISubmitCmdList` 内：前一个 submit 的 `last_state` 传给后一个。
   - 首个 submit：来自 `FrameStateStore.global_last_state`。
5. `ApplyDigestToState` 更新链式状态，生成该 submit 的 `last_state_snapshot`。
6. 完成一个 `RHISubmitCmdList` 后，把最终链式状态写回 `FrameStateStore.global_last_state`。
7. 若 `RHISubmitCmdList.write_textures` 有标记，则在该 op 最后一个 submit 的 digest 中补充 `RENDER_TARGET` 写入语义。

结论：
- 已实现“资源状态准备”和“执行提交”分离。
- 已实现同一 `RHISubmitCmdList` 内状态链式传递（`a.last_state -> b.initial_state`）。
- `last_state` 在单次 `Execute` 生命周期内持久存在。

### 4.3 Phase B：TranslateAndAssemble（可并发分发 + 有序组装）
入口：`TranslateAndAssemble(Array<RHIExecOp>&&, const ExecutePreprocessStore&)`

当前行为：
1. 对 `RHISubmitCmdList.submits` 启动异步任务（`std::async`），生成 `QueueSubmissionInfo`。
2. 每个 `QueueSubmissionInfo` 绑定对应 `SubmissionKey(op_seq, submit_idx)` 与 preprocess 结果。
3. `future.get()` 按创建顺序回收，保证 `platform_ops` 顺序不被异步完成时序打乱。
4. `RHIPresentOp` 转为 `PresentInfo` 并按 `op_seq` 进入同一 `platform_ops`。

说明：
- 当前 Phase B 主要完成“异步封装与有序组装”；平台命令列表深度翻译仍依赖后续执行路径，不是独立的 `PlatformCmdList` 产物。

### 4.4 Phase C：SubmissionDependencyResolver（自动跨队列依赖）
入口：`SubmissionDependencyResolver::Resolve(Array<PlatformExecOp>&&)`

当前策略：
1. 以资源 hazard 状态追踪 `last_writer` 与 `last_readers`。
2. 对每个 submit 的 digest 检测跨队列 `RAW/WAR/WAW` 风险。
3. 风险成立时记录 `wait_submission_keys`（按 `SubmissionKey` 排序）。
4. 保持输入顺序，不重排 `platform_ops`。

校验规则：
- `Present` 后出现 `Submit`：标记无效并报错。
- `Present.queue != Graphics`：标记无效并报错。
- 若存在 Present 目标纹理的最后写者，且其队列不是 `Graphics`：标记无效并报错。

### 4.5 Phase D：SubmissionPlanExecutor（执行提交计划）
入口：`SubmissionPlanExecutor::Execute(Array<PlatformExecOp>&&)`

执行流程：
1. 维护 `completion_by_submit` 映射（`SubmissionKey -> WaitEvent`）。
2. 对每个 `QueueSubmissionInfo`：
   - 把 `wait_submission_keys` 对应完成事件注入到 `submit.wait_events`。
   - 按队列类型执行：
     - `Graphics/Compute`：走 `CommandQueue::Execute`。
     - `Copy`：走 `CopyQueue::Execute`，并转成 `WaitEvent` 记录。
3. 对 `PresentInfo`：通过校验后执行 `Present`。

结论：
- 已形成“按 `frame_ops` 顺序执行 + 自动注入跨队列 wait 依赖”的提交闭环。

## 5. 与目标设计的一致项
已满足：
1. 调用侧以 `frame_ops` 统一提交，不再需要手写常规跨队列 wait/signal 编排。
2. 同一 `RHISubmitCmdList` 内 submit 链式状态传递已生效。
3. Executor 能根据资源摘要自动生成跨队列等待依赖。
4. `Present` 的 Graphics 最终写入约束已做运行时校验。

## 6. 当前差距与后续建议
仍建议在后续阶段继续完善：
1. `FrameStateStore` 生命周期当前是“单次 `Execute`”；若未来一帧多次调用 `Submit`，需定义跨调用状态拼接策略。
2. Phase B 目前未产出独立 `PlatformCmdList` 翻译结果对象；若目标是“translate 后完全独立执行”，需补充该产物与接口。
3. 依赖解析粒度当前为资源 key 级，后续可按子资源/范围细化，减少过度同步。
4. 高级显式同步命令（`Barrier/QueueTransfer/WaitEvent`）仍保留；需明确其与自动规划器的优先级与覆盖语义。

## 7. 阶段结论
- Stage1A：已完成（统一 `RHIExecOp` 提交入口与基础执行框架）。
- Stage1B：已完成当前版本落地（preprocess + 自动依赖解析 + 顺序执行闭环）。
- 下一步建议：进入 Stage1C，聚焦“平台命令级 translate 产物化”和“更细粒度同步规划优化”。
