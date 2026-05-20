# runtime/remote

## 模块定位

`runtime/remote` 是引擎的远程接入层。

它的职责不是直接执行 Python，也不是直接操作 `Scene`，而是把外部请求稳定地接进来，再转交给 `runtime/scripting`。当前它已经提供：

1. 基于 libhv 的 HTTP 服务端
2. 基于 libhv 的 WebSocket 事件广播
3. 统一的 remote request / event / registry 基础设施
4. 通过 `submit_fn` 桥接到 `Engine::SubmitScriptExecution(...)`

一句话理解：

**`remote` 负责“外部怎么进来”，`scripting` 负责“脚本怎么执行”。**

---

## 它在系统里的位置

当前接线关系如下：

```text
Engine
  |- ScriptHost
  |- RemoteModule
       |- RemoteRequestRegistry
       |- RemoteEventHub
       |- RemoteScriptService
       |- RemoteHttpServer
       |- RemoteWebSocketServer

HTTP / WS Client
  -> RemoteHttpServer / RemoteWebSocketServer
  -> RemoteScriptService
  -> submit_fn
  -> Engine::SubmitScriptExecution(...)
  -> ScriptHost
```

关键边界：

1. `RemoteModule` 由 `Engine` 创建和销毁
2. `remote` 不直接依赖 `Engine` 的内部实现细节
3. `remote` 不应直接碰 `ScriptHost`
4. transport 层只负责协议和传输，不负责业务编排

---

## 目录结构

```text
remote/
  RemoteApi.h
  RemoteConfig.h/.cpp
  RemoteExecutionState.h/.cpp
  RemoteEvent.h
  RemoteRequestRecord.h
  RemoteSubmitFn.h
  RemoteModule.h/.cpp
  RemoteModuleController.h/.cpp
  RemoteRequestRegistry.h/.cpp
  RemoteEventHub.h/.cpp
  service/
    RemoteScriptService.h/.cpp
  transport/
    http/
      RemoteHttpJson.h/.cpp
      RemoteHttpServer.h/.cpp
    ws/
      RemoteWebSocketServer.h/.cpp
```

推荐阅读顺序：

1. `RemoteModule.h/.cpp`
2. `service/RemoteScriptService.h/.cpp`
3. `transport/http/RemoteHttpServer.h/.cpp`
4. `transport/ws/RemoteWebSocketServer.h/.cpp`
5. `RemoteRequestRegistry.h/.cpp`
6. `RemoteEventHub.h/.cpp`

---

## 核心类与职责

### `RemoteModule`

模块总入口和生命周期编排器。

它负责：

1. 持有 remote 配置与运行状态
2. 持有 registry / event hub / service / HTTP / WS 服务端
3. 对外暴露 `SetEnabled()` / `Start()` / `Stop()`
4. 通过 `RemoteModuleController` 给 Editor 等外部系统提供轻量控制句柄

它不负责：

1. 直接解析 HTTP JSON
2. 直接做脚本业务逻辑
3. 直接执行 Python

### `RemoteModuleController`

给 `Editor` 这类外部系统持有的弱控制句柄。

它的作用是避免把 `RemoteModule` 自身直接暴露到 UI 层，同时保留：

1. 查询启用状态
2. 查询运行状态
3. 运行时启停 remote
4. 查询当前配置快照

### `RemoteScriptService`

remote 模块的核心业务层。

它负责：

1. 接收 remote 风格的执行请求 DTO
2. 转换成 `scripting::ScriptExecutionRequest`
3. 调用 `submit_fn`
4. 记录 request 状态
5. 发布脚本执行结果事件
6. 组装同步响应结果

当前最重要的方法是：

1. `ExecuteAndWait(...)`

它当前采用同步等待 `ScriptExecutionFuture` 的实现，适合作为最小闭环。

### `RemoteRequestRegistry`

线程安全地维护 remote 请求记录。

它负责：

1. 生成稳定递增的 request id，例如 `remote-000001`
2. 维护 `Queued / Running / Completed / Failed / Cancelled` 状态
3. 保存原始请求和最终结果快照
4. 为 HTTP 查询接口提供读取入口

### `RemoteEventHub`

内存内的简洁 pub/sub。

它负责：

1. 接收 service 层发布的事件
2. 允许 WS 层订阅事件
3. 在锁外调用回调，避免回调执行路径把内部锁持有太久

### `RemoteHttpServer`

基于 libhv 的 HTTP 传输层。

它负责：

1. 启动 HTTP server
2. 注册路由
3. 调用 `RemoteHttpJson` 解析和生成 JSON
4. 调用 `RemoteScriptService` 执行业务

当前暴露的路由：

