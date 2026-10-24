# Vulkan 描述符后端与绑定选择

## 从约束划分责任

Pass 只声明 shader 参数并传入资源。set、binding、数组下标仍然是 shader 接口的一部分，选择原生存储方式属于 Vulkan RHI。设备支持某个扩展，并不意味着每个 pipeline 都必须使用它。

GPU 执行晚于 CPU 录制。每次 draw/dispatch 使用的描述符、索引表和资源都必须活到使用结束；分配方式改变不能取消这条约束。传统 sets 把描述符存储交给驱动，buffer/heap 把字节存储交给引擎，完成条件仍由已有提交系统提供。

## 设备能力与 pipeline 策略

设备默认依次选择 `VK_EXT_descriptor_heap`、`VK_EXT_descriptor_buffer`、传统 descriptor sets。Heap 的 binding-mapping 路径还要求启用 maintenance5；本实现不要求 shader 改成 untyped pointers。Push descriptors 是独立能力，不再是 descriptor buffer 可用的前提。

| Pipeline 条件 | 原生实现 |
|---|---|
| 普通参数，无 bindless 表 | 传统 set layout；满足条件的一个小 set 使用 push descriptors，其余从 descriptor pool 分配 |
| 使用 bindless 表，设备选择 heap | Native resource/sampler heap，shader set/binding 映射到 heap 偏移，以 push data 传参数和表基址 |
| 使用 bindless 表，设备选择 buffer | Descriptor buffers + set offsets；支持 bufferless push descriptors 时，可以把一个普通 set 改为 push descriptors |
| 设备只有 sets、pipeline 使用 input attachment，或 heap 的完整 push data 超限 | 传统 sets，包括 bindless 的持久化数组 sets |

这是创建 pipeline 时根据反射和设备限制确定的规则，不在每次 draw 时猜测访问模式或切换存储。普通 set 中优先选择描述符数最多且不超过 `maxPushDescriptors` 的一个作为 push set；数量相同时选择较小的 set 编号。Vulkan 限制一个 pipeline layout 至多有一个 push descriptor set。

Heap 路径通过 `VkShaderDescriptorSetAndBindingMappingInfoEXT` 保留现有 SPIR-V 的 set/binding 接口，原生 pipeline 不创建 `VkPipelineLayout`。Push data 包含用户常量、bindless 索引表地址和资源表字节偏移。启用 robust buffer access 时仍编码 buffer descriptor，保留范围语义；只有未启用该保证时，单个普通 buffer 才可以使用 push address 映射。

Push descriptors 直接更新一个传统 set 的绑定状态；push data 提供 heap shader 的立即数据。两者不是同一操作。Heap 与非 heap pipeline 可以在同一命令录制中使用，每次绑定重新建立需要的原生状态，不依赖另一种路径留下的状态。

## 存储与生命周期

| 对象 | 状态含义与回收条件 |
|---|---|
| `VulkanPipelineState` | 不可变的后端选择、原生布局或 heap mappings、参数位置；随 pipeline 释放 |
| `VulkanDescriptorHeap` | 设备级描述符编码缓存及所选后端的 GPU 存储；sets 模式不创建描述符字节存储 |
| 提交的 descriptor lease | 普通参数的不可变字节区间；录制合流后 flush，GPU 完成或全部录制被丢弃后回收 |
| `VulkanCommandList` | 传统临时 descriptor pools 和 texel buffer views；已有 command context 完成并允许重新 Begin 时释放 |
| `VulkanBindlessArray` | 索引表、持久化描述符及资源成员；沿用原有更新提交与 slot retirement 规则 |

Buffer 模式保留原有瞬态存储和 bindless 描述符 buffers；绑定偏移、sampler set 大小均查询原生 layout，不能用描述符数量推断驱动是否添加 padding。

Heap 模式使用一块 resource heap 和一块 sampler heap，预留驱动要求的 reserved range。Resource heap 按设备上限缩减，最多使用 64 MiB 的应用空间。Bindless 持久表和提交临时区间使用同一个已有分配器，但 lease 的结束条件不同：前者随 bindless array 生命周期结束，后者随提交完成。驱动 reserved range 不交给应用分配器。

普通参数写入独占的新范围，不覆盖仍被 GPU 使用的数据。CPU 编码缓存仍由资源持有，slot 编号不是 shader 下标，也不是 GPU 地址。原生编码成功、缓存发布成功之后才把 slot 视为有效；失败时回收未发布的 slot。

传统 bindless sets 使用 update-after-bind。CPU 只写新分配且尚未由索引表发布的物理 slot；旧 slot 继续受原有 retirement 约束。GPU 索引表更新独立于描述符字节上传：sets 模式没有字节上传，但仍必须提交索引更新。

