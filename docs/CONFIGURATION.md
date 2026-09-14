# MoerEngine 配置

仓库根目录的 `MoerEngine.toml` 是开发时的配置源。构建会把它复制到
`target/bin/<Config>/MoerEngine.toml`，编辑根配置后需要重新构建。也可以用
`--config <path>` 直接指定另一份 TOML。

普通的编辑器、场景和后端选择仍使用原有结构化字段。线程、RHI 提交和 RDG
这类启动参数同时注册为 canonical CVar，因此调整它们不再需要修改
`GlobalConfig`、TOML loader 和各后端配置结构。

## 覆盖顺序

启动时按以下顺序合并，后者优先：

1. 原有 `[engine.*]` 字段；
2. 当前 TOML 的 `[cvars]`；
3. `--profile` 选中的 `[profiles.<name>.cvars]`；
4. 命令行 `--set Name=Value`。

例如：

```toml
[cvars]
"RHI.CommandRecording.Parallel" = true
"RHI.CommandRecording.Parallel.Workers" = 0
"RHI.Submission.BatchWindow" = 2

[profiles.rdg-parallel.cvars]
"Render.Raster.RDG.Enabled" = true
"Render.Raster.RDG.ParallelRecording" = true
"RHI.CommandRecording.Parallel" = true
```

```powershell
MoerEditor.exe --profile rdg-parallel
MoerEditor.exe --set Render.Raster.RDG.Enabled=true --set RHI.Submission.BatchWindow=1
```

这些设置在启动结束时被冻结，并生成一份强类型快照。RHI 与渲染器只读取该
快照，不会在执行过程中反复读取 TOML。控制台中的实时 CVar 仍按帧边界应用。

## 当前 startup CVar

- `Engine.Threading.RenderThread`
- `Engine.Threading.RHIThread`
- `Engine.Threading.RHIBypass`
- `Engine.Threading.ProfileLogging`
- `Engine.Threading.MaxFrameLag`
- `RHI.CommandRecording.Parallel`
- `RHI.CommandRecording.Parallel.Workers`
- `RHI.CommandRecording.Parallel.Verify`
- `RHI.CommandRecording.Parallel.Profile`
- `RHI.CommandRecording.Parallel.MinWorkUnitsPerJob`
- `RHI.Submission.BatchWindow`
- `RHI.Heartbeat.Enabled`
- `RHI.Heartbeat.StallTimeoutMs`
- `RHI.Heartbeat.PollIntervalMs`
- `Render.Raster.RDG.Enabled`
- `Render.Raster.RDG.DebugDump`
- `Render.Raster.RDG.ParallelRecording`
- `Render.Raytracing.RDG.Enabled`
- `Render.Raytracing.RDG.DebugDump`
- `Render.Raytracing.RDG.ParallelRecording`

`RHI.Submission.BatchWindow.PolicyClamped` 是只读诊断值，用于查看策略钳制后的
窗口。所有 startup CVar 都会在初始化结束后封存；若要临时试验，优先用
`--set`，若要保存一组组合，使用命名 profile。
