# Gaussian Raster 开发进度

## 2026-10-20：P0 采集工具已补齐，严格验收仍有未通过项

- [x] 从上游 main `018aca93e70f92992fa6306b2fc62382e03f963e` 创建 `feature/gaussian-raster`。
- [x] 保存 `GaussianRasterImplementationPlan.md`。
- [x] 建立 Clang 22.1.8、CMake 3.31.6、Ninja、Debug 本地构建环境。
- [x] 编译 MoerEditor 成功；Vulkan/Raster/Sponza 启动成功。
- [x] 验证层启用，首帧提交后持续运行超过一分钟，日志未发现运行/验证错误。
- [x] P0 固定相机/时间/配置、HDR/LDR 与 G-buffer 原始像素采集、上游参考构建、严格比较工具及控制资产。
- [ ] P0 严格图像验收：Control/Mizuki 两种管线已位一致；Sponza 上游存在间歇差异，240 帧额外预热也未稳定消除，暂不能将它作为确定性金标准。详见 `RasterBaselineValidation.md`。
- [x] P1：标准 PLY 解析、资产与实例、Scene API、独立更新快照和 GPU 数据链路。
- [ ] P2：标准高斯投影、排序、Splat 和参考图验证。
- [ ] P3：编辑器操作、持久化与图像导出。
- [ ] P4/P5：Mesh 转材质高斯及混合光栅化近似。

P1 已完成并验证。原 Mesh 数据布局和 shader 未修改；高斯数据接入不代表已经能显示高斯，绘制 Pass 在 P2。

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

## 下一步：先定位 P0 严格门禁，再进入 P2

- 先定位 Sponza 普通 PBR 的不确定性；保持 Mesh 数据与 shader 不变，不放宽比较阈值。
- P2 计划：为 GaussianOnly 接入标准投影 → tile 计数/扫描 → 配对/排序 → tile 区间 → 像素累积。
- 输出预乘颜色与透射率，使用独立相机/高斯 bounds，处理背景与参考颜色变换。
- 先验证解析样例，再使用真实标准 PLY 对照参考图；保留容量溢出检测。
- 暂不启用混合渲染和编辑器完整面板；Mesh 聚合与材质转换仍属于 P4/P5。

## 本地提交约定与本次记录

- Git 作者仅配置在本仓库：`Myrt1e <g2676536169@gmail.com>`，全局配置未修改。
- 按用户要求，每完成一个功能并进行适当验证后，本地提交一次；默认不推送远端。
- P0 工具和验证证据已提交：`9faced81`（`test(raster): add strict image baseline capture and validation evidence`）。这是工具/证据提交，不表示全部 P0 图像门禁已通过。
- P1 作为独立功能提交：标准 PLY、Scene/实例/快照、独立 GPU 数据链路、Python API 与契约测试。实现和验证范围见上文。
- 当前版本重新编译成功，`GaussianSceneDataContract` 再次通过（0.25 秒；CTest 总耗时 0.28 秒）。工作区测试编辑器均已关闭。
- P2 高斯绘制、编辑器完整面板、持久化和混合渲染仍未实现。
