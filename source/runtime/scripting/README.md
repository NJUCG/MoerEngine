# runtime/scripting

## 模块定位

`runtime/scripting` 是引擎里的嵌入式 Python 执行模块。

它负责：

1. 初始化和关闭 Python 解释器
2. 接收统一的脚本执行请求
3. 维护脚本执行线程和任务队列
4. 管理 `SharedGlobal / NamedSession / Stateless` 三种上下文语义
5. 把脚本里的 Scene 操作安全地桥接回主线程

它不负责：

1. 暴露远程网络协议
2. 管理 HTTP / WebSocket
3. 直接决定外部入口来源

一句话理解：

**`scripting` 负责“脚本如何安全执行”，而不是“请求从哪里来”。**

---

## 它在系统里的位置

当前主链路如下：

```text
Editor / Engine / Remote
  -> ScriptExecutionRequest
  -> ScriptHost::Submit(...)
  -> worker thread
  -> PythonRuntime
  -> (如有 Scene 操作)
       -> MainThreadCommandQueue
       -> Engine::Run(...) 中的 on_tick_scripting
       -> Scene
  -> ScriptExecutionResult
```

关键边界：

1. `ScriptHost` 是 scripting 的宿主入口
2. `PythonRuntime` 只在它的 owner thread 上工作
3. `Scene` 不允许被脚本 worker thread 直接跨线程操作
4. Scene 相关调用必须通过 `MainThreadCommandQueue`

---

## 目录结构

```text
scripting/
  ScriptingApi.h
  ScriptExecutionRequest.h
  ScriptExecutionResult.h
  ScriptExecutionFuture.h
  PythonRuntimeConfig.h/.cpp
  PythonRuntime.h/.cpp
  ScriptHost.h/.cpp
  SessionRegistry.h/.cpp
  MainThreadCommandQueue.h/.cpp
  SceneCall.h
  ScriptingModule.h/.cpp
  PybindEnttEntityCaster.h
  stubs/
```

推荐阅读顺序：

1. `ScriptExecutionRequest.h`
2. `ScriptHost.h/.cpp`
3. `PythonRuntime.h/.cpp`
4. `SessionRegistry.h/.cpp`
5. `MainThreadCommandQueue.h/.cpp`
6. `ScriptingModule.cpp`

---

## 核心类与职责

### `ScriptExecutionRequest`

统一描述一次脚本执行输入语义。

当前包含：

1. `origin`
2. `execution_kind`
3. `session_policy`
4. `session_id`
5. `source_name`
6. `code`

这是所有上层入口都应该复用的统一输入结构。

### `ScriptExecutionResult`

统一描述脚本执行结果。

当前包含：

1. `success`
2. `stdout_text`
3. `stderr_text`
4. `exception_text`

### `ScriptExecutionFuture`

对 `std::future<ScriptExecutionResult>` 的轻量包装。

它负责：

1. `valid()`
2. `get()`
3. `wait()` / `wait_for()`
4. 为后续 cancel 能力预留扩展点

注意：`cancel()` 目前只是预留接口，没有真正实现 ScriptHost 级取消。

### `ScriptHost`

模块最重要的入口类。

它负责：

1. 启动和停止脚本 worker thread
2. 持有 `PythonRuntime`
3. 持有 `SessionRegistry`
4. 接收 `Submit(...)` 请求
5. 在 worker thread 中执行脚本
6. 管理主线程 scene 命令队列

可以把它理解成：

**脚本执行的任务调度器和宿主。**

### `PythonRuntime`

嵌入式 Python 解释器包装层。

它负责：

1. 配置 Python home / executable / module search path
2. 初始化和关闭解释器
3. 维护 shared globals
4. 在指定 globals 上执行 snippet
5. 收集 stdout / stderr / exception 输出

最重要的约束：

1. 只能在 owner thread 上访问
2. 不能被任意线程直接拿来执行

### `SessionRegistry`

管理脚本执行上下文。

它负责：

1. `SharedGlobal`
2. `NamedSession`
3. `Stateless`

这里的“session”不是独立 Python 进程，而是不同的 Python globals dict。

### `MainThreadCommandQueue`

把脚本线程中的 Scene 操作安全提交到主线程。

它负责：

1. 接收 `Scene&` 相关命令
2. 在主线程稳定时机统一执行
3. 当 scene 不再可用时批量取消 pending 命令

### `ScriptingModule`

