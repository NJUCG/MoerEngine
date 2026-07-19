# Nsight：ReSTIR Grid RIS `1/K` 寄存器对比

## 目的

确认把 `SampleLocalLightsForGrid` 中的 `1 / num_build_samples` 从循环内移动到最终补偿系数，是否真的减少 `PresampleLightGrid` compute shader 的物理寄存器占用。

只比较 NVIDIA 驱动最终生成的 GPU shader。HLSL 局部变量数量、DXIL/SPIR-V 临时变量数量和源码观感都不能单独证明寄存器下降。

## 本次环境

| 项目 | 值 |
| --- | --- |
| GPU | NVIDIA GeForce RTX 5070 Ti Laptop GPU |
| Driver | 595.97 |
| Nsight Graphics | 2025.5.0 |
| API | Vulkan 1.3 |
| Build | `target/bin/Debug/MoerEditor.exe` |
| Scene | `asset/scenes/sponza/Sponza.gltf` |
| Resolution | 1280 x 720，VSync off |
| Shader | `PresampleLightGrid.hlsl`，entry point `main`，`numthreads(256, 1, 1)` |

## A/B 代码

Baseline：每次候选循环都乘 `1/K`。

```hlsl
float inv_num_samples =
    1.f / float(_grid_params.common_params.num_build_samples);

for (uint i = 0; i < _grid_params.common_params.num_build_samples; i++) {
  // ...
  ctx.SelectNext(_rng, cur_light_info, rnd_light_idx, inv_pdf);
  inv_pdf *= inv_num_samples;

  float ris_weight = target_pdf * inv_pdf;
  weight_sum += ris_weight;
  // ...
}

float weight =
    (selected_target_pdf > 0.f) ? weight_sum / selected_target_pdf : 0.f;
```

Variant：循环中累计未归一化的 `T/q`，最后只归一化一次。

```hlsl
for (uint i = 0; i < _grid_params.common_params.num_build_samples; i++) {
  // ...
  ctx.SelectNext(_rng, cur_light_info, rnd_light_idx, inv_pdf);

  float ris_weight = target_pdf * inv_pdf;
  weight_sum += ris_weight;
  // ...
}

float weight = (selected_target_pdf > 0.f) ?
                   weight_sum /
                       (float(_grid_params.common_params.num_build_samples) *
                        selected_target_pdf) :
                   0.f;
```

统一的正比例缩放不改变 weighted reservoir 的选中概率，因此两种写法在数学上等价；浮点求值顺序不同，可能有末位舍入差异。

## 已生成的捕获

本次使用独立 Raytracing 配置，未修改根目录 `MoerEngine.toml`。原 shader cache 在实验前备份，实验后已恢复；运行时 shader 副本也已恢复为源码版本。

| 版本 | GPU Trace Report | 捕获状态 |
| --- | --- | --- |
| Baseline | `target/nsight-restir-grid/baseline-gpu-trace-admin/MoerEditor_2026_07_19_23_08_57.ngfx-gputrace` | 采集成功，但运行时回退到 Power RIS，不含 Grid dispatch |
| moved `1/K` | `target/nsight-restir-grid/moved-1-over-k-gpu-trace-admin/MoerEditor_2026_07_19_23_12_03.ngfx-gputrace` | 采集成功，但运行时回退到 Power RIS，不含 Grid dispatch |

两次都在 Raytracing Sponza 稳态运行 60 秒后自动捕获。捕获 HUD 和最终文件确认注入成功，capture metadata 显示同一 GPU、驱动、API、分辨率和启动参数。

## 严格对比流程

### 0. 强制运行 Grid 路径

默认开启 `Adaptive Grid Fallback`，当局部灯数量小于阈值 64 时，运行时会把 Grid
回退到 Power RIS。此时报告里只有 `PresampleLight`，不会出现 `PresampleLightGrid`，该
捕获不能用来衡量 `SampleLocalLightsForGrid`。

采集前必须同时满足：

1. `ReSTIRDI` -> `InitialSampleSettings` -> `LocalLightSelection` 选择 `Grid`。
2. 取消勾选 `Adaptive Grid Fallback`；自动采集时也可给目标进程设置环境变量
   `MOER_RT_FORCE_LIGHT_GRID=1`。
3. 先用一次捕获确认 Event List 中确实出现 `PresampleLightGrid`，再开始正式 A/B。

### 1. 放开 GPU counter 权限

GPU Trace 的 Shader Profiler 需要 GPU performance counter 权限。二选一：

1. 仅本次测试：以管理员身份运行 Nsight Graphics。
2. 长期开发机：NVIDIA Control Panel -> `Developer` -> `Manage GPU Performance Counters` -> `Allow access to the GPU performance counters to all users`。

权限未满足时，CLI 会报告：

```text
TARGET ERROR: GPU Performance Counters unavailable.
Please enable access to GPU performance counters.
```

### 2. 配置 GPU Trace

在 Nsight Graphics 中选择 `GPU Trace Profiler`：

| 设置 | 值 |
| --- | --- |
| Executable | `target/bin/Debug/MoerEditor.exe` |
| Working Directory | `target/bin/Debug` |
| Arguments | `--config=<绝对路径>/target/bin/Debug/MoerEngine.nsight-restir.toml` |
| Architecture | `Blackwell GB20x` |
| Metric Set | `Top-Level Triage`，ID 0 |
| Real-Time Shader Profiler | On |
| Collect Shader Pipelines | On |
| Start After | 30000 ms，等场景与后台编译稳定 |
| Max Duration | 1000 ms |
| Limit To | 20 frames |
| Lock Clocks | A/B 必须相同；做耗时对比时建议 Base |

CLI 等价命令：