1. `GET /healthz`
2. `GET /api/remote/status`
3. `POST /api/script/execute`
4. `GET /api/script/requests/:request_id`

### `RemoteWebSocketServer`

基于 libhv 的 WebSocket 事件广播层。

它负责：

1. 接受 `/ws/events` 路径的客户端连接
2. 订阅 `RemoteEventHub`
3. 把事件转成 JSON 并广播给当前所有连接

当前它只负责事件下发，不负责双向命令协议。`onmessage` 目前没有业务语义。

---

## 当前数据模型

### `RemoteConfig`

远程服务配置，当前来源于 `[engine.remote]`：

1. `enable`
2. `bind_address`
3. `http_port`
4. `websocket_port`

### `ERemoteExecutionState`

统一表示 remote 层请求状态：

1. `Queued`
2. `Running`
3. `Completed`
4. `Failed`
5. `Cancelled`

### `RemoteEvent`

统一事件结构，当前主要承载：

1. `type`
2. `request_id`
3. `state`
4. `message`
5. `stdout_chunk`
6. `stderr_chunk`

当前事件类型的主力用法是：

1. `script.completed`
2. `script.failed`

### `RemoteRequestRecord`

registry 中保存的完整请求快照，包含：

1. `request_id`
2. `state`
3. 原始 `ScriptExecutionRequest`
4. 可选的 `ScriptExecutionResult`

---

## 关键请求流转

最重要的一条链路是远程执行脚本：

```text
POST /api/script/execute
  -> RemoteHttpServer
  -> RemoteHttpJson::ParseExecuteScriptRequest
  -> RemoteScriptService::ExecuteAndWait
  -> submit_fn
  -> Engine::SubmitScriptExecution(...)
  -> ScriptHost::Submit(...)
  -> ScriptExecutionFuture
  -> RemoteRequestRegistry
  -> RemoteEventHub
  -> HTTP Response
  -> WebSocket event (script.completed / script.failed)
```

这个流转里最重要的设计约束是：

1. transport 不直接调用 `ScriptHost`
2. 所有脚本执行请求统一先过 service 层
3. registry 和 event hub 是公共基础设施，而不是 HTTP 专属实现

---

## 当前对外能力

### 1. 健康检查

```http
GET /healthz
```

典型返回：

```json
{
  "ok": true,
  "service": "remote"
}
```

### 2. 运行状态查询

```http
GET /api/remote/status
```

典型返回：

```json
{
  "enabled": true,
  "running": true,
  "bind_address": "127.0.0.1",
  "http_port": 18080,
  "websocket_port": 18081
}
```

### 3. 执行脚本

```http
POST /api/script/execute
Content-Type: application/json
```

最常用的请求体：

```json
{
  "code": "print('hello remote')",
  "source_name": "remote/http",
  "execution_kind": "Snippet",
  "session_policy": "Stateless",
  "session_id": "",
  "origin": "Terminal"
}
```

典型响应：

```json
{
  "request_id": "remote-000001",
  "state": "Completed",
  "success": true,
  "stdout_text": "hello remote\n",
  "stderr_text": "",
  "exception_text": ""
}
```

注意：

1. `ExecuteAndWait(...)` 当前是同步等待结果
2. 这里的 `session_policy`、`origin`、`execution_kind` 实际复用了 `runtime/scripting` 的语义

### 4. 查询请求快照

```http
GET /api/script/requests/{request_id}
```

用于查询 registry 中保存的 request 状态和结果。

### 5. WebSocket 事件流

```text
ws://<host>:<port>/ws/events
```

当前主要用于广播脚本执行完成或失败事件。

示例事件：

```json
{
  "type": "script.completed",
  "request_id": "remote-000001",
  "state": "Completed",
  "message": "",
  "stdout_chunk": "hello remote\n",
  "stderr_chunk": ""
}
```

---

## Engine 接线与生命周期

当前由 `Engine` 负责：

1. 先创建并启动 `ScriptHost`
2. 从 `GlobalConfig` 读取 `engine.remote`
3. 组装 `submit_fn`
4. 创建 `RemoteModule`
5. 根据配置或外部控制决定是否启动 remote
6. shutdown 时先停 `RemoteModule`，再停 `ScriptHost`

这个顺序非常重要：

1. remote 是入口层，应先停入口再停执行器
2. 这样可以避免 shutdown 过程继续接收新的远程请求

---

## 如何使用

### 通过配置启用

在 `MoerEngine.toml` 中：

```toml
[engine.remote]
enable = true
bind_address = "127.0.0.1"
http_port = 18080
websocket_port = 18081
```

### 通过运行时控制句柄启用

适合 Editor 或测试代码：