负责 Python 绑定模块及 Scene API 的接线。

当前最重要的事情有两件：

1. 暴露 `moer` Python 模块及绑定类型
2. 通过全局 active queue 把 Scene 相关 API 路由到 `MainThreadCommandQueue`

修改这里的 pybind11 绑定时，也必须同步维护 `stubs/moer/__init__.pyi`。

---

## 执行模型

### 1. 基本流程

```text
caller
  -> ScriptHost::Submit(request)
  -> PendingExecution queue
  -> ScriptHost worker thread
  -> ExecuteRequest(...)
  -> PythonRuntime::ExecuteSnippet(...)
  -> ScriptExecutionResult
```

### 2. 启动流程

`ScriptHost::Start()` 当前会做这些事：

1. 启动 worker thread
2. 在 worker thread 中初始化 `PythonRuntime`
3. 注册 active scene command queue
4. 执行 bootstrap 脚本，导入 `moer` 并准备 `scene = moer.scene()`
5. 用 bootstrap 后的 globals 初始化 `SessionRegistry`
6. 标记宿主进入 running 状态

这里有一个很重要的实现细节：

1. `NamedSession` 和 `Stateless` 的 seed globals 来自 bootstrap 后的 shared globals 拷贝
2. 也就是说，后续 session 默认都能看到基础 `moer` 绑定和 `scene` 入口

### 3. 停止流程

`ScriptHost::Stop()` 当前会：

1. 请求 worker thread 停止
2. 取消所有 pending scene 命令
3. 等待 worker thread 退出
4. 取消尚未执行的脚本请求
5. 清空 session registry
6. 清除 active scene command queue
7. 关闭 PythonRuntime

---

## 线程模型

这是最重要的维护点之一。

### 1. 脚本执行不在主线程

脚本代码默认在 `ScriptHost` 的 worker thread 上执行。

优点：

1. 不阻塞引擎主循环
2. 脚本执行和渲染主线程解耦

### 2. `PythonRuntime` 只允许 owner thread 访问

`PythonRuntime` 会记录初始化线程，并在每次执行时检查调用线程。

因此：

1. 不要从其他线程直接拿 `PythonRuntime` 执行
2. 不要试图把它暴露给任意异步任务直接使用

### 3. Scene 访问必须回到主线程

Python 脚本中的 Scene API 最终会通过 `MainThreadCommandQueue` 提交回主线程处理。

原因：

1. `Scene` 属于引擎主线程/帧循环拥有的对象
2. 脚本 worker thread 不能直接跨线程读写 `Scene`
3. renderer 切换和 shutdown 时 scene 可能暂时不可用

### 4. 引擎必须驱动 `ProcessMainThreadCommands()`

如果引擎主循环不调用 `ScriptHost::ProcessMainThreadCommands(scene)`，那么脚本里依赖 Scene 的命令就无法被实际执行。

当前 `Engine::Run(...)` 已经通过 `on_tick_scripting` 驱动这件事。

---

## Session 语义

### `SharedGlobal`

所有请求共用同一份 globals。

适合：

1. Editor 内部连续调试
2. 需要保留变量和 import 状态的场景

### `NamedSession`

按 `session_id` 复用一份独立 globals。

适合：

1. 多个终端客户端各自维护上下文
2. 同一客户端多次请求之间共享状态

### `Stateless`

每次请求都从 seed globals 克隆新上下文。

适合：

1. 一次性执行
2. 远程调用默认最小共享状态
3. 更容易预测执行结果

### 要特别说明的边界

这里的 session 是：

1. 不同的 Python globals dict

不是：

1. 不同 Python 解释器实例
2. 不同进程
3. 完全隔离的模块缓存或宿主全局状态

这意味着：

1. 顶层变量和顶层函数定义会按 globals 隔离
2. `sys.modules`、builtins、扩展模块里的宿主级状态并不会随 session 完全隔离

---

## 当前使用方式

### 最小使用步骤

```cpp
Moer::scripting::ScriptHost script_host(runtime_config);
script_host.Start();

Moer::scripting::ScriptExecutionRequest request;
request.source_name = "example.py";
request.session_policy = Moer::scripting::EScriptSessionPolicy::SharedGlobal;
request.code = "print('hello scripting')";

auto future = script_host.Submit(std::move(request));
auto result = future.get();

script_host.Stop();
```

### 在 Engine 中的典型接线

当前 `Engine` 会：

