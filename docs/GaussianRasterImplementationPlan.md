# MoerEngine 光栅化高斯实施方案

> 实施状态：P1/P2/P3 已完成，包括标准 PLY、独立高斯光栅、编辑器属性、场景文档和 PNG 闭环；详见 `GaussianRasterProgress.md` 与 `GaussianEditorGuide.md`。P0 Sponza 波动按用户决定暂搁置；P4/P5 论文转换与混合渲染尚未实现。

日期：2026-10-20。依据本次需求收敛：只开发光栅化，纯高斯优先。本文替代早先方案中的双 Renderer 首期范围。本文件是实施设计，不代表功能已完成。

开发分支：`feature/gaussian-raster`。基线：本次从 `https://github.com/NJUCG/MoerEngine.git` 获取并核对默认分支 `main`，提交 `018aca93e70f92992fa6306b2fc62382e03f963e`（`refactor(vulkan): simplify pipeline binding accumulation`）。此前调研基于 `34b56c5dda8ac5c4e59b3e4475d400d2eea7ae31`；新基线涉及 RenderGraph、RHI 和 shader 等更新，本文的模块划分仍作为目标，具体 API 和插入位置须在 P0 按新基线复核。

## 1. 三种状态与范围

将用户要求的三种状态解释为纯 Mesh、纯高斯和 Mesh＋高斯。依据此前统一转换讨论，混合模式读取原 Mesh，生成独立的材质高斯派生资源，不改写原 Mesh。此方案不是最后叠加 Mesh HDR 的轻量合成路线。

| 状态 | 执行路径 | 保证 |
|---|---|---|
| MeshOnly | 原 RasterRenderer 原始路径 | 无高斯时原 Mesh 输出和 GPU-driven 数据契约不变 |
| GaussianOnly | 标准 3DGS GaussianSplatPipeline | 标准 PLY 保留 SH、opacity 与原始算法语义；与选定参考实现比较 |
| Mixed | 独立派生统一高斯场景＋材质高斯光栅化路径 | 不把近似光栅化宣称为论文完整传输复现 |

空场景输出既定背景。状态按已启用实例类型判定，不按视锥内数量判定，避免相机移动触发模式反复切换。相机和灯光不计入 Mesh/高斯数量。实例隐藏时是否参与阴影以后由显式属性决定。

首期交付 P0–P3：纯 Mesh 回归、标准 PLY 纯高斯渲染、编辑器编辑、保存重载与图像导出。混合识别后，在混合后端未实现前明确显示“暂未支持”，不得静默遗漏 Mesh 或高斯，也不得把不完整图像标为成功导出。

## 2. 论文复现的准确边界

参考：https://mangosister.github.io/gsr_site/ 和 https://arxiv.org/html/2406.09733v3 。

论文提供统一高斯表示、源数据转换、法线分布与基础 BSDF 外观、线性透射及路径追踪操作。它没有提供可直接照搬的完整实时光栅化后端。

本项目拆成三层验收：

1. 表示与转换：实现 Mesh → 几何/传输/材质高斯，核验参数、轮廓与透射；可以对照论文方法。
2. 运算模块：用离线数值测试核验高斯积分、透射、SGGX 与 BSDF 的局部运算，不要求在本期增加顶层光追 Renderer。
3. 光栅化应用：投影、排序、近似着色、阴影和颜色累积是本项目另行设计，称为“论文表示的光栅化近似”。标准 alpha 累积与论文线性透射不可互相冒充。

若后续把目标升级为完整论文渲染复现，需单独安排论文的光线传输参考实现。这个目标与“只做光栅化”不能同时默认满足。

标准 PLY 不能只靠格式转换恢复真实材质。外部 PLY 默认保留 SH 外观；Mesh 派生高斯具有材质外观。共存时的累积规则是扩展设计：第一版材质预览采用显式的 StandardAlpha 合成约定和转换参数映射，保留论文传输参数作为独立字段，不把 magnitude 当 alpha。映射误差需要独立验收，不承诺与论文最终图像一致。不得因导入一个 Mesh 而暗中改变原 PLY 的 opacity/SH。

