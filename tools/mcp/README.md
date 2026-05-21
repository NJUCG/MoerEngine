# tools/mcp

`tools/mcp` 是 `MoerEngine` 的 MCP bridge 入口目录。

它当前的职责不是重新实现一套场景接口，而是：

1. 通过 `stdio` 接入 `VS Code` 的 MCP host
2. 通过现有 `runtime/remote` HTTP 接口桥接引擎能力
3. 把依赖安装与缓存逻辑收口在 launcher 中

---

## 1. 当前实现形态

当前启动链路如下：

```text
.vscode/mcp.json
  -> 3rdparty/python312/x64/python.exe
    -> tools/mcp/launcher.py
      -> .cache/mcp/site-packages
      -> tools/mcp/server.py
        -> tools/mcp/engine_client.py
          -> GET /api/remote/status
          -> POST /api/script/execute
```

当前目录结构职责如下：

1. `launcher.py`
   负责检查并自举 MCP Python 依赖到工作区缓存目录，再切换到真正的 server
2. `server.py`
   负责注册 MCP tools，并把请求桥接到现有 `runtime/remote` HTTP 接口
3. `engine_client.py`
   负责封装 `GET /api/remote/status` 和 `POST /api/script/execute`
4. `requirements.lock.txt`
   记录当前 MCP bridge 的 Python 直接依赖版本

运行期缓存会落到下面目录：

```text
.cache/mcp/
  site-packages/
  pip-cache/
  logs/
  install.lock
  install.stamp.json
```

说明：

1. 根目录 `.gitignore` 已忽略 `/.cache/`
2. 这意味着依赖下载和安装结果不会污染仓库状态
3. `launcher.py` 不能往 stdout 输出普通日志，否则会污染 MCP 的 stdio 协议流
4. `server.py` 当前显式写死了 MCP server version
   原因：`FastMCP 1.27.1` 在当前 `--target` 安装模式下，metadata fallback 会拿到空版本，导致初始化失败

---

## 2. 当前工具状态

当前 4 个工具里，只有前 2 个已经接了真实行为：

1. `engine_ping`
   已接通 `GET /api/remote/status`
2. `python_execute`
   已接通 `POST /api/script/execute`
3. `scene_summary`
   仍是占位实现
4. `scene_query`
   仍是占位实现

换句话说，当前 MCP 已经是“能真实控制引擎”的状态，但 scene 查询面还没做完。

---

## 3. 已确认的运行特性

这部分是当前 MCP 接入最重要的行为结论。

### 3.1 `python_execute` 当前是阻塞式请求

`python_execute` 不是 fire-and-forget，也不是流式脚本执行。

它当前走的是：

```text
MCP tools/call
  -> POST /api/script/execute
    -> RemoteScriptService::ExecuteAndWait(...)
      -> ScriptHost::Submit(...)
      -> future.get()
```

这意味着：

1. 只有当脚本返回后，MCP 请求才会结束
2. 如果脚本里有长时间 `sleep`
3. 如果脚本里是长循环或无限循环
4. 那么当前 HTTP 请求和当前 MCP tool call 都会一直挂住

所以当前这条路径适合：

1. 一次性脚本执行
2. 短时场景修改
3. 查询+改动+返回结果的同步工作流

不适合直接承载：

1. 长时间动画播放
2. 持续 stdout streaming
3. 需要中途 cancel 的长任务

如果未来要正式支持这些能力，需要额外设计：

1. 非阻塞 remote 执行模型
2. 增量 stdout / stderr
3. cancel
4. 可能还需要引入 WebSocket 或事件订阅路径

### 3.2 `session_policy` 直接复用 scripting 语义

当前 MCP 没有重新发明会话模型，而是直接复用 `runtime/scripting`：

1. `SharedGlobal`
2. `NamedSession`
3. `Stateless`

实际体验上：

1. `Stateless` 最适合一次性执行
2. `NamedSession` 可以在多次 MCP 调用之间保留 Python globals 状态
3. 这意味着 `NamedSession` 可以保留变量、对象、甚至实验性的后台线程状态

### 3.3 `NamedSession` 可保留后台状态，但持续动画仍未验证完成

已经验证过：

1. 在 `NamedSession` 中保存自定义状态是可行的
2. 在 `NamedSession` 中创建后台线程也是可行的
3. 后续再次查询时，能读回变化中的 transform 数值

但当前还有一个未解决现象：

1. 通过后台线程以 60Hz 持续调用 `set_node_translation(...)`
2. 查询到的 transform 数值确实在变化
3. 但引擎视图中没有观察到预期的持续运动

因此目前不要把“后台线程 + `NamedSession` + Scene API”视为已经可靠的正式动画方案。

一句话总结：

