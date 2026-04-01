# Unified Query Abstraction v1

## 1. 背景
旧版 GPU Profiling 在 `VulkanQueue.cpp` 中以 `ProfilerStorage` 的时间戳路径为核心，功能可用但较专用。v1 的目标是引入统一 Query 抽象，覆盖 `Timestamp/Trace` 与 `Occlusion`，同时保留现有 profiler 行为。

## 2. 范围
- 已实现：Vulkan 完整落地 + RHI 抽象接口。
- 已实现：旧接口 `PushScopeWithTimeScope` / `PopScopeWithTimeScope` / `GetProfilerEntry` 兼容。
- 已实现：`future + callback` 完成态查询。
- 已实现：内部 QueryPool 扩展点（backend factory）。
- 当前限制：D3D12 Query 后端未实现，返回错误结果；Copy Queue 不支持 Query，返回错误结果。

## 3. 公共接口（RHI）
定义位置：`source/runtime/render/rhi/RHICommand.h`

### 3.1 类型
- `QueryKind`: `Timestamp` / `Occlusion`
- `QueryStatus`: `Pending` / `Ready` / `Error`
- `QueryResult`: 统一结果容器
  - `TimestampQueryResult`: `begin_tick/end_tick/duration_ns`
  - `OcclusionQueryResult`: `sample_count/visible`
- `QueryToken`: 轻量句柄（可复制）
- `QueryFuture`: `IsReady()`、`Wait()`、`Get()`、`Then(callback)`

### 3.2 CommandList API
定义/实现位置：
- `source/runtime/render/rhi/RHICommand.h`
- `source/runtime/render/rhi/RHICommandList.cpp`

接口：
- `BeginTimestampQuery(name)` / `EndTimestampQuery(token)`
- `BeginOcclusionQuery(name)` / `EndOcclusionQuery(token)`

RAII：
- `TimestampQuerySpan`
- `OcclusionQuerySpan`

兼容层：
- `PushScopeWithTimeScope(name)` 内部会调用 `BeginTimestampQuery(name)`
- `PopScopeWithTimeScope()` 内部会调用 `EndTimestampQuery(token)`

## 4. 命令与重排
- 新增 `QueryCmd`（`BeginTimestamp/EndTimestamp/BeginOcclusion/EndOcclusion`）。
  - 位置：`source/runtime/render/rhi/RHIImpl.h`
- 重排器把 `QueryCmd` 当作强顺序边界（与 `ScopeCmd` 同级），避免包围关系被打散。
  - 位置：`source/runtime/render/rhi/vulkan/RHICmdReorderer.h`

## 5. Vulkan Runtime 设计
定义/实现位置：
- `source/runtime/render/rhi/vulkan/VulkanQueryRuntime.h`
- `source/runtime/render/rhi/vulkan/VulkanQueryRuntime.cpp`

### 5.1 核心组件
- `VulkanQueryRuntime`
- `IQueryPoolBackend`
- `VulkanTimestampPoolBackend`
- `VulkanOcclusionPoolBackend`
- `PoolBackendFactory`（按 `QueryKind` 注册）

### 5.2 QueryPool 语义
- Pool 绑定到 `owner_thread + owner_cmd_buffer`。
- 若检测到跨线程/上下文复用，运行时会新建池并输出 warning。
- 池容量不足自动扩容（倍增）。

### 5.3 生命周期
1. 录制开始：`BeginRecord(cmd_list, submit.query_tokens)`
2. 执行 Query 命令：`HandleQueryCommand(...)`
3. 提交后：`FinalizeSubmit(timeline)`
4. GPU 完成后（队列线程）：`ResolveCompleted(timeline)`
5. 结果写入 `QueryToken` shared-state，触发 `Then` 回调

错误路径：
- 空提交或不支持队列时，调用 `ResolveAsError(...)` 回填错误结果。

## 6. Profiler 兼容行为
位置：
- `source/runtime/render/rhi/vulkan/VulkanQueue.h`
- `source/runtime/render/rhi/vulkan/VulkanQueue.cpp`

说明：
- `ProfilerStorage` 改为消费 `QueryToken/QueryFuture` 的 timestamp 结果。
- `ProfileData` 产物、平滑窗口统计、`Trace::EmitScope` 行为保持。

## 7. 线程与回调语义
- `QueryFuture::Then(callback)` 在 Query resolve 所在线程触发。
- Vulkan 路径下，该线程是队列工作线程（非主线程）。
- `Then` 仅触发一次；`Get/Wait` 在 `Ready` 后可稳定读取结果。

## 8. 使用示例
```cpp
using namespace Moer::Render;

CommandList cmd;

// Timestamp
QueryToken ts = cmd.BeginTimestampQuery("GBuffer");
// ... draw/dispatch ...
cmd.EndTimestampQuery(ts);

ts.Then([](const QueryResult& r) {
    if (r.status != QueryStatus::Ready) return;
    auto t = std::get<TimestampQueryResult>(r.payload);
    // t.duration_ns
});

// Occlusion
QueryToken occ = cmd.BeginOcclusionQuery("MeshVisibility");
// ... draw ...
cmd.EndOcclusionQuery(occ);

QueryResult occ_result = occ.GetFuture().Get();
if (occ_result.status == QueryStatus::Ready) {
    auto o = std::get<OcclusionQueryResult>(occ_result.payload);
    // o.sample_count / o.visible
}
```

## 9. 后续扩展建议
- 新增 Query 类型时：
  1. 扩展 `QueryKind` 与 `QueryResult` payload
  2. 增加对应 `IQueryPoolBackend` 实现
  3. 注册 `PoolBackendFactory`
  4. 在 `QueryCmd` + Runtime resolve 逻辑中接入
- 如需对外暴露 QueryPool，可基于 backend/factory 增加受控句柄层，维持线程所有权约束不变。
