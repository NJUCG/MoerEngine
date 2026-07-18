# RenderDoc GPU Marker 指南

MoerEngine 的 Vulkan 命令流使用统一的 GPU marker 层级。抓取一帧后，RenderDoc 的 Event Browser 应能直接按“帧 → Renderer → RenderGraph/流水线 → Pass → Subpass/计时区间”浏览，而不需要从裸 `vkCmd*` 事件反推渲染阶段。

## 预期层级

Raster 帧的主干结构如下（实际 pass 会随配置变化）：

```text
Raster Frame <frame_id>
├─ Raster Renderer
│  └─ Raster RenderGraph: RasterFrame
│     ├─ Pass: ShadowDepth
│     │  └─ Raster ShadowDepthPass
│     │     ├─ Point Shadow Face +X
│     │     │  ├─ Raster PointShadow Culling +X
│     │     │  └─ Raster PointShadow Draw +X
│     │     ├─ Point Shadow Face -X
│     │     ├─ Point Shadow Face +Y
│     │     ├─ Point Shadow Face -Y
│     │     ├─ Point Shadow Face +Z
│     │     └─ Point Shadow Face -Z
│     ├─ Pass: Geometry
│     │  └─ Raster GeometryPass
│     ├─ Pass: Lighting
│     └─ Pass: UiCombine
│        └─ UI Composition
└─ Editor UI
   └─ Main Viewport

Present: <source texture name>
```

关闭 RenderGraph 时，中间节点会变为 `Raster Linear Pipeline`，pass 层级保持一致。Raytracing 路径使用 `Raytracing Frame <frame_id> → Raytracing Renderer → <stage>` 的对应结构。

Point Cube 的面顺序固定为 `+X, -X, +Y, -Y, +Z, -Z`。每个面的 marker、culling 计时 key 和 draw 计时 key 都彼此独立，也不与 CSM key 复用。

## 抓帧与检查

1. 在根目录 `MoerEngine.toml` 中选择要检查的 renderer。
2. 用 RenderDoc 以 Vulkan API 启动 `target/bin/Debug/MoerEditor.exe`。
3. 等待场景和 shader 完成加载，再抓取稳定帧。
4. 在 Event Browser 中展开帧根节点；搜索 `Pass:`、`Point Shadow Face` 或具体计时 key。
5. 选择 draw/dispatch 后，通过 Pipeline State、资源和 Shader Debug 查看详细状态。

若 Event Browser 出现 `[RHI Diagnostics] Command Layers`，说明启用了 RHI 命令重排诊断采样；正常抓帧不会显示内部 `Layer N` 节点。

## 编写 marker

高层渲染代码优先使用 RAII guard：

```cpp
ScopedGpuMarker pass_marker(
    cmd_list,
    "Pass: MyPass",
    GpuMarkerPalette::Pass()
);
```

需要 GPU 时间的区间显式选择 timestamp 模式：

```cpp
ScopedGpuMarker timed_scope(
    cmd_list,
    "Raster MyPass",
    GpuMarkerPalette::Scope(),
    EGpuMarkerMode::Timestamp
);
```

提交级根节点通过 `CmdSubmit` 设置：

```cpp
queue.Execute(
    cmd_list.Submit()
        .DebugLabel("Raster Frame 42", GpuMarkerPalette::Frame())
        .TickProfiling()
);
```

`ScopedGpuMarker` 不可复制、不可移动。它必须比内部 marker 活得更久，并在 `CommandList::Submit()` 之前析构或显式 `Close()`。Debug 构建会检查 scope 欠栈、类型不匹配、未闭合提交等错误；Release 构建会尽量安全收束损坏的 scope 栈。

## 命名与粒度约定

- `Frame`：提交根节点，可包含动态 frame id，不作为 profiler key。
- `Renderer`：Raster、Raytracing 或 UI 等大阶段。
- `RenderGraph`：RenderGraph 或线性流水线容器。
- `Pass`：稳定的渲染 pass 名，统一使用 `Pass: <name>`。
- `Subpass`：cascade、cube face、denoise stage 等可读子阶段。
- `Timestamp`：同时产生 GPU timestamp 的稳定 profiling key。
- `Transfer`：RHI 自动生成的 upload、copy、clear 等原生命令标签。

同一次 `CommandList::Submit()` 中，每个 timestamp key 必须唯一。不要把 frame id、对象地址等动态内容放进 timestamp key；需要区分重复工作时，应使用 `CSM0`、`+X` 等稳定后缀。Debug 构建会对重复 key 断言，Release 构建会把后续重复项降级为纯可视 marker，避免覆盖 profiler 数据。

RHI scope 是命令重排器的独占层边界，能保证 barrier 和命令落在正确 marker 内。代价是过细的 marker 会限制重排空间，因此应在 pass/subpass 级使用；逐 draw/逐 dispatch 的诊断优先依赖自动命令名或临时调试 marker。

## 验证

Marker 机制的回归测试：

```powershell
cmake --build build --target TestRHICommandMarker TestRHICmdReorderer TestRHIRecordDiagnostics --config Debug -j 30
target/bin/Debug/TestRHICommandMarker.exe
target/bin/Debug/TestRHICmdReorderer.exe
target/bin/Debug/TestRHIRecordDiagnostics.exe
```

涉及真实层级或颜色的修改仍需至少抓取一帧，在 RenderDoc 中确认结构、六个 point-cube 面和 pass 颜色。
