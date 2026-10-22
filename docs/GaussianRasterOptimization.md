# 高斯光栅性能优化记录（2026-10-22）

本轮优化基于 `56017c767e6d20d64a5edd65bb7f60701c24d84b`。按用户 2026-10-22 的明确通知，将本轮优化统一做一次本地提交；本轮不推送。

## 改动

1. **Splat 分批共享读取。** 一个 16×16 tile 的线程协作加载最多 256 个高斯到 groupshared，再按原有排序逐像素合成。越界像素和已提前结束的像素仍参与同步与加载，整个 tile 都结束后才统一退出；不假设 wave 大小。
2. **保守剔除无贡献的 tile。** 在原有 tile 包围框内，计算二维高斯二次型在连续像素中心矩形上的最小值，结合 opacity 和原有 `1/255` alpha 阈值排除无贡献配对。细长旋转椭圆增加 FP32 保守余量，非有限结果保留；没有新增 3σ 椭圆截断。
3. **提前排除低 alpha 样本。** 投影时预计算带保守余量的对数阈值，Splat 在 exp 前排除低于阈值的样本；原有 exp、alpha clamp、排序及透射率终止规则保留。

Project 的计数与 EmitPairs 使用同一剔除函数和实际视口尺寸，覆盖边缘不满 16×16 的 tile。复用投影数据原先保留的 `center_depth.w`，结构体大小不变。原 Mesh 数据、Mesh 渲染分支、Scene 格式和 RHI 接口没有修改；本轮未实现混合渲染。

## 参考实现

- [gsplat 的共享批次合成](https://github.com/nerfstudio-project/gsplat/blob/512d366b67073d77ca099ede742683c165dfc23b/gsplat/cuda/csrc/RasterizeToPixels3DGSSerialBatchFwd.cu)（Apache-2.0）。
- [Slang Gaussian Rasterization 的共享批次合成](https://github.com/google/slang-gaussian-rasterization/blob/8f8498e14af84eb2e50e45a5ce9409eed71213fb/slang_gaussian_rasterization/internal/slang/alphablend_shader.slang)（Apache-2.0）。
- [FlashGS 的椭圆与 tile 相交思路](https://github.com/InternLandMark/FlashGS/blob/cdfc4e4002318423eda356eed02df8e01fa32cb6/csrc/cuda_rasterizer/preprocess.cu)（MIT）。本实现按当前渲染器的截断规则编写 HLSL，不引入 CUDA 依赖。
- Moer 历史分支 `3dgs-dev` 见 [PR #42](https://github.com/NJUCG/MoerEngine/pull/42)，2024-04-17 合入。旧 Splat 仍逐像素遍历全局数据，旧 RHI、排序及数据布局与当前路径不同，未恢复旧代码。

## 测量

固定 Miku 资产（259,821 个 SH3 高斯）、相机及 1500×1002 场景视口，RTX 5060 8 GB、i7-14700、驱动 610.88。Debug 构建，GPU shader 使用现有 DXC 优化配置；Vulkan 校验开启，GT/RT/RHI 多线程，关闭 VSync 和帧率锁。每轮暖机 10 秒后采样 30 秒，各三轮。

| 路径 | 三轮平均帧间隔（ms） | 对应渲染节奏（FPS） |
| --- | --- | --- |
| 优化前 | 159.960 / 92.586 / 94.626 | 6.252 / 10.801 / 10.568 |
| 仅共享批次 | 16.743 / 16.705 / 16.769 | 59.726 / 59.863 / 59.633 |
| 共享批次 + tile 剔除 + exp 前判断 | 8.705 / 6.896 / 9.460 | 114.879 / 145.015 / 105.707 |

三轮平均帧间隔的中位数从 94.626 ms 降至 8.705 ms，该固定用例吞吐约提高 10.87 倍。FPS 来自 CPU RenderFrame 开始时间间隔，不等于屏幕实际呈现频率或输入延迟。共享桌面的 WDDM 调度有波动，保留全部轮次，不以最好一轮作为结论。

该视角 GPU tile-pair 从 1,019,294 降至 515,191，减少约 49.46%，没有溢出；预分配容量仍为 4,194,304，工作区分配量没有随实际配对数缩小。最后另采一轮细分 GPU 时间：Project 0.302 ms、Scan 0.020 ms、Emit 0.141 ms、Sort 2.796 ms、Ranges 0.028 ms、Splat 6.549 ms；原先细分测量 Splat 约 82–87 ms。细分测量额外插入查询，会影响总帧时间，不与常规采样混用。最终非有限值保守判断加入后完成了细分采样和最终正确性验证，三轮常规采样在此判断加入前完成。

## 验证

- MoerEditor、三个高斯测试目标编译成功；GaussianSceneDataContract / GaussianEditorUiContract 两项 CTest 通过。
- GPU 测试新增 65×49 边缘 tile、513 层多批次及不同像素提前终止、1×1 视口、细长旋转高斯与接近 alpha 阈值的情况；继续覆盖排序、实例、在途 resize、预算变更、溢出和 PNG。
- 固定 1500×1002 编辑器视角，三轮共享批次和三轮组合优化的 PNG 均与优化前逐像素一致。
- Apple 固定视角的原始浮点 RGBA 与优化前完全一致。独立 NumPy 双精度参考：Apple PSNR 115.02 dB、最大 RGB 误差 0.000538；Miku PSNR 113.35 dB、最大 RGB 误差 0.000533。此参考并非官方 INRIA 渲染器。
- 最终无临时观测补丁的生产源码重新构建，Apple/Miku GPU 测试通过；实际编辑器成功呈现，运行日志没有发现 Vulkan 校验错误。本轮未重新采集 Mesh 金标准，既有 P3 Mesh 验证见进度记录。

测试资产、MPD、截图、采样脚本及细分观测补丁仅本地保存，不纳入版本控制。当前没有引入硬件计数器，不能据此断言性能损耗全部来自显存带宽。GPU radix 排序、indirect 调度和硬件 quad 路径属于后续可单独评估的优化方向，不在本轮实现中。

## 待观察问题

用户曾报告编辑器偶发闪烁、模型突然消失，随后暂未再次复现。初步检查运行日志未发现容量溢出或 Vulkan 校验错误，尚未定位原因，也没有针对该现象实施修复。固定截图及数值验证不能排除间歇性问题；后续复现时需继续区分高斯计算输出与编辑器合成显示路径。