```cpp
const auto remote_controller = engine.GetRemoteModuleController();
remote_controller.SetEnabled(true);
```

### 快速手工测试

```powershell
curl.exe http://127.0.0.1:18080/healthz
curl.exe http://127.0.0.1:18080/api/remote/status
```

执行脚本：

```powershell
$code = @'
print("hello remote")
'@
$body = @{ code = $code } | ConvertTo-Json -Compress
curl.exe -X POST http://127.0.0.1:18080/api/script/execute -H "Content-Type: application/json" --data-raw $body
```

现成的 UI 示例可以看 `source/editor/subui/RemoteExamplesUI.cpp`。

---

## 如何修改和新增功能

### 新增一个 HTTP API

推荐改动顺序：

1. 先判断它是不是业务能力，如果是，优先落到 `RemoteScriptService` 或新增 service
2. 如果需要 JSON DTO，改 `transport/http/RemoteHttpJson.h/.cpp`
3. 在 `RemoteHttpServer::RegisterRoutes()` 中新增 route
4. 补对应测试

不要这样改：

1. 直接在 route lambda 里拼复杂业务逻辑
2. 在 HTTP 层直接触碰 `ScriptHost`
3. 在 transport 层偷偷维护业务状态

### 新增一个 WebSocket 事件类型

推荐改法：

1. 确认事件由谁拥有语义，通常应在 service 或 module 层发布
2. 如有必要，扩展 `RemoteEvent`
3. 在 `RemoteEventHub` 发布新事件
4. 让 WS 层继续做“纯广播”
5. 补事件测试

不要把事件拼装逻辑塞进 `RemoteWebSocketServer`。

### 新增一种外部接入面

例如未来加入 terminal adapter、web UI adapter、MCP adapter，建议复用：

1. `RemoteScriptService`
2. `RemoteRequestRegistry`
3. `RemoteEventHub`

不要重新绕开现有 service 层再造一条私有请求通路，否则状态和事件会开始分叉。

### 新增 remote 配置项

至少同步检查这些位置：

1. `runtime/core/include/config/GlobalConfig.h`
2. `runtime/core/source/config/GlobalConfig.cpp`
3. `runtime/remote/RemoteConfig.h/.cpp`
4. `MoerEngine.toml`
5. `template.MoerEngine.toml`
6. 如需要，补 `status` 接口输出

---

## 代表性测试与回归入口

推荐先看这些测试：

1. `source/test/remote/TestRemoteHttpSmoke.cpp`
2. `source/test/remote/TestRemoteWebSocketSmoke.cpp`
3. `source/test/remote/TestRemoteDisabledByConfig.cpp`
4. `source/test/remote/TestRemoteHttpExecuteSmoke.cpp`
5. `source/test/remote/TestRemoteWebSocketEventSmoke.cpp`
6. `source/test/remote/TestRemoteEngineLoopbackDemo.cpp`

它们分别覆盖：

1. libhv 最小 HTTP 连通性
2. libhv 最小 WebSocket 连通性
3. remote 启停和 disabled 行为
4. HTTP execute 端到端路径
5. WebSocket 事件广播路径
6. 引擎运行中的完整 loopback 演示

如果改动范围较小，优先使用最窄的测试：

1. 改路由或 JSON：先跑 `TestRemoteHttpExecuteSmoke`
2. 改事件广播：先跑 `TestRemoteWebSocketEventSmoke`
3. 改启停逻辑：先跑 `TestRemoteDisabledByConfig`

---

## 常见误区

1. 不要把 `remote` 理解成“脚本系统本体”，它只是接入层
2. 不要在 transport 层直接依赖 `Engine` 或 `ScriptHost`
3. 不要在 `RemoteWebSocketServer` 里堆业务逻辑，WS 层应尽量保持薄
4. 不要在多个入口各自维护请求状态，统一走 `RemoteRequestRegistry`
5. 不要把一次性 demo 代码写进核心 service；示例和 UI 应留在测试或 editor 层

---

## 当前边界与后续扩展

当前模块有意保持收敛：

1. 没有做完整认证、鉴权、RBAC
2. 没有做复杂的双向 WS 命令协议
3. `ExecuteAndWait(...)` 仍是同步模型
4. 还没有完整的 terminal / web UI / MCP 客户端实现

如果后续继续扩展，优先保持下面两条原则：

1. transport 薄，service 厚
2. 外部入口多样，但内部状态与事件面统一

---

## 一句话心智模型

可以把 `runtime/remote` 看成：

**一个由 `Engine` 托管、把外部 HTTP / WebSocket 请求转成统一脚本执行请求，并通过 registry + event hub 维护状态与事件的远程接入模块。**