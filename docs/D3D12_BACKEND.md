# D3D12 后端

Windows 下将根目录 `MoerEngine.toml` 的 `[engine.rhi] type` 设为 `"D3D12"`。着色器使用 DXIL / Shader Model 6.8，设备还需支持 `ExtendedCommandInfoSupported`；适配器按高性能优先选择。DXR 1.1 和 Mesh Shader 支持单独查询。

## 功能边界

| 能力 | 当前实现 |
|---|---|
| Buffer、Texture | D3D12MA 分配；2D、数组、Cube、3D、mip、深度及 BC 格式 |
| 资源传输 | 上传、GPU 复制、传统回读、拥有式回读；紧密行布局与 D3D12 footprint 转换 |
| 管线与参数 | DXIL 反射；多个常量参数、UBO、raw/structured/typed buffer、纹理、采样器和资源数组 |
| Bindless | 保留现有 shader handle；按有序命令更新成员，每次提交保存使用的索引表及资源 |
| 光栅与计算 | 直接、索引、实例、间接及 count buffer 绘制，间接 dispatch；HS/DS、GS、AS/MS 管线 |
| Multiview | 设备支持 view instancing 时最多四视图，支持稀疏 mask 和附件相对层号；六面点光阴影保持串行回退 |
| 光追 | 三角形/AABB BLAS、TLAS、实例变换、历史 TLAS、inline RayQuery；BLAS 可更新 |
| 同步与结果 | GPU Fence 接受/拒绝状态，完成 Future、时间戳、精确遮挡查询和 PIX scope |
| Present | HWND swapchain、尺寸重建、呈现回执；源尺寸和原生格式必须与交换链匹配 |

Graphics、Compute 和 Copy 三个逻辑队列共享一个原生 Direct Queue，并向 RDG 报告相同的 native queue ID。当前支持有序执行语义，尚无跨原生队列的并行收益。

未实现的能力包括独立 shader-table 光追管线、cooperative matrix/vector、VRS/fragment-density、NRD/CUDA 的 D3D12 互操作，以及旧 `GetProfilerEntry()` 统计。现代 GPU scope 使用查询结果。无对应 DXGI 表示的像素格式会明确失败；cubic sampler 当前使用 linear filter。

## 提交如何保活资源

D3D12 要求命令 allocator、资源和描述符存储一直有效到 GPU 使用结束；提交成功本身不允许回收。后端用一份 `Packet` 持有不可变 `CmdSubmit`、allocator/list、暂存缓冲、描述符范围及查询存储。Completion 等待原生 fence 后，先物化回读、发布所有结果，再释放用户回调和资源；`Sync()` 还等待 CPU 回收结束。

`Timeline::MarkSubmitted` 只表示信号已交给原生队列。拒绝的信号独立记录，不用 host signal 伪装成功。录制失败时，尚未执行的资源状态和 bindless 更新会回滚，普通清理回调仍会执行，success callback 不会执行。

所有原生录制和提交共用 submission mutex，资源状态由这一有序边界维护；纹理状态按 mip、array layer 和 plane 区分。RDG 的显式屏障会转换为 D3D12 状态，逻辑队列之间不需要原生 ownership transfer。这个选择省去了另一套并行状态合并协议，代价是原生录制串行化。

Bindless 成员表保存的是 CPU 侧资源引用，GPU 索引表与描述符属于使用它们的提交。后续的 unbind 或槽位复用不会改写已录制工作的表；被丢弃的更新可以在后续命令流中重新发布。

## Present 为什么也属于完成边界

`Present` 可能在复制命令之后继续排入 GPU 工作，所以完成 fence 必须在原生 `Present` 之后 signal。交换链重建同时阻止新的提交，并确认此前的呈现包已完成 CPU 回收，才释放 backbuffer 引用并调用 `ResizeBuffers`。仅等待复制命令完成不能保证 backbuffer 已允许重用或销毁。

## 着色器移植约定

- 多个逻辑常量共用 cbuffer，递归反射结构、矩阵及数组的成员偏移，并从 C++ 的 4 字节组件布局重排。外部 UBO 只有在布局有差异时，才通过提交内的 GPU 复制生成原生布局。typed buffer 未指定 view format 时，从 DXIL 返回类型和分量数推导。
- StructuredBuffer 的元素步长来自 DXIL 反射，不能使用底层 buffer 的分配步长：场景缓冲经常按 byte 分配，同时供 raw bindless 和 structured 入口使用。无界资源数组的根签名范围保持无界，描述符存储按该次命令的实参数量分配。
- RHI 的 GPU driven shader 使用包含绘制起点的顶点/实例索引。DXIL 的普通 `SV_VertexID` / `SV_InstanceID` 不包含这些偏移，因此几何及 gizmo VS 使用 SM 6.8 的 `SV_StartVertexLocation` / `SV_StartInstanceLocation` 重建绝对索引。[HLSL Extended Command Information](https://microsoft.github.io/hlsl-specs/proposals/0015-extended-command-info/)
- 全屏 VS 的 UV 输出放在 Position 之前，全屏 PS 也使用相同的参数顺序，以保持 DXIL 寄存器匹配。Vulkan 继续使用显式 location。
- SMAA 通过统一 heap accessor 获取资源。Probe GI 的 DXIL 搜索保留循环，避免把整套自适应搜索复制到每个 volume/level；Vulkan 的展开策略保留。
- 纹理复制不能把 RHI 的紧密行数据直接当作 D3D12 footprint。上传和回读转换行距；GPU buffer/texture 复制通过暂存缓冲重排行，完整 BC 子资源复制不使用未按 block 对齐的 box。
- 纹理到纹理复制按选定 mip 计算尺寸，并保留源、目标数组层；数组范围拆成单层复制命令。首次构建之前不返回空存储的历史 TLAS，让渲染器使用当前 TLAS 作为首帧回退。

## 验证入口

```powershell
cmake -S . -B build -DMOER_D3D12_GPU_TESTS=ON
cmake --build build --config Release --target MoerEditor TestD3D12Backend -j 30
ctest --test-dir build -C Release -R '^D3D12BackendContract$' --output-on-failure
```

硬件测试默认不注册到 CTest，避免让仅运行 CPU 合约测试的环境依赖图形设备；Windows 下仍可直接运行 `TestD3D12Backend.exe`。

测试在实际硬件上执行，验证数据而非仅检查入口存在：上传/回读、奇数尺寸、BC、mip/array/3D、float16/uint UAV clear、多个常量、structured/typed buffer、无界资源数组、bindless、间接 dispatch、光栅像素、GS、AS/MS、时间戳、遮挡、multiview 分层像素、三角形/AABB 命中、BLAS 更新、TLAS 历史、失败回滚，以及连续 Present/resize。测试同时检查 D3D12 验证层错误计数。

2026-11-04 在 Windows / RTX 5080 上使用现有 VS 2022 Release 构建验证。11 项相关 CTest 通过。Sponza 的光栅、细分曲面开启/关闭、视野调整，以及 ReSTIR DI 光追均已观察到场景内容；光追切回光栅并正常退出。自动 `--threading-renderer-switch-validation` 完成 Raster 重载、Raster→Raytracing→Raster，各阶段稳定 12 个 ready GT frame 后退出。该构建关闭 NRD，光追画面保留采样噪声。D3D12 验证层未报告错误；光追输出的 UI 清屏仍可能产生 optimized clear value 不匹配的性能警告。

尚未做性能基准。逐提交创建 allocator/list、描述符快照和暂存缓冲的开销仍可通过完成后的复用降低；这类优化应继续复用现有 Packet 完成边界。
