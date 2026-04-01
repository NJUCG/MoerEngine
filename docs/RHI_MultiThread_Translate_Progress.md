# RHI MultiThread Translate Progress

更新时间：2026-03-11
参考基线：`docs/RHI_MultiThread_Translate_Optimization.md` 第 4 章（Vulkan Stage1B 已落地能力）

## 1. 总览结论
- Stage1B 四阶段主链路已在 Vulkan 平台侧形成闭环：`Preprocess -> TranslateAndAssemble -> Resolve -> Execute`。
- 核心实现集中在 `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp`。
- `RHIExecutor` 已在 Vulkan 路径接入该执行器，非 Vulkan 保持 fallback 顺序执行。

## 2. 入口接入状态

| 检查项 | 状态 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| `RHIExecutor::Submit(Array<RHIExecOp>&&)` Vulkan 分流 | 已完成 | `source/runtime/render/rhi/RHIExecutor.cpp:64-70` | Vulkan 走 `VulkanSubmissionExecutor::Execute(std::move(ops))`。 |
| 非 Vulkan fallback | 已完成 | `source/runtime/render/rhi/RHIExecutor.cpp:8-46` | 仍是顺序执行，不含自动依赖规划。 |

## 3. 4.1 内部核心结构对照

| 能力项（优化文档） | 状态 | 代码位置 | 备注 |
| --- | --- | --- | --- |
| `ResourceAccessDigest` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:47-55` | 以 `ResourceKey -> ResourceAccessDigestEntry` 记录读写摘要。 |
| `CmdSubmitPreprocessResult` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:91-98` | 含 `initial_state_snapshot` / `last_state_snapshot` / `digest`。 |
| `ExecutePreprocessStore` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:100-123` | 提供 `Add/Find` 与 `SubmissionKey -> result` 查询。 |
| `FrameStateStore` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:125-128` | 生命周期为单次 `Execute()`。 |
| `QueueSubmissionInfo` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:130-157` | 含 `wait_submission_keys`、原始 `CmdSubmit`、preprocess 指针。 |
| `PresentInfo` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:159-173` | 含 `valid` 与 `error`。 |
| `PlatformExecOp` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:175` | `variant<QueueSubmissionInfo, PresentInfo>`。 |

## 4. 4.2 Phase A（Preprocess）进展

| 子能力 | 状态 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| 按 `op_seq` / `submit_idx` 遍历 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:917-923` | 双层顺序遍历。 |
| `ResourceAccessDigest` 收集 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:924-925` + `244-657` | 已覆盖 upload/copy/draw/dispatch/barrier/queue_transfer/raytracing/custom 等命令分支。 |
| 链式 `initial_state_snapshot` 传递 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:920,946` | submit0 取 `global_last_state`，后续 submit 取前次 `chained_state`。 |
| `ApplyDigestToState` 生成 `last_state_snapshot` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:686-708,949-950` | digest 应用到链式状态并保存快照。 |
| `RHISubmitCmdList` 结束回写 `global_last_state` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:954` | 满足 Execute 生命周期内持久。 |
| `write_textures` 补充 RT 写语义 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:927-940` | 仅在该 op 最后一个 submit 注入。 |

## 5. 4.3 Phase B（TranslateAndAssemble）进展

| 子能力 | 状态 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| submit 异步分发（`std::async`） | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:973-998` | 每个 submit 启动异步任务组装 `QueueSubmissionInfo`。 |
| 绑定 `SubmissionKey` 与 preprocess | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:977-979,988-995` | 通过 `preprocess_store.Find(key)` 建链。 |
| 按创建顺序 `future.get()` 回收 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:1000-1006` | 维持 `platform_ops` 顺序稳定。 |
| Present 组装为 `PresentInfo` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:1008-1010` | 与 submit 统一进入 `platform_ops`。 |
| 独立 `PlatformCmdList` 产物 | 未实现（设计差距） | `docs/RHI_MultiThread_Translate_Optimization.md:72,113-114` | 目前 Phase B 是异步封装+有序组装，不是深度 translate 产物。 |

## 6. 4.4 Phase C（SubmissionDependencyResolver）进展

| 子能力 | 状态 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| hazard 状态追踪（`last_writer/last_readers`） | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:177-180,715-716,740-786` | 资源粒度追踪读写关系。 |
| 跨队列 `RAW/WAR/WAW` 依赖提取 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:742-759` | 读/写与历史 writer/readers 形成依赖集合。 |
| `wait_submission_keys` 排序输出 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:762-769` | 按 `SubmissionKeyLess` 排序。 |
| 不重排 `platform_ops` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:718-833` | 原地遍历标注依赖，返回同序列。 |
| Present 后 Submit 校验 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:713,722-730` | 发现则 `submit_info.valid=false` 并报错。 |
| Present queue 必须 Graphics | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:792-798` | 不满足则 `present_info.valid=false`。 |
| Present 目标最后写者队列校验 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:804-826` | 最后写者非 Graphics 时报错。 |

## 7. 4.5 Phase D（SubmissionPlanExecutor）进展

| 子能力 | 状态 | 代码位置 | 说明 |
| --- | --- | --- | --- |
| `completion_by_submit` 维护 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:839-840,884` | 存储 submit 完成事件。 |
| 注入 `wait_submission_keys -> submit.wait_events` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:850-861` | 依赖完成事件按 key 注入。 |
| Graphics/Compute 执行 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:865-869` | 走 `CommandQueue::Execute`。 |
| Copy 执行并转 `WaitEvent` | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:871-875` | `CopyQueue::Execute` 结果转换为 `WaitEvent`。 |
| Present 执行 | 已完成 | `source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp:886-903` | 校验 `valid` 后发起 Present。 |

## 8. 现存差距（与优化文档第 6 章一致）
- `FrameStateStore` 当前仅在单次 `Execute` 生命周期内有效，尚未定义“同帧多次 Submit”跨调用状态拼接策略。
- Phase B 尚无独立平台命令级 translate 产物（`PlatformCmdList` 类似对象）。
- 依赖解析粒度仍是资源 key 级，尚未细化到子资源/范围。
- 高级显式同步命令与自动规划器之间的优先级/覆盖语义仍需进一步收敛。

## 9. 快速定位索引
- Vulkan 主实现：`source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.cpp`
- Vulkan 执行器接口：`source/runtime/render/rhi/vulkan/VulkanSubmissionExecutor.h`
- RHI 提交入口：`source/runtime/render/rhi/RHIExecutor.cpp`
- 参考设计文档：`docs/RHI_MultiThread_Translate_Optimization.md`
