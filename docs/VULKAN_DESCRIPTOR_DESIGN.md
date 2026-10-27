# Vulkan 描述符的绑定方式与分配管理

描述符系统要解决两个问题：shader 如何找到资源，以及这份绑定信息在 GPU 使用期间由谁保存。Vulkan 的 descriptor set、descriptor buffer、descriptor heap 和 push descriptors，主要改变的是表示方式与责任边界；引擎仍需根据资源的更新频率、并发录制方式和执行生命周期，决定如何分配与回收空间。

**应用不再管理临时描述符，不等于底层不再分配、复制或绑定描述符。** 特别是 push descriptors 与普通 set 绑定，可以采用相同的 GPU 绑定机制；减少引擎中的独立状态，与减少实际执行开销，是需要分别判断的收益。

理解这些设计，可以先把 API 路径与分配策略分开，再看它们如何组合。

## 资源绑定的基本模型

讨论描述符时，需要区分四种内容：

| 内容 | 例子 | 作用 |
| --- | --- | --- |
| 资源数据 | 纹理像素、顶点、Uniform Buffer 中的常量 | shader 实际读取或写入的内容 |
| CPU 绑定信息 | buffer、offset、range，image view、sampler | 引擎用于描述一次绑定的输入 |
| 编码后的描述符 | 驱动根据绑定信息生成的描述符内容 | 供 GPU 定位、解释资源 |
| 命令中的绑定状态 | set、表偏移、heap 索引、push 状态 | 指定后续 draw 或 dispatch 使用哪份绑定 |

修改 Uniform Buffer 中的矩阵，不一定需要修改描述符；只要 buffer、偏移和范围不变，原描述符仍然指向同一位置。反过来，切换到另一段 buffer，即使常量内容相同，也可能需要修改绑定。

这里有三条独立约束。首先，并发 CPU 录制不能分配到相互冲突的空间。其次，描述符写入必须在 GPU 读取时可见。最后，仍被 GPU 使用的描述符及其引用的资源不能被提前回收。互斥锁解决第一条，内存可见性规则解决第二条，fence 或 timeline semaphore 等完成信号解决第三条。

对于需要保留逐次绑定版本的系统，最简单的办法是：为新版本分配另一段空间，完成录制后交给提交记录持有，等相关 GPU 使用结束再归还。尚未提交但仍可能执行的命令，同样必须保留它引用的空间。具体 API 对更新时机还有约束；例如传统 set 的 update-after-bind 能力，改变的是允许更新的窗口，不会自动消除读写冲突。

**CPU 可见与 GPU 可见可以是同一块内存的属性。** 应用可以直接把描述符写入映射后的 GPU 可访问内存，也可以先在 CPU 缓存编码结果，再复制到目标区域。是否保留副本，取决于重用需求和内存访问成本。[Vulkan descriptor buffer 设计说明](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_buffer.html)

## Vulkan 的四种使用方式

### Descriptor set 和 pool

传统路径以对象组织描述符：

1. 用 `VkDescriptorSetLayout` 定义 binding、类型和数量。
2. 从 `VkDescriptorPool` 分配 `VkDescriptorSet`。
3. 通过 `vkUpdateDescriptorSets` 写入绑定。
4. 通过 `vkCmdBindDescriptorSets` 选择后续命令使用的 set。