## 3. 不改动的 Mesh 数据契约

保留原 CMesh/CPrimitive/CRenderable、顶点/索引布局、Meshlet、culling、vertex pulling、bindless Mesh 索引及默认 shader。转换器只读 CPU 资产的几何、材质和纹理，生成新资产。

纯 Mesh 时不调用转换器，不分配高斯工作区，不增加绘制、复制或合成 Pass，不改变 AA、曝光及历史推进。

混合模式内 Mesh 已转换为高斯，所以虽然旧代码不受影响，混合性能仍可能下降。必须分别统计转换成本、派生高斯数量、tile 配对数量、显存和 GPU 时间；不承诺等于原 Mesh 帧率。

## 4. 建议新增模块

以下类型和目录均为拟新增，不代表当前已有实现。

```text
scene/gaussian/
  GaussianAsset             标准几何与可选外观数据
  GaussianInstance          资产引用、变换、启用与实例覆盖参数
  GaussianSceneUpdate       增量更新、revision 和删除记录
  GaussianGpuScene          上传、资源池、fence 生命周期
  DerivedGaussianScene      混合模式的派生场景，后续阶段
  MeshToGaussianConverter   只读 Mesh 的转换器，后续阶段

scene/loader/
  GaussianPlyLoader         标准 PLY 解析和数据合法性校验

renderer/gaussian/
  GaussianSplatPipeline     纯高斯和后续混合共用的投影/排序基础
  GaussianRasterResources   每视口工作区、颜色、透射率
  GaussianCompositePass     背景与颜色合成
  GaussianRenderSettings    渲染配置和显示策略
  GaussianMaterialPass      后续材质着色

shaderheaders/shared/gaussian/
  GaussianParameters        C++/HLSL 共享布局
```

引擎仍选择 Raster。RasterRenderer 的外围状态分发调用旧 Mesh 路径或独立高斯 feature，不为三种状态各复制一套窗口、UI、提交与帧管理。新路径图像输出可独立适配公共输出阶段，避免先重构旧路径。

### 4.1 资产定义

- Geometry：中心、尺度/旋转或协方差、稳定 Gaussian ID、bounds。
- SHRadiance：SH 阶数与系数、标准 opacity。
- MaterialScattering：材质引用、SGGX 参数、论文 magnitude，后续实现。
- 元数据：格式版本、坐标/单位、来源、内容 hash、外观与传输语义。

每个资产共享几何数据，多实例只保存变换与覆盖参数。非均匀缩放通过 Σworld=AΣlocalAᵀ 处理；SH 观察方向按约定映射到资产局部坐标，专门验证旋转后的外观。

不要默认 PLY 内 nx/ny/nz 字段是有效法线。PLY 中存在这些字段也不代表存在可用材质。

## 5. 现有代码改造位置

| 位置 | 改造 |
|---|---|
| scene/loader/LoaderInterface.* | 注册专用标准 3DGS PLY 入口；不替换 Assimp |
| scene/LogicalComponents.h | 增加 Gaussian 资产/实例组件，复用 CNode 层级与变换 |
| Scene 公共接口、SceneLifecycle.cpp | 导入、实例创建、查询/编辑、删除与资产引用；原子失败，不发布半成品 |
| SceneTickSync.cpp、GpuSceneUpdate.h | 独立高斯 dirty 标记和更新包，HasWork 覆盖高斯；避免触发 Mesh full rebuild |
| RenderScene / GpuScene 周边 | 可选 GaussianGpuScene，保持 Mesh 资源编号和上传顺序 |
| RasterFramePacket.h | 加入不可变高斯配置、状态、资源版本；RT 不读编辑器 registry |
| RasterRenderer.cpp | 状态分发、graph/legacy 两条调度、新资源按需初始化 |
| RasterConfig.h | 新增高斯设置，旧默认项不变 |
| editor/scene_editing_ui/SceneFileDialog.h | .ply 文件过滤与导入入口 |
| editor/scene_editing_ui/SceneEditingUI.* | 导入进度、错误与资产操作 |
| editor/inspector_ui/InspectorUI.* | 实例与资产面板，复用现有 Scene 变换/可见性 API |
| editor/raster_ui/ | 高斯调试视图、颜色处理、性能统计 |
| 场景序列化 | 新组件及引用 remap，独立派生缓存；兼容旧 Mesh 场景 |

