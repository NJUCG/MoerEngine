# Gaussian Raster 开发进度

## 2026-10-20：P0 采集工具已补齐，严格验收仍有未通过项

- [x] 从上游 main `018aca93e70f92992fa6306b2fc62382e03f963e` 创建 `feature/gaussian-raster`。
- [x] 保存 `GaussianRasterImplementationPlan.md`。
- [x] 建立 Clang 22.1.8、CMake 3.31.6、Ninja、Debug 本地构建环境。
- [x] 编译 MoerEditor 成功；Vulkan/Raster/Sponza 启动成功。
- [x] 验证层启用，首帧提交后持续运行超过一分钟，日志未发现运行/验证错误。
- [x] P0 固定相机/时间/配置、HDR/LDR 与 G-buffer 原始像素采集、上游参考构建、严格比较工具及控制资产。
- [ ] P0 严格图像验收：Control/Mizuki 两种管线已位一致；Sponza 上游存在间歇差异，240 帧额外预热也未稳定消除，暂不能将它作为确定性金标准。详细采集报告仅保留在本地。
- [x] P1：标准 PLY 解析、资产与实例、Scene API、独立更新快照和 GPU 数据链路。
- [x] P2：标准高斯投影、排序、Splat 和参考图验证（2026-10-20）。
- [ ] P3：编辑器操作、持久化与图像导出。
- [ ] P4/P5：Mesh 转材质高斯及混合光栅化近似。

P1/P2 已完成：标准 PLY 可沿独立光栅路径输出图像。原 Mesh 数据布局和 shader 未修改。编辑器完整操作面板、持久化与混合渲染仍在后续阶段。

原始构建运行证据保存在本任务 outputs/MoerEngine-Baseline-Build-Run.md 及关联日志。

## P1 实现内容

- 专用 `LoadGaussianPly`：ASCII / binary little-endian、按属性名解析、SH 0–3 阶、log-scale 和 logit 转换、四元数归一化、源 SH 通道顺序转换。
- 错误处理：缺字段、重复字段、非法 SH 数量、截断、非有限数值、尺度溢出、零四元数、RHI 容量上限。空资产有效但不参与渲染。
- 不可变 GaussianAsset 与共享实例，独立几何/实例 GPU 布局。包围盒包含资产支撑范围及实例世界空间快照；旧 Mesh 聚合 bounds 没有改写，P2 使用独立高斯 bounds 适配相机。
- Scene API：导入、创建共享实例、获取资产/实例设置、更新设置、删除、Empty/MeshOnly/GaussianOnly/Mixed 分类。标准 `.ply` 接入通用加载与导入入口。
- 帧快照管理：相机无关的高斯资源更新、版本、可见性、世界变换和资产生命周期。高斯叶节点编辑不触发 Mesh/TLAS 更新。
- RenderScene 可选 GaussianGpuScene：独立 buffer，首次/资产变化时上传几何，实例变化只上传实例；无高斯的初始场景不创建高斯 GPU 资源。
- Scene Python 接口同步提供高斯状态、实例设置和删除，沿用原导入/变换 API；可用于后续编辑器接入和运行验证。
- 旧 Mesh 缓存暂不能保存高斯，因此明确拒绝，而不是静默丢失数据。真正持久化在 P3。

## P1 验证

1. Debug `MoerEditor`、`TestGaussianSceneData` 构建成功；`git diff --check` 通过。
2. `GaussianSceneDataContract` CTest 通过。覆盖解析/编码、所有支持 SH 阶数、共享几何打包、纯 PLY Scene、增量快照、参数拒绝、失败导入/加载的原子性、旧缓存拒绝保存、删除及历史帧持有资产。
3. 在实际 Vulkan/Raster/Sponza 编辑器中执行合成单高斯 PLY 导入，Scene 状态 MeshOnly → Mixed → MeshOnly；编辑位置和实例外观参数，再禁用并删除。
4. GPU 日志：revision 1 上传几何；revision 2/3 为实例变化，`geometry_upload=false`；revision 4/5 清除资源/实例。相关操作没有产生新的 Mesh RenderScene 更新日志。
5. 首帧 14:18:35 提交，运行至 14:21:06，正常关闭，退出码 0。验证层启用，日志未发现 error/critical/Validation Error/VUID。

上述 GPU 验证确认了实际资源创建/上传与生命周期，没有进行高斯 shader 读取或 buffer 字节读回；图像正确性属于 P2，不能据此声称已经渲染高斯或证明 Mesh 图像位一致。

本任务 outputs 中保存 `MoerEngine-Gaussian-P1-Runtime.log`、精简运行摘要、CTest 和构建日志。测试编辑器已正常关闭。

## 下一步：按用户决定，暂搁置 Sponza 波动，推进 P2

- 用户已决定暂不处理 Sponza 基线波动；保留原始失败报告，不把它标为通过，也不再用它阻塞后续开发。
- P2 计划：为 GaussianOnly 接入标准投影 → tile 计数/扫描 → 配对/排序 → tile 区间 → 像素累积。
- 输出预乘颜色与透射率，使用独立相机/高斯 bounds，处理背景与参考颜色变换。
- 先验证解析样例，再使用真实标准 PLY 对照参考图；保留容量溢出检测。
- 暂不启用混合渲染和编辑器完整面板；Mesh 聚合与材质转换仍属于 P4/P5。