应用管理 set 和 pool 的生命周期，驱动管理其底层描述符存储。pool 的容量同时包含 set 数量和各描述符类型的配额；因此，剩余 sampler 配额不能直接补偿耗尽的 uniform buffer 配额。[vkAllocateDescriptorSets](https://docs.vulkan.org/refpages/latest/refpages/source/vkAllocateDescriptorSets.html)

短期 set 可以只分配、不逐个释放，等所有使用结束后整体 reset pool。Vulkan 明确保证：自创建或最近一次 reset 以来没有单独释放过 set 的 pool，不得因碎片导致分配失败。这不排除容量或内存耗尽。[VkDescriptorPoolCreateInfo](https://docs.vulkan.org/refpages/latest/refpages/source/VkDescriptorPoolCreateInfo.html)

长期对象可以持有可重用的 set；生命周期各不相同时，也可以使用支持单独释放的 pool。两者可以并存。整体 reset 减少逐项管理，其代价是一个尚未完成的使用者可能延长整个 pool 的占用。[vkResetDescriptorPool](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetDescriptorPool.html)

传统路径还有一个有用的选择：dynamic uniform/storage buffer descriptor。set 保持不变，在绑定时传入 dynamic offset，即可选择 buffer 中不同的数据段。这把一部分“更新描述符”的需求变成了“更新命令中的偏移”。[Descriptor Dynamic Offset](https://docs.vulkan.org/guide/latest/descriptor_dynamic_offset.html)

### Descriptor buffer

`VK_EXT_descriptor_buffer` 将描述符存储交给应用，但保留 set layout 和 pipeline layout：

1. 查询 layout 总大小、各 binding 的偏移，以及设备要求的对齐。
2. 在具有相应 usage 的 buffer 中分配区域。
3. 调用 `vkGetDescriptorEXT`，把驱动编码后的描述符写入目标内存。
4. 绑定 descriptor buffer，再为各 set 设置 buffer 索引和偏移。

应用知道描述符的大小和位置，内容仍由驱动编码。一个 buffer 可以容纳许多逻辑上的 set；选择某个 set 的新版本，通常只需改变绑定偏移。[vkGetDescriptorEXT](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetDescriptorEXT.html)、[vkCmdSetDescriptorBufferOffsetsEXT](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdSetDescriptorBufferOffsetsEXT.html)

这使普通的内存管理方法可以用于描述符：固定槽位、线性块、环形分配、空闲区间分配都能采用。与此同时，应用接手了容量、对齐、发布和回收责任。host 写入的描述符可通过提交的可见性规则提供给 GPU；非 coherent 映射仍需相应 flush，GPU 写入则需要建立到描述符读取的同步。

descriptor buffer 不支持传统 dynamic buffer descriptor。需要变化的地址或偏移，要通过新的描述符内容、shader 中的索引，或其他合适的常量传递方式表达。这说明显式内存管理与高效传递逐次变化的参数，是两个不同的问题。[descriptor buffer 设计中的动态缓冲区讨论](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_buffer.html#_should_we_support_dynamic_buffers)

### Descriptor heap

`VK_EXT_descriptor_heap` 进一步改变 shader 的寻址模型：使用一个资源 heap 和一个 sampler heap，通过偏移或索引访问其中的描述符。它不再要求传统 descriptor set 和 pipeline layout，也提供 set/binding 到 heap 的映射，使既有 shader 接口可以继续使用。常量可以通过新的 push data 接口直接传递，也可以通过其中的设备地址间接访问。[VK_EXT_descriptor_heap](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html)

这里的 heap 是一段按规范组织、可供 GPU 访问的描述符内存区域。驱动通过 `vkWriteResourceDescriptorsEXT`、`vkWriteSamplerDescriptorsEXT` 等接口生成描述符；应用负责内存及其分配。heap 还包含实现要求的保留区域，不能把全部容量都当作自由槽位。[描述符写入](https://docs.vulkan.org/refpages/latest/refpages/source/vkWriteResourceDescriptorsEXT.html)、[heap 地址范围](https://docs.vulkan.org/refpages/latest/refpages/source/VkBindHeapInfoEXT.html)

与 descriptor buffer 相比，heap 的主要变化是绑定和寻址约定，而不是获得了新的碎片整理算法。二者底层都可以使用 buffer 内存。heap 明确限定资源与 sampler 的绑定模型，并为常量地址提供直接路径；descriptor buffer 则沿用 set layout 来解释内存。

“heap”这个名称需要结合 API 理解。D3D12 的 `ID3D12DescriptorHeap` 是一个存放描述符的不透明对象，应用用句柄和槽位偏移访问；Vulkan descriptor heap 显式描述内存地址范围。引擎中名叫 `DescriptorHeap` 的类，也可能只是 descriptor buffer 的封装。[Vulkan 与 D3D12 heap 的比较](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html#_is_this_the_same_as_directx_12_descriptor_heaps)

### Push descriptors

push descriptors 将少量绑定状态记录进 command buffer。应用提供 `VkWriteDescriptorSet` 一类的绑定信息，不必为这些绑定创建和回收普通 descriptor set；命令缓冲区内部负责保存 push 状态。

push 发生在 CPU 录制阶段，影响后续录制的 draw 或 dispatch。它不会即时修改正在运行的 GPU 工作，也不会上传被引用 buffer 的数据。`maxPushDescriptors` 限制的是整个 push set layout 中的描述符总数；绑定可以增量更新，因此是数量较少、频繁变化场景的一种候选方案，性能仍取决于更新量和驱动实现。它既能用于 UB，也能用于支持的纹理、sampler 和其他 buffer 描述符。[vkCmdPushDescriptorSet](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushDescriptorSet.html)

push descriptors 与 push constants 的区别是：前者传递资源绑定，后者传递常量字节。descriptor heap 的 push data 则可以承载常量、索引或地址，由 shader 接口决定解释方式。

push descriptors 早已通过 `VK_KHR_push_descriptor` 提供，后来进入 Vulkan 1.4；使用时仍需查询和启用相应能力。它与 descriptor buffer 组合时，需要 `descriptorBufferPushDescriptors`；`bufferlessPushDescriptors` 表示应用无需为这套组合额外提供 push 描述符的 backing buffer。[descriptor buffer features](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorBufferFeaturesEXT.html)、[descriptor buffer properties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDescriptorBufferPropertiesEXT.html)

#### 与普通 set 绑定的实际区别

真实约束是：GPU 执行 draw A 和 draw B 时，必须分别取得它们所需的资源绑定。普通 set 与 push 都要保存这些信息。对一个 UB 而言，两条路径最终都可以让同一个 shader binding 指向相同的 buffer、offset 和 range；push 将绑定内容录入命令后，无需再调用 `vkCmdBindDescriptorSets`。

**差别主要在状态的所有权、更新语义和生命周期；API 不保证它们生成不同或更少的 GPU 绑定操作。**

| 问题 | 普通 set 与 bind | Push descriptors |
| --- | --- | --- |
| 应用管理的绑定状态 | 独立的 set 对象，可供多个命令缓冲区引用 | 当前命令缓冲区中的 push 状态 |
| 谁安排临时存储的分配与回收 | 应用通过 pool/set 或自有分配策略安排 | 驱动结合命令缓冲区管理 |
| 如何保留不同 draw 的绑定版本 | 应用准备不同 set，或在适用时用 dynamic offset 等方式表达差异 | 驱动保存命令序列所需的版本；后一次 push 影响后续 draw |
| 相同绑定能否跨录制复用 | 可以直接复用已准备好的 set | 没有供应用跨命令缓冲区绑定的独立 push-set 对象；同一命令缓冲区内的状态可以连续使用 |
| 谁保留被引用的资源和数据 | 引擎保证资源及数据满足所有相关使用的生命周期和同步要求 | 仍由引擎保证，push 不复制资源数据或接管其销毁 |

普通 set 的 bind 不承诺为每次 draw 保存独立快照。不能一般地用“绑定同一个 set、录制 draw A、覆盖该 set、录制 draw B”代替两个版本；更新可能使已录制命令失效，或违反在途使用约束。update-after-bind 改变允许更新的窗口，也不会自动为每次 draw 保留旧内容。push 则提供沿命令序列更新绑定的语义。[普通 set 的内容消费与更新规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBindDescriptorSets.html)、[push 状态规则](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPushDescriptorSet.html)

Mesa RADV 的一份源码快照提供了具体例子：普通绑定与 push 都通过 `radv_set_descriptor_set` 登记内部 set，随后共用 `radv_emit_descriptors_per_stage` 输出绑定。在直接寻址分支，两者都把描述符存储的 GPU 地址交给 shader 使用的寄存器。push 路径还会更新内部描述符，从命令缓冲区的 upload 区域分配空间并复制字节。因此，这个实现仍有实际的描述符存储、分配和复制，只是由驱动管理；其他驱动可以采取不同实现。[RADV 源码快照 b0ea691](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/b0ea691e3fccc311be070b9ad52b14e772e5e9a4/src/amd/vulkan/radv_cmd_buffer.c)

如果应用原本已经按命令录制分配临时 set、保留各次绑定版本，并在 GPU 完成后整批回收，那么它与驱动实现 push 的组织方式可能很接近。迁移到 push 可以减少引擎独立维护的分配与回收状态，但不能据此断言底层内存占用、复制量或空间浪费下降。应用继续通过命令缓冲区的提交与完成机制保证安全复用，驱动承担内部绑定存储的保存责任。

性能比较还必须区分“绑定已有 set”和“为变化的绑定准备新版本”。前者可以复用已编码的内容，反复 push 相同内容反而可能增加工作；后者才涉及获取存储、更新、绑定和回收等成本。所观察的 RADV 普通路径会跳过已经绑定的同一个 set，而 push 路径仍会更新并复制内部描述符。减少 API 调用或应用代码，并不能单独证明总成本更低。

### 四条路径的责任边界

| 路径 | 应用直接管理的存储单位 | 命令选择绑定的方式 | 主要取舍 |
| --- | --- | --- | --- |
| Set 与 pool | set 和 pool 对象 | 绑定 set，可附加 dynamic offsets | 驱动隐藏存储细节，应用管理 pool 配额与生命周期 |
| Descriptor buffer | buffer 内的字节区间 | buffer 索引与 set 偏移 | 应用控制存储，保留 set layout 约束 |
| Descriptor heap | resource 与 sampler heap 内的区域 | heap 偏移、索引及映射 | 统一 heap 寻址，应用承担空间管理 |
| Push descriptors | 少量命令绑定状态 | 将描述符更新录入命令 | 减少应用的临时管理，仍有驱动内部成本，数量与支持范围受限 |

这些路径并非四个必须择一的完整架构。普通 set 或 descriptor buffer 都可以承载 bindless 数组，push 可以处理剩余的小量动态绑定。bindless 描述的是“shader 通过索引选择资源”的组织方式。[Vulkan descriptor indexing 示例](https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html)

混合还要区分层级：引擎可以提供不同后端路径，但不代表能在一次 draw 中任意叠加它们。descriptor heap 绑定与旧 set／descriptor buffer 状态会相互使对方失效；它与 descriptor buffer 加 push 的组合方式不同。[descriptor heap 绑定规则](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html#_descriptor_heaps)

## 分配器的组织与回收

### Context 和 allocator 的关系

context 通常是引擎的 CPU 命令录制环境，包含当前命令缓冲区、绑定缓存、临时分配和资源跟踪。它不天然等于一个线程、一帧或一个 GPU queue；这些绑定关系由引擎决定。描述符 allocator 也不同于 `VkCommandPool` 或 D3D12 command allocator，后两者管理的是命令存储。

讨论“一对多”时，需要分清三层：谁发起分配、谁维护分配状态、谁拥有底层存储。

| 组织方式 | 分配关系 | 收益与代价 |
| --- | --- | --- |
| Context 自有 allocator | 一个 context 的 allocator 管理多个 buffer 或 pool | 热路径局部化；空闲容量分散在各 context |
| Context 本地分配加共享后端 | 每个 context 一个游标，多个游标从共享池领取整块空间 | 小额分配无需争用共享游标；存在块尾剩余 |
| 多个 context 共用 allocator | 多个录制者同步推进同一分配状态 | 更容易共同利用当前块；需要锁或原子操作 |
| 按提交批次持有分配 | 录制者把所用区域交给同一提交记录 | 能复用统一完成信号；较早结束的区域可能较晚释放 |

最后一项是回收组织方式，可以与前三项组合。**统一回收，不要求共享一个分配游标。** 多个线程可以各自分配，提交时把各块登记到同一个完成记录。

同样，一个 context 可以同时拥有 sampler、资源描述符和常量数据的多个 allocator；一个物理 heap 也可以由多个 allocator 管理互不重叠的区域。类的数量不决定物理内存的数量。

### 让分配方式匹配生命周期

短期绑定若一起失效，可以使用线性分配：对齐游标、取得区间、推进游标，最后整块 reset。它省去逐项释放和空闲区间搜索，但牺牲块尾空间，以及块中较早结束部分的即时复用。

环形分配适合分配与完成大体有序的工作流。每段空间附带完成条件，写指针绕回前检查它是否已释放。若前面的长任务阻挡后面的已完成区域，环形结构仍会停顿；“环形”本身不保证容量足够。

生命周期独立的持久绑定，则适合固定槽位或可变大小空闲区间。固定槽位便于按索引重用；可变区间支持不同大小的表，并在释放时合并相邻空闲区间。这需要更多元数据和分配操作，也不能合并被存活分配隔开的空洞。

已有分配器可以处理“从某个地址范围取出一段空间”。例如 VMA virtual allocator 能只管理 offset 和 size，不创建 Vulkan 内存；它可以用于 descriptor buffer 的子分配。引擎仍需决定何时把区间交给它释放，因为通用分配器不知道哪些 GPU 命令还会使用它。[VMA virtual allocator](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/virtual_allocator.html)

### 容量问题的来源

假设两个 context 各领取一个 64 KiB 块，各使用 40 KiB。总空余还有 48 KiB，但任何一块都放不下一个连续的 32 KiB 请求。即使所有分配操作完全无锁，这个请求仍然无法满足。若这 128 KiB 原本来自同一连续区域，统一顺序放入两次 40 KiB 就能留下连续的 48 KiB；是否值得这么做，要比较共享分配状态的成本与块尾浪费。

需要进一步区分空余空间在哪里：

| 状态 | 无法复用的原因 | 对应的改进方向 |
| --- | --- | --- |
| 已释放但分散的空闲区间 | 请求需要更大的连续范围 | 合并相邻空闲区间，调整尺寸分类 |
| 仍归某 context 所有的块尾 | 父分配器只看见整个块被占用 | 改变块大小、领取粒度或共享方式 |
| CPU 已不用但 GPU 尚未完成 | 执行生命周期仍有效 | 等待、扩容或减少同时在途的工作 |
| 静态区空闲而动态区耗尽 | 容量被固定分区隔离 | 调整预算或允许重新划分 |

锁的数量、分配速度和容量利用率是三个指标。换用更快的 allocator，只有在改变了相关空间的可见性、粒度或回收方式时，才会改变对应的容量问题。

回收条件也应绑定真实使用者。单队列可以用有序完成值管理；多队列共享区域时，要覆盖所有相关队列的使用，或者通过依赖关系汇总到一个完成点。CPU 的帧结束、引用计数归零或录制结束，只有在其契约确实包含这些保证时，才能作为回收依据。

## 四个引擎中的组合方式

以下比较固定到 2026 年 10 月的源码快照：DXVK `02e2028`、MiniEngine `e5975f9`、DiligentCore `cd2329b`，以及 UE 5.7.1 和 5.8.4。MiniEngine 是 D3D12 实现，Diligent 同时观察 Vulkan 与 D3D12 后端；共同比较的是绑定、分配和回收的组织方式。

### DXVK

DXVK 要把 D3D 的资源绑定状态转译为 Vulkan，并保留足够广的设备兼容性。所观察的实现按设备能力选择 descriptor heap、descriptor buffer 或传统 descriptor pool。前两条路径共用相近的 range 分配模型，并由 worker 使用对应 API 生成描述符；这些是替代路径。[DxvkContext](https://github.com/doitsujin/dxvk/blob/02e2028c3fe954c8913b1f39bce2f581739f0c41/src/dxvk/dxvk_context.cpp#L24)、[descriptor worker](https://github.com/doitsujin/dxvk/blob/02e2028c3fe954c8913b1f39bce2f581739f0c41/src/dxvk/dxvk_descriptor_worker.cpp)

在 heap／buffer 路径中，context 持有自己的资源描述符 allocator。后端 buffer 被切成多个 range，range 内线性分配，空间不足就换 range；没有可复用 range 时创建新的存储。命令列表跟踪使用过的 range，只有相关 CPU 与 GPU 使用都结束，才允许整体重用。[range 分配](https://github.com/doitsujin/dxvk/blob/02e2028c3fe954c8913b1f39bce2f581739f0c41/src/dxvk/dxvk_descriptor_heap.cpp)、[range 状态](https://github.com/doitsujin/dxvk/blob/02e2028c3fe954c8913b1f39bce2f581739f0c41/src/dxvk/dxvk_descriptor_heap.h)、[命令列表跟踪](https://github.com/doitsujin/dxvk/blob/02e2028c3fe954c8913b1f39bce2f581739f0c41/src/dxvk/dxvk_cmdlist.cpp)

传统路径则从当前 pool 分配 set，耗尽后切换 pool，并在完成后 reset。两种实现的共同点是：把高频分配放在较大的生命周期单位内，避免为每次绑定建立独立的释放流程。[descriptor pool](https://github.com/doitsujin/dxvk/blob/02e2028c3fe954c8913b1f39bce2f581739f0c41/src/dxvk/dxvk_descriptor_pool.cpp)

其选择来自转译器的工作负载：绑定状态频繁变化，需要较低的录制成本。代价是 range 尾部损耗、在途 range 占用，以及切换底层 buffer 后可能发生的表重建。创建新 buffer 可以避免当前容量不足，但会增加内存占用；它不等价于提高既有空间的利用率。

### MiniEngine

MiniEngine 的 `CommandContext` 为资源和 sampler 分别持有 `DynamicDescriptorHeap`。资源描述符先保存在 CPU 可用的描述符存储中，context 缓存需要绑定的句柄；提交绑定时，将需要更新的表复制到当前 shader-visible heap 的连续区域，再设置表地址。[DynamicDescriptorHeap](https://github.com/microsoft/DirectX-Graphics-Samples/blob/e5975f9b0744fc5096dd593ed72bf8f5c004164f/MiniEngine/Core/DynamicDescriptorHeap.cpp)

context 内使用线性游标。当前 heap 不够时，将它退役，从共享池取得另一个 heap，并重新复制有效的绑定表。共享池通过 fence 判断整个 heap 何时可用；没有空闲 heap 就创建新的。因此，日常的小额分配与共享池的同步分开，回收单位是整个 heap。[heap 领取与退役](https://github.com/microsoft/DirectX-Graphics-Samples/blob/e5975f9b0744fc5096dd593ed72bf8f5c004164f/MiniEngine/Core/DynamicDescriptorHeap.cpp#L32)、[context 提交](https://github.com/microsoft/DirectX-Graphics-Samples/blob/e5975f9b0744fc5096dd593ed72bf8f5c004164f/MiniEngine/Core/CommandContext.cpp#L132)

这里的 CPU 描述符存储承担了两个明确责任：复用资源的描述符，以及在切换 heap 后重建绑定表。D3D12 的 `CopyDescriptors` 在 CPU 时间线上立即执行，源必须来自 non-shader-visible heap；这个过程不意味着录制了一条 GPU copy 命令。[Copying Descriptors](https://learn.microsoft.com/en-us/windows/win32/direct3d12/copying-descriptors)

MiniEngine 也提供 root CBV 路径，直接把常量 buffer 的 GPU 地址写入命令绑定。于是，纹理等资源通过 descriptor table，部分常量 buffer 通过 root descriptor；常量数据本身仍由另外的临时内存分配处理。[CommandContext 常量绑定](https://github.com/microsoft/DirectX-Graphics-Samples/blob/e5975f9b0744fc5096dd593ed72bf8f5c004164f/MiniEngine/Core/CommandContext.h#L449)

这个设计优先简化动态表的分配与回收。代价来自整堆粒度和 heap 切换：剩余空间可能暂时不可用，有效表可能需要重拷贝。D3D12 对同时绑定 heap 的数量有限制，部分硬件上的 heap 切换也有额外成本。[Descriptor Heaps Overview](https://learn.microsoft.com/en-us/windows/win32/direct3d12/descriptor-heaps-overview)

### Diligent

Diligent 的 Vulkan 后端使用传统 set／pool，但区分可持有的绑定与动态绑定。持久 set 通过共享 allocator 分配，并延迟单独释放；context 的动态 allocator 从当前 pool 连续取得 set，耗尽后领取新 pool，最后将整批 pool 交给延迟回收机制。GPU 使用结束后，pool 才被 reset 并返回共享池。[DescriptorPoolManager](https://github.com/DiligentGraphics/DiligentCore/blob/cd2329b912aa8add6a4cac538424756534266104/Graphics/GraphicsEngineVulkan/src/DescriptorPoolManager.cpp)、[Vulkan context](https://github.com/DiligentGraphics/DiligentCore/blob/cd2329b912aa8add6a4cac538424756534266104/Graphics/GraphicsEngineVulkan/src/DeviceContextVkImpl.cpp)

D3D12 后端采用共享 GPU heap，每种 heap 的空间进一步划分为持久区域与动态区域。父分配器用按大小检索的空闲区间分配连续空间，释放时合并相邻区间；每个 context 从动态区领取 chunk，再在 chunk 内线性分配。[DescriptorHeap](https://github.com/DiligentGraphics/DiligentCore/blob/cd2329b912aa8add6a4cac538424756534266104/Graphics/GraphicsEngineD3D12/src/DescriptorHeap.cpp)、[VariableSizeAllocationsManager](https://github.com/DiligentGraphics/DiligentCore/blob/cd2329b912aa8add6a4cac538424756534266104/Graphics/GraphicsAccessories/interface/VariableSizeAllocationsManager.hpp)

静态与 mutable 绑定使用可重用的描述符表；动态绑定按需从 CPU 描述符缓存复制到新区域。某些 buffer 绑定还可使用 root CBV／SRV／UAV。这里的混合发生在同一个后端内部，由绑定更新方式决定走哪条路径。[PipelineResourceSignatureD3D12Impl](https://github.com/DiligentGraphics/DiligentCore/blob/cd2329b912aa8add6a4cac538424756534266104/Graphics/GraphicsEngineD3D12/src/PipelineResourceSignatureD3D12Impl.cpp)

它把“长期独立释放”和“短期成批释放”交给不同机制，避免所有请求都承担逐项管理成本。不过，父 allocator 只能重用已经归还的 chunk，看不到仍在使用的 chunk 尾部。静态区与动态区的固定分区也会造成容量隔离：当前实现的动态 GPU 区耗尽时返回失败，需要调整预算或负载，并不自动无限扩张同一个 heap。[动态子分配](https://github.com/DiligentGraphics/DiligentCore/blob/cd2329b912aa8add6a4cac538424756534266104/Graphics/GraphicsEngineD3D12/src/DescriptorHeap.cpp#L539)

### UE

UE 的普通绑定路径继续使用 descriptor set；bindless 路径则把长期资源索引与逐次变化的小量绑定分开。纹理和 buffer 可以持有持久描述符槽位，shader 通过索引访问；另外保留逐次提交的 UB 绑定。UB 在这里经常被单独讨论，是这套 shader 与 RHI 接口的组织结果。UB 也可以拥有持久 bindless 描述符，纹理和普通 buffer 也可以在其他设计中采用临时绑定。[UE Uniform Buffer](https://github.com/EpicGames/UnrealEngine/blob/684b4c133ed87e8050d1fdaa287242f0fe2c1153/Engine/Source/Runtime/VulkanRHI/Private/VulkanUniformBuffer.cpp)、[UE 5.8 bindless manager](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanBindlessDescriptorManager.cpp)

UE 5.7.1 的 descriptor buffer 路径，为临时 UB 描述符使用共享 `FTempBlockAllocator`。多个 context 从共享块取空间，分配过程使用锁和原子游标；描述符直接编码到映射后的目标区域。块记录各使用者的完成点，全部完成后才整块复用，容量不足则取得或创建另一块。[UE 5.7 UB 注册](https://github.com/EpicGames/UnrealEngine/blob/684b4c133ed87e8050d1fdaa287242f0fe2c1153/Engine/Source/Runtime/VulkanRHI/Private/VulkanBindlessDescriptorManager.cpp#L679)、[UE 5.7 临时块分配](https://github.com/EpicGames/UnrealEngine/blob/684b4c133ed87e8050d1fdaa287242f0fe2c1153/Engine/Source/Runtime/VulkanRHI/Private/VulkanMemory.cpp#L4777)

共享块能让不同 context 利用同一块的剩余容量，代价是共享分配状态，以及块的回收被最后一个使用者延迟。这个实现已经可以直接写入目标内存，并不必然存在一份长期保存的 CPU 描述符字节副本。

UE 5.8.4 提供两种 bindless 后端：

| 后端 | 持久资源 | 逐次变化的绑定 |
| --- | --- | --- |
| Descriptor buffer | 放在 descriptor buffer 中 | 临时 UB 使用 `vkCmdPushDescriptorSet2KHR` |
| Descriptor heap | 放在 resource／sampler heap 中 | 使用 heap 索引及 push data 地址映射，packed globals 可走直接地址 |

在 descriptor buffer 分支，`SingleUseUBAllocator` 的职责被移除：应用不再为这些临时 UB 绑定分配描述符字节区间，命令缓冲区负责保存 push 状态。UE 同时要求该路径支持 `bufferlessPushDescriptors`。在 heap 分支，绑定映射允许通过索引或地址表达这类参数。[UE 5.8 UB push](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanBindlessDescriptorManager.cpp#L1024)、[heap 映射](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanBindlessDescriptorManager.cpp#L581)、[扩展要求](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanExtensions.cpp#L1792)

这里改变了责任边界，也改变了应用需要管理的对象：引擎不再显式维护这些临时 UB 描述符的字节区间及回收状态，并不意味着驱动内部不再需要描述符存储。CPU 的绑定元数据、实际 UB 数据和资源存活责任仍然存在；例如 packed UB 数据仍会分配临时内存并写入内容。[UE 5.8 参数更新](https://github.com/EpicGames/UnrealEngine/blob/f45729c0b6f097038e72b52d366a62baeca3275b/Engine/Source/Runtime/VulkanRHI/Private/VulkanPipelineState.cpp#L309)

5.7 到 5.8 的这项变化，利用另一种 API 表达，将短期 UB 绑定的存储与生命周期交给命令缓冲区管理。驱动可以将状态编码在命令流或内部存储中，具体形式由实现决定。源码可以确认引擎侧责任的减少，但不能仅凭这次 API 切换，推导出更少的 GPU 绑定、更少的复制或更低的内存峰值；这些收益需要在对应设备和负载上测量。

## 从约束选择设计

这几个系统反复出现的相似结构，是由不同生命周期推导出来的：持久绑定需要稳定位置和独立回收；短期绑定适合线性分配与成批退役；少量高频变化的参数可以考虑放进命令状态。API 提供实现这些结构的不同工具，具体成本还取决于绑定内容的复用程度和驱动实现。

选择方案时，可以沿着下面的顺序收敛：

1. **先确定什么发生变化。** 只是资源内容变化，可以保持描述符；只是 buffer 内偏移变化，可以考虑 dynamic offset；资源选择变化，再判断是更新描述符还是传递索引。
2. **再确定绑定版本需要保留多久。** 持久对象、单次录制、提交批次和多队列共享数据，适合不同回收边界。优先复用已有提交完成机制。
3. **根据并发决定分配状态归属。** 本地游标减少共享争用，共享当前块减少容量分散。可以共享底层池，而不共享每次分配的游标。
4. **最后选择容量策略。** 固定预算、允许扩容、等待回收，是不同的系统行为。比较 allocator 时，应分别观察固定预算下的成功率，以及允许扩容时的峰值占用。

评价 API 切换时，应分别确认两项结果：引擎需要独立维护的状态和生命周期是否减少，以及 CPU 录制开销、descriptor 更新量、GPU 执行成本和内存占用是否改善。前者可以通过责任边界分析判断，后者需要公平比较可复用 set、动态更新 set、显式 descriptor buffer 与 push 等适用方案。采用 push 或移除自有 allocator，本身不是性能或容量优化的证据。

CPU 描述符缓存也应有具体用途。为了复用昂贵的编码结果、批量复制或在 heap 切换后重建表，它可能值得保留；若绑定只写一次且能直接进入目标区域，就没有必要预设一份永久副本。缓存的必要性来自访问模式。

一个设计是否简化，可以检查它删除机制后，原来的条件由谁保证。局部分配把并发责任交给 context 的单一录制者；整块退役把回收责任交给更粗的 GPU 完成边界；push 把临时绑定存储交给命令缓冲区。每种变化都有相应代价，但都能说清楚减少了哪些独立状态，以及什么仍然必须保留。