资源更新由主线程生成快照，渲染线程消费；GPU 使用中的资源按 fence 延迟回收。连续拖动参数合并更新，避免每次 UI 事件重建大 buffer。

## 6. 纯高斯：第一条必须打通的路径

### 输入

首期支持常见 3DGS ASCII 与 binary little-endian PLY、SH 0–3 阶，按属性名解析而非假定字段固定顺序。明确识别 log-scale、opacity logit 与 quaternion 分量约定，校验数量、有限数值、长度与 SH 系数数目。普通点云 PLY、缺字段或不支持编码清晰报错。

### 每帧 GPU 流程

```text
实例/资产更新
  → Prepare：变换、视锥剔除、协方差投影、SH 求值
  → Count / Scan：计算 tile 配对偏移
  → EmitPairs：生成 tile、深度、Gaussian ID
  → RadixSort：所有可见实例统一排序
  → TileRanges：构造每个 tile 的区间
  → Splat：逐像素前向 alpha 累积
  → 背景合成
  → 明确的显示变换 / UI / 导出
```

输出保存预乘颜色和剩余透射率，不强制将 alpha 写成 1。纯高斯不运行 Mesh Geometry、Lighting、AO、SSR，不需要伪造 Mesh GBuffer。针对空 Mesh 检查 Scene bounds、相机适配、空资源及初始化路径。

多个高斯资产统一排序，不把每个资产先渲染成一张图后依次覆盖。区分有效 count 与容量；溢出必须报告并重试/扩容，不静默截断。首期 Vulkan；D3D12 间接计算路径单独验证，不默认承诺。

废弃的旧版高斯 Shader 已按用户要求删除。P2 基于当前 RHI 和 P1 共享数据结构重新实现；验证排序边界与 subgroup 能力、相机空间、SH clamp、tile 清理、count/capacity 和透明度输出。

### 图像一致性

提供“参考输出”和“引擎显示”两种明确策略。参考输出固定背景、投影、过滤、阈值、SH clamp、颜色编码和输出变换；默认不叠加会改变参考图的曝光、Bloom、TAA。参考算法若无可用本地运行环境，则使用记录完整设置的可信参考图，并报告证据限制。

引擎显示允许用户调整曝光和色调映射，但不能再宣称与未做该处理的参考图逐像素一致。纯 PLY 不调用论文材质转换。

## 7. 编辑器：首期真正可操作的内容

| 面板 | 首期能力 | 参数语义 |
|---|---|---|
| 导入 | 选择 PLY、显示进度/错误、导入到当前 Scene | 成功后原子发布资产与实例 |
| 场景树 | 高斯类型标识、选择、命名、隐藏、删除、复制实例 | 首期可通过树选取；精确逐高斯拾取后续实现 |
| Inspector 基本信息 | 源文件、Gaussian 数量、SH 阶数、bounds、显存统计 | 原始数据只读 |
| Inspector 实例 | 平移、旋转、缩放、启用、opacity multiplier、颜色乘数 | 实例覆盖；默认 1，不覆写源 PLY；颜色乘数不是重光照 |
| Raster 设置 | 背景、参考/引擎显示模式、曝光、调试输出 | 参考配置可一键复位 |
| 调试 | 颜色、T、tile 覆盖、有效高斯数、配对数、溢出及 GPU 时间 | 避免每帧阻塞回读 |
| 保存/导出 | 保存源引用与实例参数，重载恢复；导出最终 PNG | 不含 UI；原始 PLY 不被隐式修改 |