## 当前选择的代价

为了让同一 bindless array 同时服务现代路径和传统回退，每个 array 都维护一套持久化传统 sets。现代路径因此额外承担一份驱动描述符存储和相应更新。这里明确选择了直接可用的 fallback；尚未引入按首次使用延迟构建副本的额外状态和同步。

CPU 描述符字节缓存、固定资源表容量和原来的提交分配器继续保留。此次改造不是“heap 消除所有 CPU 副本”，也不意味着无需容量规划。普通 sets 的临时 pools 按 command context 回收，没有再加入全局 set 缓存或独立完成队列。每个 pool 记录剩余 set 数和各类型描述符数量，只有完整 layout 能容纳时才从中分配，不能根据本次活跃写入数量推断 pool 消耗。

默认选择 heap 是能力策略，不是跨硬件的性能结论。实际成本取决于 descriptor 编码、拷贝、pool 分配、pipeline 切换以及 shader 访问方式；须在相同场景和构建配置下测量。

## 验证和对比入口

开发环境可以在启动进程前设置：

```powershell
$env:MOER_VULKAN_DESCRIPTORS = 'auto' # auto / heap / buffer / sets
$env:MOER_VULKAN_PUSH_DESCRIPTORS = '0' # 关闭 push descriptors，用于对照；不关闭 heap push data
```

显式请求不支持的后端会报错，`auto` 自动降级。Pass API 与 shader bindings 不需要改动。

```powershell
cmake --build build --config Debug --target MoerEditor TestVulkanDescriptorBackend TestVulkanDescriptorPolicy --parallel 16
ctest --test-dir build -C Debug -R '^(VulkanDescriptorPolicyContract|VulkanDescriptorBufferAllocatorContract|ShaderConstantsContract)$' --output-on-failure
python tools/descriptor_backends/run_matrix.py --validation-layer-dir build/descriptor-validation/layer
```

矩阵需要本机安装支持对应扩展的 Vulkan 驱动，以及能理解 heap 扩展的验证层。`--validation-layer-dir` 可省略以使用系统配置；它不下载或安装验证层。不要在 editor 或测试进程仍运行时构建：项目构建步骤会覆盖进程正在加载的 DXC DLL。

GPU 测试对同一工作负载切换 auto/heap/buffer/sets、开启/关闭 push descriptors、串行/并行录制。每项包含 1,536 次 dispatch、8 批提交、每 4 批一次 GPU 完成等待，检查每个输出值；覆盖 UBO、普通 storage/texel buffer、bindless buffer/image/sampler、真实深度纹理、40 元素数组、binding/set 空洞、模式切换及分配器复用。设备允许 256 B push constants 时还覆盖大常量 pipeline；本机 256 B heap push data 限制使它回退到传统 sets，并验证同一 bindless array 可继续访问。

CPU 合约检查能力缺失、push 独立性、push data 边界回退，以及原有分配器的并发、回滚和回收。Push constant 反射保留完整声明范围，同时只上传活跃参数；shader cache 格式版本已升级，旧反射缓存会重建。Shader constants 合约也编译实际的 BilateralFilterDenoiser，避免它遗留的常量声明漏掉 bindless 索引表地址；该遗漏曾使本机 DXC 在冷编译时崩溃。

集成验证同时统一了采样深度图的描述符 layout 与资源屏障规则，并显式声明 Hi-Z、光追后处理和环境图拷贝 storage images 的实际格式。这些约束在三种后端上都成立，不能依赖某一路径恰好容忍不一致。

2026-10-24 本机验证使用 RTX 4080 SUPER、Vulkan 1.4.357 和 Khronos validation layer 1.4.365：16 组 GPU 回读全部通过，3 项 CPU/shader 合约全部通过。默认 auto 选择 heap；光栅和光追均在首次呈现后继续运行超过一分钟并正常关闭，检查包括退出时刷出的完整验证日志，未发现验证错误或 storage image 格式警告。完整场景日志分别为 `build/descriptor-editor-raster-final.log`、`build/descriptor-editor-raytracing-final.log`。物理设备测试目前仅覆盖这一款 NVIDIA GPU；缺失扩展的选择规则由 CPU 合约覆盖。

矩阵日志和 `results.json` 写入 `build/descriptor-backends`。计时包含 Debug、验证层、录制和完成等待，只用于发现异常和形成后续对照入口，不作为性能优化结论。性能比较应使用 Release、相同场景与分辨率，分开比较 CPU 录制、GPU 时间、峰值描述符占用和分配失败；先通过回读正确性再测量。