1. 在 init 时创建并启动 `ScriptHost`
2. 在 run loop 中调用 `ProcessMainThreadCommands(scene)`
3. 在 shutdown 或 renderer 切换时取消 pending scene commands

这套接线是目前最推荐的集成方式。

### 与 `remote` 的关系

`remote` 本身不执行 Python，它只是把外部请求转换成 `ScriptExecutionRequest` 并交给：

1. `Engine::SubmitScriptExecution(...)`
2. `ScriptHost::Submit(...)`

所以如果 remote 行为异常，往往要先确认 scripting 执行链路是否健康。

---

## 如何修改和新增功能

### 新增一种执行语义

例如以后要支持文件执行、模块调用等新的 `execution_kind`。

推荐改法：

1. 先扩展 `EScriptExecutionKind`
2. 再修改 `ScriptHost.cpp` 中的 `ExecuteRequest(...)`
3. 如有必要，扩展 `PythonRuntime`
4. 最后补测试和上游入口适配

不要绕开 `ScriptExecutionRequest` 单独新增一套散乱接口。

### 新增一种 session 策略

推荐改法：

1. 扩展 `EScriptSessionPolicy`
2. 在 `SessionRegistry` 增加对应上下文管理逻辑
3. 在 `ExecuteRequest(...)` 中接入新策略
4. 补 session 语义测试

### 新增 Scene 脚本 API

推荐改法：

1. 在 `ScriptingModule.cpp` 暴露新 binding
2. 如果需要操作 `Scene`，通过 `SceneCall` / `MainThreadCommandQueue` 路径提交到主线程
3. 同步更新 `stubs/moer/__init__.pyi`
4. 补 scene control 类测试

不要这样改：

1. 在 worker thread 中直接持有并操作 `Scene&`
2. 在 Python binding 里偷偷跨线程读写场景对象

### 修改 PythonRuntime 初始化方式

需要同时关注：

1. `PythonRuntimeConfig`
2. `PythonRuntime::Initialize()`
3. module search path
4. owner thread 约束
5. embed Python 的生命周期

这部分非常容易引入只在某些机器或目录结构下复现的问题，改完必须做真实运行验证。

---

## 代表性测试与继续阅读入口

优先看这些测试：

1. `source/test/scripting/TestScriptingSessionPolicy.cpp`
2. `source/test/scripting/TestScriptingSceneControl.cpp`
3. `source/test/remote/TestRemoteHttpExecuteSmoke.cpp`
4. `source/test/remote/TestRemoteEngineLoopbackDemo.cpp`

它们分别帮助理解：

1. 三种 session policy 的实际行为
2. 场景脚本控制与主线程命令队列
3. remote 到 scripting 的最小 execute 闭环
4. 引擎运行中的真实 loopback 场景

如果你想继续顺着代码追：

1. 看 `Engine.cpp` 里 `ScriptHost` 的创建和 `on_tick_scripting`
2. 看 `RemoteScriptService.cpp` 如何把 remote 请求转成 `ScriptExecutionRequest`
3. 看 `ScriptingModule.cpp` 如何暴露 `moer` 模块与 Scene API

---

## 常见误区

1. 不要把 session 误解为独立 Python 进程
2. 不要让脚本 worker thread 直接操作 `Scene`
3. 不要从非 owner thread 访问 `PythonRuntime`
4. 不要忘记 `Engine` 或宿主循环必须驱动 `ProcessMainThreadCommands()`
5. 不要修改 pybind11 绑定后忘记同步 `stubs/moer/__init__.pyi`
6. 不要假设 `ScriptExecutionFuture::cancel()` 已经具备真实取消能力

---

## 当前边界与扩展方向

当前模块保持了比较清晰的第一版边界：

1. 执行模型以 snippet 为主
2. 结果模型以一次性 `stdout / stderr / exception` 聚合为主
3. Scene 操作通过主线程命令队列串回主线程
4. future cancel 仍未真正落地

如果后续继续扩展，优先保持：

1. 所有上层入口统一复用 `ScriptExecutionRequest / Result`
2. 所有 Scene 相关脚本能力继续走主线程队列
3. PythonRuntime 的线程所有权约束不要被破坏

---

## 一句话心智模型

可以把 `runtime/scripting` 看成：

**一个由 `ScriptHost` 托管、在独立脚本线程中运行嵌入式 Python，并通过 session registry 与 main-thread scene command queue 连接引擎世界的执行模块。**