所有写入走 Scene API，不让 ImGui 直接修改 GPU buffer。首期编辑范围为资产/实例；数百万高斯逐点移动、逐点 SH 编辑、训练不在首期。

后续混合阶段增加：转换设置、缓存状态、重建/取消、派生数量、材质覆盖、透射参数、SGGX 预设、灯光编辑与“不响应灯光”的 SH 标识。材质 UI 只作用于支持材质的资产，不让 SH 资产出现无效滑块。

## 8. 后续混合实现

1. Mesh 转换读取原几何、纹理、法线、材质；转换结果按资产共享，避免每个实例重复生成。
2. 缓存 key 包含源几何、材质/纹理 hash、转换版本与设置；实例移动不重新转换，材质编辑只刷新相应数据。
3. 初期限定静态不透明 Mesh；蒙皮、变形、透明/透射材质若未支持必须提示，不静默近似。
4. 以平面、球、已知材质模型验证转换，再接入复杂场景；阶段内不保证转换后的 Mesh 与原管线像素一致。
5. 材质高斯先实现显式预览光照，再增加灯光视角透射/阴影；法线分布不能无说明地替换成单个平均法线。简化采用何种近似必须标注。
6. 标准 SH 高斯继续计算 SH 颜色。是否让其向材质高斯投影需明确几何透射近似；不默认给 SH 再乘一遍光照。
7. 不向旧 Mesh GBuffer 填入高斯属性。统一场景用自己的颜色/T 和必要辅助输出；单层深度、法线不能视作多层散射的完整描述。

混合模式进入时先完成派生数据，再在帧边界切换；转换失败保留源场景和可回退状态，并显示错误。退出时回到原 Mesh 路径，处理历史重置。不得因查看或切换模式改写源资产。

## 9. 分阶段交付与验收

| 阶段 | 交付 | 验收门槛 |
|---|---|---|
| P0 | Mesh 基线、状态路由设计、纯高斯测试资产 | 固定相机/时间/配置，记录 Mesh HDR/LDR；确认可重复性 |
| P1 | PLY loader、资产/实例、Scene API、GPU 更新 | 合法/非法文件、实例生命周期、重载与空 Mesh 场景正确 |
| P2 | 标准 Splat、背景和输出 | 单高斯、重叠排序、多实例、相机/变换、真实 PLY 对照通过 |
| P3 | Inspector、导入、调参、保存、PNG、性能统计 | 编辑立即生效，保存重载一致，首期用户操作闭环完成 |
| P4 | Mesh 到材质高斯转换及缓存 | 源 Mesh 不变；基础形状/材质、参数运算与转换误差可测 |
| P5 | 混合光栅化近似和编辑器扩展 | 三状态切换正确；说明与论文完整传输的差异，测画质与开销 |

纯 Mesh 回归贯穿每阶段：没有高斯、全部禁用、导入失败、添加后删除。旧路径确定性条件下要求 HDR/LDR 位一致；若基线自身不确定，先诊断，不任意放宽阈值。

纯高斯参考比较使用预先记录的容差和差异图，覆盖排序、投影、SH、背景与过滤语义；不要把跨实现的浮点误差误称为位一致。单一实现固定配置可测试自身确定性。

运行时改动遵循仓库构建约定：检查 MoerEngine.toml，光栅开发用 Raster；构建通过后实际启动并检查日志与验证层。首期不修改 RaytracingRenderer，不做训练、不做反向传播。

## 10. 实施起点

光栅高斯开发分支已从上述最新上游基线创建，接下来建立 P0 基线并复核接口；随后实现 P1–P3。优先完成“空场景导入一个标准 PLY → 正确显示 → 编辑实例 → 保存重载 → 导出图像”。混合数据接口预留明确类型，但不把未验证的混合光照算法放入纯高斯首期依赖链。