```powershell
$ngfx = 'C:\Program Files\NVIDIA Corporation\Nsight Graphics 2025.5.0\host\windows-desktop-nomad-x64\ngfx.exe'
$exe = '<repo>\target\bin\Debug\MoerEditor.exe'
$dir = '<repo>\target\bin\Debug'
$config = '<repo>\target\bin\Debug\MoerEngine.nsight-restir.toml'
$env:MOER_RT_FORCE_LIGHT_GRID = '1'

& $ngfx `
  --activity 'GPU Trace Profiler' `
  --platform Windows `
  --exe $exe `
  --dir $dir `
  --args "--config=$config" `
  --start-after-ms 30000 `
  --max-duration-ms 1000 `
  --limit-to-frames 20 `
  --architecture 'Blackwell GB20x' `
  --metric-set-id 0 `
  --real-time-shader-profiler `
  --auto-export
```

### 3. 在报告中定位 shader

最快查看路径：

1. 双击 baseline 的 `.ngfx-gputrace`，等待报告解析完成。
2. 在左侧 `Event List` 展开 `Graphics Exec` -> 某一帧 -> `LightingPass` -> `PresampleLightGrid`。
3. 选中 `PresampleLightGrid` 下的 `vkCmdDispatch`，不要只选整帧或 `Graphics Exec`。
4. 如果 marker 不容易找到，在 `Event Search` 搜索 `8192`，选择
   `vkCmdDispatch(groupCountX = 8192, groupCountY = 1, groupCountZ = 1)`。当前配置中
   `4096 cells * 512 slots / 256 threads = 8192 groups`；如果以后修改 grid 参数，需重新计算这个值。
5. 点击窗口底部的 `Shader Pipelines` 标签。此时表格应该只剩当前 dispatch 使用的 compute pipeline。
6. 横向滚动表格，记录 `Hash`、`# Reg`、`# Warp`、`Smem`、`CTA Dim`。`# Reg` 是最终驱动机器码每线程分配的寄存器数，是本实验的主指标。
7. 对 variant 报告重复相同步骤，并确保选中的 dispatch、CTA Dim 和运行配置一致。

未生成 shader debug info 时，`File Name` 可能显示为 `comp.10000.spv (...)`，而不是
`PresampleLightGrid.hlsl`；这不表示选错 shader，应以 marker、dispatch 维度和 pipeline hash
共同确认。`Samples` 是 Shader Profiler 的采样数，不是该 dispatch 的执行时间。

### 4. 判定标准

| 指标 | 如何判断 |
| --- | --- |
| `# Reg` | 每线程物理寄存器数；只有该值下降，才能说寄存器占用下降 |
| `# Warp` | 理论驻留 warp 数；增加才说明寄存器下降跨过 occupancy 档位 |
| 限制 tooltip | 悬停 `# Warp`，确认限制资源是否从 registers 变化 |
| Local/Scratch | 必须保持 0；下降寄存器却产生 spill 通常是负优化 |
| Shader duration | 同配置多次采样看中位数；单帧差异不能下结论 |
| 输出图像 | A/B 应保持视觉一致，排除公式或 cache 切换错误 |

如果 `# Reg` 相同，则编译器已经完成等价的缩放移动，或者该临时量未决定最终寄存器分配档位。此时最多只能把改动视为源码/ALU 清理，不能宣称降低寄存器压力。

## 本次状态

关闭 Adaptive Grid Fallback 后，手动选择同一个 `PresampleLightGrid` compute pipeline，
CTA Dim 均为 `256 x 1 x 1`，得到：

| 版本 | `# Reg` | `# Warp` | Live Registers | Smem | Instruction Mix |
| --- | ---: | ---: | ---: | ---: | ---: |
| Baseline | 46 | 40 | 43 | 0 | 1.24K |
| moved `1/K` | 47 | 40 | 44 | 0 | 1.25K |

修改版每线程多分配 1 个物理寄存器，峰值活跃寄存器也增加 1；理论驻留 warp 数仍为
40，因此没有 occupancy 收益。`Instruction Mix` 的界面显示值也没有下降。该列经过取整，
不能把 `0.01K` 直接解释成精确增加 10 条 ALU 指令；但它至少不支持“指令减少”的结论。

截图没有显示 Local/Scratch 或 spill 指标，`Smem = 0` 只代表没有使用 shared memory，不能
据此断言没有寄存器 spill。

本机第一次采集曾被 GPU counter 权限拒绝；以管理员身份运行 Nsight Graphics 后，两份
GPU Trace 均已成功生成。但报告只包含 `PresampleLight` 和 `PresampleEnvMap`，说明自适应
逻辑把 Grid 回退到了 Power RIS。这两份报告不能回答 Grid shader 的寄存器问题，必须强制
Grid 后重新采集。上表来自随后强制 Grid 的手动 A/B 捕获。

结论：把统一的 `1/K` 从候选循环移到最终补偿不会改善本 shader 的寄存器压力，当前
NVIDIA 驱动编译结果增加了 1 个寄存器，但没有改变 `# Warp`。工程上保留 moved `1/K`
写法，以便代码直接对应 RIS 统计量；这是可读性取舍，不应宣传为寄存器或性能优化。

## 参考

- [Nsight Graphics 2025.5 Shader Profiler](https://docs.nvidia.com/nsight-graphics/2025.5/UserGuide/shader-profiler.html)
- [Nsight Graphics 2025.5 GPU Trace](https://docs.nvidia.com/nsight-graphics/2025.5/UserGuide/gpu-trace-overview.html)
- [NVIDIA：GPU Performance Counter 权限错误](https://developer.nvidia.com/nvidia-development-tools-solutions-err-nvgpuctrperm-nsight-graphics)