## 本地提交约定与记录

- Git 作者仅配置在本仓库：`Myrt1e <g2676536169@gmail.com>`，全局配置未修改。
- 按用户要求，每完成一个功能并进行适当验证后，本地提交一次；默认不推送远端。
- 按用户要求，基线工具、观测源码、采集资产和详细验证证据仅本地保留，不纳入可发布的分支历史。原基线提交已撤下，渲染器默认代码不含观测调用。
- P1 作为独立功能提交：标准 PLY、Scene/实例/快照、独立 GPU 数据链路、Python API 与契约测试。实现和验证范围见上文。
- 当前版本重新编译成功，`GaussianSceneDataContract` 再次通过（0.25 秒；CTest 总耗时 0.28 秒）。工作区测试编辑器均已关闭。
- P2 高斯绘制、编辑器完整面板、持久化和混合渲染仍未实现。

## 开发手册命名整理

- 按 `docs/DEVELOPMENT.md` 整理本次新增 C++ 模块及测试中的变量名，保留历史 Mesh 模块命名。
- 基线常量改用 `k_` 前缀；观测状态对象成员使用 `m_`；布尔变量和参数使用 `is_` / `has_` / `should_`。
- 高斯 GPU Buffer 成员明确为 `m_gaussian_buffer` / `m_instance_buffer`；上传、PLY 解析及测试中的计数与索引补齐语义及 `_count` / `_id`。
- C++ 实例设置使用 `is_enabled`，Python 仍暴露原 `enabled` 属性。PLY 字段、抓图 JSON、日志字符串与 GPU 共享布局保持原样。
- 使用现有契约测试与固定 Mesh 控制场景验证重命名；不添加新渲染功能。
- 验证结果：MoerEditor / TestGaussianSceneData 编译通过；CTest 通过（0.57 秒）；控制场景 3 帧 × 4 层与重命名前逐字节一致，正常退出且未发现运行/验证错误。

## 弃用警告与本地基线清理

- 对照上游 `018aca93` 确认：两处 `fopen` 属于原有代码。按用户要求保持原样，不处理其既有警告。
- 两处 Profile 文件对话框中的 `u8path` 也属于上游原有代码，已恢复，不纳入本次改造。
- 本地基线工具读取环境变量时，Windows 使用 `_dupenv_s` 并自动释放拷贝；其他平台保留标准 `getenv`。不关闭编译器警告。
- 基线文件通过 `.git/info/exclude` 本地忽略。按需应用观测补丁，使用后撤回；生产渲染器不包含该补丁。
- 本地历史已重建：高斯接入 `1676ed4a`、命名整理 `0a797d3d`；旧历史的恢复备份只在仓库外的本地工作目录。没有推送远端。
- 本地工具的 `getenv` 修复通过了实际抓图验证，控制场景 12 个输出层与此前逐字节一致。原有 `fopen` / `u8path` 修改随后按用户要求全部撤回；允许保留上游既有警告，不以全仓库零警告为本次目标。

## 旧版 3DGS 清理

- 按用户要求删除废弃的 `shaders/features/3dgs/` 中全部 9 个旧版 Shader。
- 移除 deprecated `writeImage.hlsl` 中未使用的旧版头文件引用；更新 Shader 目录说明和 P2 实施计划。
- 当前 C++ 没有调用旧版 Shader，保留 P1 数据链路与原 Mesh 管线；P2 按当前架构重新实现。
- 验证：MoerEditor 编译成功，GaussianSceneDataContract 通过（0.50 秒）；源码无旧版 include 引用。同步清理生成目录中这 9 个 Shader 的残留副本。

## P2：独立标准高斯光栅路径（2026-10-20）

- 新增懒创建的 `GaussianSplatPass`，GPU 执行投影、tile 计数、分层前缀扫描、tile/depth 配对、稳定 radix 排序、区间构建和前向透明度累积。没有使用废弃的 3DGS 实现。
- GaussianOnly 使用独立 Pass，输出预乘 RGB/alpha、透射率和显示纹理；跳过 Mesh 光照、AO、AA、曝光和 tonemapping。标准 SH 保留烘焙外观语义。
- 相机和实例矩阵显式使用 HLSL row_major，对齐 Moer 的 CPU 矩阵存储；SH 在资产局部观察方向计算。标准训练 RGB 解码后进入硬件 sRGB UI 目标，原始 RGB 输出用于数值对照。
- 游戏线程捕获当前视口的内容分类；PLY 首次加载按高斯 bounds 适配相机。空资产、禁用或删除最后一个高斯时输出黑背景。
- MeshOnly 保留原有执行代码与资源布局。Mixed 暂时明确告警并显示诊断色；论文的材质高斯与 Mesh 转换不属于 P2。
- 默认 tile-pair 容量为 4,194,304，可通过 C++ `GaussianRasterSettings` 配置（范围 1..2^24）；展开实例的高斯数量上限为 2,097,152。扫描饱和计数、容量检查和诊断输出防止越界及静默丢点；暂不自动扩大容量。
- 相机、工作区、纹理及几何/实例资源持有到 GPU 完成，支持在途帧与 resize 重叠。实际编辑器运行发现的相机 Buffer 提前释放问题已修复。
- RenderGraph 开关不改变高斯算法：P2 的高斯分支串行记录 RHI 命令，尚未逐 Pass 纳入 RDG/并行录制；Mesh RDG 路径保持不变。

