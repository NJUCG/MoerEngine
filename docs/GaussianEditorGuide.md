# 高斯编辑与导出（Raster）

本阶段支持标准 3DGS PLY 的独立光栅渲染，不支持重光照、训练或 Mesh/高斯混合渲染。

## 从 PLY 开始

1. 使用 Raster 渲染器，File → Open Scene，选择 `.ply`。这会替换当前场景，并按资产包围盒设置初始相机。
2. Hierarchy 中 `[GS]` 是高斯实例。选中后，在 Inspector 编辑 Position / Rotation / Scale、可见性、Enabled、Opacity multiplier 和 Color multiplier。
3. Duplicate instance 共享原始几何，可独立修改变换/外观。Reset appearance 只恢复实例乘数与启用状态，不改 PLY。
4. Import Into Current Scene 在后台解析 PLY，成功后添加实例；加载时可取消。导入 Mesh 场景将进入当前尚未实现的 Mixed 状态，显示诊断色；纯高斯预览请用 Open Scene 打开 PLY。

标准 SH 外观已经包含训练时的光照。颜色乘数和显示曝光都是外观/显示调节，不是新增灯光的响应。

## 渲染设置

Configs → Gaussian Rendering：

- Reference RGB：默认黑背景，保留 P2 的标准算法与显示 RGB 语义，跳过 Mesh 光照、AA、曝光与色调映射。
- Engine preview：对显示颜色进行线性曝光和 Reinhard 映射，不能当作未处理的参考输出比较。
- Background：参考模式下按显示 RGB 指定背景。
- View：Color / Transmittance / Tile coverage。Tile coverage 按每个 16×16 tile 的 splat 配对数量显示对数热图。
- Frame Gaussian bounds：重新取景全部启用且在任一视口可见的高斯 bounds。
- Tile-pair capacity：默认 4,194,304，允许 1..16,777,216。超过预算时整帧诊断，不静默裁掉 splat。展开实例的高斯数量另有限制 2,097,152；增加配对预算不能解除这个限制。

统计显示最近完成的 GPU 计数/耗时，非阻塞读取，可能落后于当前画面。几何按资产共享；工作区字节数包含纹理估算，不代表整个引擎或驱动的显存分配。

## 保存、重开与 PNG

- File → Save Gaussian Scene As 保存 `.moergs`。保存相对 PLY 引用、实例/层级、可见性、Game 相机、支持的灯光参数与显示设置，不改源 PLY，不嵌入数百万高斯。
- 把文档和引用 PLY 一起保留，源文件内容变化后重开会读取新内容。可以移动整组文件，只要相对关系保持一致。
- File → Open Scene 重开 `.moergs`，恢复 Game 相机，不再进行 PLY 的自动取景。编辑器 Scene 视口的临时相机不写入文档。
- 包含 Mesh/纹理资源的场景不会保存为 `.moergs`，避免遗漏数据；旧 Mesh State Cache 仍拒绝高斯。`.moergs` 不支持作为普通场景片段导入，请导入 PLY 或用 Open Scene。
- File → Export Gaussian PNG 导出当前视口、当前显示模式/调试视图，使用视口分辨率，RGB8 sRGB，含背景、不含 UI。请求时固定显示设置，GPU 异步读回后在后台编码。
- 导出状态显示在 File 菜单及 Gaussian Rendering 面板。容量溢出、混合模式、写入失败会返回错误；无效帧不会替换已有 PNG。完成前请等待状态。

文档和 PNG 先写临时文件，再发布到目标路径；文档引用缺失、格式/版本错误或参数无效时不替换当前场景。

## 验证

构建目标：`MoerEditor TestGaussianSceneData TestGaussianEditorUi TestGaussianRasterGpu`。

- CTest：`GaussianSceneDataContract` / `GaussianEditorUiContract`，覆盖参数、保存重载、错误保留、导出请求状态与 Inspector 绘制/失效选择。
- 在 `target/bin/Debug` 执行 `TestGaussianRasterGpu.exe [asset.ply capture.ppm]`，要求可用 Vulkan GPU，覆盖投影/排序/透明度、显示变换、统计、在途 resize/预算变更和 PNG 像素/错误保护。
- 真实 PLY 数值对照：`source/test/gaussian/ReferenceGaussianPly.py`（NumPy/Pillow）。这是独立 CPU 参考，不是官方 INRIA 渲染器。

实测公开资产为 [DX.GL Apple](https://huggingface.co/datasets/dxgl/multiview-datasets)，36,313 个 SH3 高斯，数据集标注 CC0。第三方资产和详细验证证据仅本地保留。