**MCP 当前已经适合做同步控制和短时脚本执行，但不应假设它已经具备稳定的持续动画能力。**

---

## 4. 当前已验证可用的 Scene Python API

下面这些 API 已经在真实脚本或 stub 中确认可用。

### 4.1 Scene 状态与入口

1. `moer.scene()`
2. `scene.is_ready()`
3. `scene.get_source_file_path()`
4. `scene.get_root_node_entity()`
5. `scene.is_valid_entity(...)`
6. `scene.is_valid_node_entity(...)`
7. `scene.is_root_node(...)`

### 4.2 Node 查询

1. `scene.get_node_display_name(...)`
2. `scene.try_get_node_name(...)`
3. `scene.try_get_node_local_transform(...)`
4. `scene.get_node_subtree_stats(...)`
5. `scene.get_node_child_count(...)`
6. `scene.find_node_entity_by_name(...)`

### 4.3 Main 节点入口

1. `scene.get_main_camera_entity()`
2. `scene.get_main_directional_light_entity()`
3. `scene.get_main_point_light_entity()`

### 4.4 Node 修改

1. `scene.set_node_name(...)`
2. `scene.set_node_translation(...)`
3. `scene.set_node_rotation(...)`
4. `scene.set_node_scale(...)`
5. `scene.set_local_transform(...)`
6. `scene.attach_to_parent(...)`
7. `scene.detach_from_parent(...)`

### 4.5 创建接口

1. `scene.create_entity(...)`
2. `scene.create_entity_with_node(...)`
3. `scene.create_point_light(...)`
4. `scene.create_procedural_renderable(...)`
5. `scene.import_scene_from_file(...)`

### 4.6 删除接口

1. `scene.destroy_entity(...)`
2. `scene.destroy_node_subtree(...)`
3. `scene.destroy_renderable(...)`
4. `scene.destroy_point_light(...)`

### 4.7 常用绑定类型

1. `moer.float3`
2. `moer.float4`
3. `moer.Quaternion`
4. `moer.Transform`
5. `moer.EntityWithNodeCreateInfo`
6. `moer.PointLightCreateInfo`
7. `moer.ProceduralMeshCreateInfo`
8. `moer.MaterialCreateInfo`
9. `moer.NodeLocalTransform`
10. `moer.NodeSubtreeStats`

---

## 5. 当前明显缺口

当前 Scene Python API 还不够完整，这也是 `scene_summary` / `scene_query` 还没接真的主要原因。

当前已知缺口包括：

1. 没有按索引或迭代方式枚举 child node entity 的接口
2. 只有 `get_node_child_count(...)`，没有 `get_node_child_entity(...)`
3. 没有直接列举场景中 renderable entity 的接口
4. 没有从 root 递归遍历整棵 scene graph 的现成 Python API
5. 没有通用“按条件查实体”的高层查询接口

这意味着当前最稳的控制路径仍然是：

1. 已知 entity 时直接改
2. 已知名字时 `find_node_entity_by_name(...)`
3. 需要明确目标时，优先创建 procedural renderable 再操作

---

## 6. 已完成的真实 MCP 验证

下面这些事情已经通过真实 MCP 调用验证成功：

1. `engine_ping` 可以返回 remote 状态、绑定地址和端口
2. `python_execute` 可以执行简单 Python 脚本并返回 stdout
3. 通过 `python_execute` 创建 procedural cube mesh 成功
4. 通过 `python_execute` 对 procedural cube 做单次平移成功
5. 通过 `python_execute` 批量创建多组 cube 成功
6. `NamedSession` 中的状态可以被下一次 MCP 调用复用

这些验证说明：

**当前 MCP 已经足够支撑“短时脚本控制引擎”的工作流。**

---

## 7. 手工验证方式

手工验证 launcher 时，可以执行：

```powershell
& .\3rdparty\python312\x64\python.exe .\tools\mcp\launcher.py --check
```

`--check` 当前不仅会检查依赖，还会验证：

1. 缓存依赖是否可导入
2. `server.py` 是否可导入
3. MCP initialization options 是否能成功构建

如果 bootstrap 失败，请查看：

```text
.cache/mcp/logs/bootstrap.log
```

---

## 8. 维护建议

后续如果 agent 继续扩 MCP，建议优先遵守下面几条：

1. 不要把 pip 依赖装回 `3rdparty/python312/x64/Lib/site-packages`
2. 继续通过 `launcher.py` + `.cache/mcp` 管理运行期依赖
3. 在 scene 查询面没有补全前，不要高估 `scene_summary` / `scene_query` 的可实现深度
4. 任何长时间脚本都要先考虑当前 blocking HTTP 模型是否合适
5. 如果要做动画、streaming、cancel，优先从 `runtime/remote` 的模型能力补起，而不是只在 MCP 层绕工作流