验证：

1. `MoerEditor`、`TestGaussianSceneData`、`TestGaussianRasterGpu` 构建成功，P1 CTest 通过。
2. GPU 集成测试验证单/双高斯、逆序与同深度的 600 高斯、多实例平移/缩放/opacity、提前终止、清空后 resize、8 帧在途 resize、sRGB 显示解码及容量溢出诊断。合成 RGB/T 对双精度 CPU 参考的最大误差约 2.2e-6；验证层日志无错误。
3. 真实资产：DX.GL Apple，36,313 Gaussian，SH3，8.6 MiB，数据集标注 CC0。来源 https://huggingface.co/datasets/dxgl/multiview-datasets ，下载 https://dx.gl/splat/apple.ply ，SHA256 `972ebac43d0ac46c375ba83d92be3866b337ae3d9a5307137617883e3ebee879`。资产仅本地保存。
4. 固定 256x256 视角，真实 PLY GPU 原始 RGB 与独立 NumPy 双精度参考对照：最大绝对误差 0.000538，RMSE 1.78e-6，PSNR 115.02 dB。此参考独立计算协方差/SH 并逐 splat 合成，不调用官方 INRIA 渲染器，不能据此声称与所有官方配置逐像素一致。
5. 固定 Mesh 控制场景的 linear / graph 两路，各 3 帧 × 4 层（HDR、LDR、base color、normal），与 P0 已验证版本逐字节一致。Sponza 既有波动仍按用户决定暂不处理。
6. 编辑器实测：同步模式与 Render/RHI 双线程 + RenderGraph 开关模式均可运行；纯 PLY 首次呈现、viewport resize、修改 opacity/平移、禁用和删除最后一个实例均成功。GPU 日志确认独立 Gaussian revision 更新，运行及正常退出无 Vulkan 校验错误。
7. 生产源码已撤去本地 Mesh 观测补丁；基线工具、第三方资产和运行证据均不进入 Git 提交。

运行 GPU 检查：在 `target/bin/Debug` 执行 `TestGaussianRasterGpu.exe [asset.ply capture.ppm]`。可选的真实资产对照脚本为 `source/test/gaussian/ReferenceGaussianPly.py`，依赖 NumPy/Pillow；未加入默认 CTest，避免要求无 GPU 的构建机运行 Vulkan 测试。

下一步为 P3：编辑器导入/Inspector、属性交互、持久化、图像导出及统计/诊断面板。混合材质与论文转换路径保留在 P4/P5。

## P3.1：高斯导入与 Inspector（2026-10-21）

- 文件选择器增加标准 3DGS PLY。导入到当前场景时后台解析 PLY，主线程原子创建实例；提供加载状态和取消，失败不修改 Scene。
- Hierarchy 使用 `[GS]` 标识；Inspector 复用原变换/可见性编辑，增加来源、SH 阶数、数量、局部 bounds、几何 Buffer 字节数（共享）、启用、颜色/opacity 乘数和重置。
- 新增 Scene 实例复制 API，保留父节点、局部变换、可见性和外观，继续共享不可变资产。高斯叶节点使用独立删除路径。
- 验证：Editor 构建与 GaussianSceneDataContract 通过；实际 Vulkan 编辑器持续运行，通过 Python API 复制真实 Apple、修改透明度/平移并删除，GPU revision 独立更新，日志未发现验证层错误。原生文件选择器交互不属于该自动化 API 冒烟验证。
- 这是 P3 的第一个功能提交；独立场景保存、显示控制、PNG 与统计尚在后续工作中。

## P3.2：独立高斯场景文档（2026-10-21）

- `.moergs` JSON 文档保存相对 PLY 引用、资产共享、节点层级/名称/TRS/可见性、实例启用/颜色/opacity、相机及方向/点/环境常量光。源 PLY 不被写入。
- File 菜单增加 Save Gaussian Scene As；Open Scene 可重载文档。文档不通过旧 Mesh cache，包含 Mesh/纹理资源时明确拒绝保存；文档导入到已有 Scene 暂不支持，避免静默丢失组件。
- 保存通过同目录临时文件完成后原子替换，限制 `.moergs` 扩展名；加载先在独立 LogicalScene 校验全部资产与数据，失败保留当前 Scene。恢复已保存相机时不执行 PLY 自动取景。
- 验证：Editor / GPU 测试目标构建通过，CPU 契约覆盖共享资产/参数/相对引用/覆盖保存、坏版本、缺失资产、循环层级与负乘数。真实 Apple 的两个实例保存、重启加载成功，日志确认只上传一份几何；重存文档与原文档一致，未发现 Vulkan 验证错误。
