# RDG 性能排查与优化记录

本文记录 2026-10-28 的 RDG 性能排查。第一部分固定上一轮的代码改动与测量结果；后续优化另行追加，避免把不同阶段的收益混在一起。

## 1. 问题与责任边界

完整 RDG 接管之后，简单场景的 Raster 从约 90 FPS 降到 30–40 FPS，Raytracing 也明显变慢。受控相邻提交对比确认，两条管线各自的完整接管提交都造成了大幅回退，时间早于后续的 PassExecutionClass 整理。

RDG 仍负责资源依赖、子资源状态、屏障和执行计划；RHI 负责命令录制、提交和 GPU 完成后的资源回收。性能优化必须保留这些责任，以及失败时不发布不完整工作的约束。每帧建图本身不能解释或合理化当前回退。

本轮用户授权范围是：保持架构，优化实现。查找索引、遍历范围、容量增长及重复计算可以调整；改变资源访问契约、图的边界、录制策略或生命周期所有权，需要先讨论具体方案并取得同意。

## 2. 上一轮的具体优化

### 2.1 声明变更只清理真正存在的编译结果

- 位置：`RenderGraph.cpp`，`InvalidateCompile`。
- 原来：添加资源、访问或边界声明时，即使图尚未编译，也反复清空编译容器并扫描资源、重置使用区间。
- 现在：图已有编译结果或上次编译错误时才做清理。首次建图期间不反复执行这些工作。
- 正确性：已编译图修改后仍会失效；执行后或不允许修改的 setup 阶段仍拒绝变更。省掉的是不存在的结果的清理，不是失效规则。

### 2.2 按名称和物理身份分别索引导入资源

- 位置：`RenderGraph.h/.cpp`，`resource_names`、`imported_resource_ids`。
- 原来：每次导入都在线性资源表中查找名称和底层对象身份。
- 现在：图内维护名称索引和物理对象索引；名称查询支持 `string_view`，避免为查询构造字符串。
- 正确性：名称与物理身份仍是两个独立判定条件；不同身份的名称冲突仍报错，空身份不会被视为同一对象。临时资源的名称约束不变。索引仅属于当前图，不跨帧缓存句柄。
- 验证：增加了名称与物理身份碰撞场景的 RenderGraph 测试。

### 2.3 语义依赖分析只遍历当前 Pass 实际访问的资源

- 位置：`RenderGraphCompiler.cpp`，`BuildSemanticDependencies`。
- 原来：每个 Pass 都重新建立全部资源的 cell 使用表，并遍历完整资源集合。
- 现在：编译期间复用按资源组织的 cell 使用存储，记录本 Pass 访问的资源，只处理这些资源；对访问资源排序，保持输出的确定性。
- 正确性：读写依赖、版本、子资源覆盖和状态合并规则没有变化。改变的是遍历范围。

### 2.4 命令合并采用几何容量增长

- 位置：`RHICommandList.cpp`，`AppendRecorded`。
- 原来：每次合并调用 `reserve(已有数量 + 新增数量)`；连续合并许多小 Pass 时可能反复搬移整个前缀。
- 现在：容量不足时按约 1.5 倍增长，并确保至少容纳此次追加。
- 正确性：命令顺序、payload 所有权和保活规则不变。需要的容量仍在转移 payload 前准备，保留分配失败时的行为。

上述四项已包含在 `0fd4f02d`：`perf(rdg): reduce declaration, dependency and command merge overhead`。

### 2.5 没有阴影绘制时不建立场景读取集合

- 位置：`renderer/raster/RasterPassGraph.cpp`。
- 原来：即使阴影绘制列表为空，也导入场景资源并准备读取声明。
- 现在：只有存在实际绘制时，才构造这部分读取集合和索引。
- 正确性：保留 ShadowDepth.Ready 和 bindless 发布流程；有绘制时仍声明原来的全部资源访问。

### 2.6 完全相同的边界声明去重

- 位置：`RenderGraph.h/.cpp`，`StateDeclaration`、初始与最终状态登记。
- 原来：重复导入帮助函数会重复添加相同的初始状态和导出状态。
- 现在：通过默认比较判断整条声明是否相同，完全相同才省去追加。
- 正确性：状态、范围、队列或访问模式不同的声明仍保留，并交给原有冲突检查处理。

### 2.7 子资源分割前按资源归集访问范围

- 位置：`RenderGraphCompiler.cpp`，`BuildAtomicCells`。
- 原来：每处理一个资源，重新扫描全部 Pass 的访问以收集此资源的分割端点。
- 现在：先遍历访问一次，建立按资源索引的范围列表，再使用各自的范围生成 cells。
- 正确性：纹理 mip/layer/aspect、buffer 字节区间以及初始和最终边界的分割规则不变。该局部索引只存编译期间有效的范围指针。

### 2.8 缩小“临时资源缺少生产者”检查范围

- 位置：`RenderGraphCompiler.cpp`，`BuildSemanticDependencies` 末尾。
- 原来：为每个资源扫描完整访问表和 opaque 引用，之后才判断它是否是需要检查的临时资源。
- 现在：先筛出未被写入的临时资源；GPU 访问从已有 cell 的 `last_access` 判断，opaque 引用仍按原规则检查。
- 正确性：已导入资源不需要图内生产者；已写入资源已经有生产者。其他临时资源的非法读取和引用仍被拒绝。

### 2.9 Lowering 按 Pass 和资源归集数据

- 位置：`RenderGraphLowering.cpp`。
- 原来：为每个 Pass 建立保活集合时扫描全部访问；验证每次访问的导入/导出边界时扫描全部边界。
- 现在：访问先按 Pass 归集，边界先按资源归集，只在相关集合中完成原有工作。
- 正确性：保活集合仍包含 GPU 访问和 opaque 引用；边界匹配仍检查资源、精确范围、prologue/epilogue 成员关系以及 import/export 标志，没有放宽覆盖验证。

2.5–2.9 在本轮开始时仍是未提交工作区改动。没有为单项优化分别进行受控 A/B，因此不能给每项分配一个独立的毫秒收益。

## 3. 受控测量结果

条件：Windows、MSVC VS2022 Debug（`/Ob0 /Od /RTC1`）、RTX 4080 SUPER、相同 Sponza、1920×1080 窗口、1500×1002 场景视口、固定 ImGui 布局、Vulkan validation 开启、CUDA 开启、NRD 关闭、垂直同步和帧率锁关闭、RDG 并行录制关闭、ProfileDump 开启。每次预热后取最后 30 秒。

根配置保留了旧的 `render_graph = false` 字段；完整接管后的版本不再通过它切回 linear 路径。这里关闭的是 `render_graph_parallel_recording`，图编译与执行仍然启用。

FPS 根据相邻 `RenderFrame` 开始时间的间隔计算，不是对一个 CPU scope 耗时简单取倒数。

| 管线 | 版本 | FPS | 帧间隔 ms | RenderFrame ms | PrepareFrame ms |
| --- | --- | ---: | ---: | ---: | ---: |
| Raster | `e971a229`，接管提交的直接父提交 | 108.51 | 9.22 | 8.74 | 0.40 |
| Raster | `15e64157`，完整接管 | 28.64 | 34.92 | 34.35 | 0.49 |
| Raster | `0fd4f02d` + 上述工作区优化 | 45.21 | 22.12 | 21.58 | 0.47 |
| Raytracing | `15e64157`，RT 接管的直接父提交 | 43.77 | 22.85 | 4.86 | 17.88 |
| Raytracing | `1244d4f4`，完整接管 | 20.07 | 49.83 | 31.81 | 17.89 |
| Raytracing | `0fd4f02d` + 上述工作区优化 | 24.27 | 41.20 | 23.33 | 17.75 |

同一帧内所有 `RenderGraph.Compile` 调用合计：Raster 从接管版本的约 6.93 ms 降至当前约 3.67 ms；RT 从约 4.55 ms 降至约 2.84 ms。Raster 当前存在三个图调用，RT 存在两个，不能只看单次 Compile 的平均值。

这些结果证明接管提交引入了回退，也证明当前版本仍未恢复。历史接管版本与当前版本之间还包含其他重构，不能把全部差值归功于上述性能补丁。CPU scope 存在嵌套，尤其 UI 与录制 scope，不可直接相加作为独占耗时。GPU 同步成本也尚未逐项归因。

原始记录保存在本机 `build/rdg-bisect-results/`，汇总为 `summary.json`，每个运行目录含 `capture.mpd`、解析结果、日志、配置和进程元数据。这些是忽略目录内的本机证据；本节保留可随仓库提交的关键条件和数值。

## 4. 已做验证与尚未解决的问题

上一轮完成 Debug 构建、RDG 相关测试及 Raster/RT 运行验证。受控历史与当前版本的七次运行均正常退出，日志未发现 error、critical、VUID 或 Validation Error。上述结论不等于已经证明所有场景、队列组合或并行配置均无问题。

尚未解决的是：当前 CPU 帧时间仍显著高于接管前。需要继续区分建图、依赖编译、lowering、命令录制/合并以及提交等待的成本，并对新改动进行相同条件的前后对比。

## 5. 第二轮优化

本轮从 `0fd4f02d` 加 2.5–2.9 的工作区状态开始。先补充热点测量，再做保持同一资源声明、编译结果和执行契约的实现优化。新的改动、验证与实测结果将在本节记录。

### 5.1 测量与实现边界

临时计时拆分了编译各阶段、lowering、录制前验证、命令合并和主要 Pass 的建图。用于定位的 Raster 主图包含 67 个 Pass、138 个资源、800 条归一化访问、132 条依赖边和 417 条编译屏障。完整帧还含 UI 图。额外探针会产生开销，不能直接拿这组 FPS 与第一轮数据比较；正式前后对比移除全部临时探针。

Shadow 和 Probe 的 AddToGraph 各包含约数毫秒的准备工作，但其范围同时覆盖既有场景计算，不能把它们全部计为图管理开销。lowering 在原有 `RecordAndMergeFrontendCommands` scope 之前执行；只把已有 Compile 和录制 scope 相加会遗漏这部分。

### 5.2 拓扑排序只访问当前节点的出边

原来每选出一个节点，都从头查找最小的可执行 Pass，再扫描全部边更新入度。现在用最小堆维护可执行 Pass，并利用已经按源 Pass 排序的边数组定位当前节点的出边。

最低声明序号优先的规则保留，因此确定性顺序不变。节点没有全部处理完时仍报告依赖环。除边的既有排序外，遍历成本从反复的全表扫描变成出边遍历、二分定位和堆操作。新增的 ready 集合只是此次拓扑计算的局部临时数据。

### 5.3 依赖层级从已经验证的拓扑顺序计算

原来每个层级都扫描所有 Pass，并为该层级的每个 Pass 再扫描全部边。现在沿拓扑顺序把 `当前层级 + 1` 传播到后继，后继取所有前驱中的最大值；最后按声明顺序把 Pass 放入相应层级。

这一结果与原来的逐波消除入度一致：一个 Pass 必须位于最深前驱之后。同层内部依然按声明序号排列。临时资源 alias 边也参与计算，前面的拓扑完整性验证继续执行。没有改变 GPU 顺序、CPU 录制分组或并行策略。

### 5.4 边登记使用编译期索引

`AddEdge` 原来为每条依赖在线性边表中寻找 `(src, dst)`，再在线性 reason 表中检查重复。现在用两段 32 位 Pass ID 组成 64 位 key，在编译器内索引已有边；同一对端点仍合并到一条边。

reason 在收集时追加，在消费者读取计划之前通过 `SortEdges` 统一排序、去重。一条边收到 k 条原因时，原来逐条查重最坏需要 O(k²) 次比较；现在使用已有确定性比较规则排序，再线性去重，成本为 O(k log k)。中间可能短暂保留重复原因，最终输出集合和顺序保持一致。

边索引的代价是额外的 O(E) 临时存储。它由边数组派生，只活在一次编译中，不进入公共计划，不跨图缓存。语义排序和 alias 规划后的排序统一经过 `SortEdges`，同步修正索引，避免排序之后把新 reason 加到错误的边。如果 alias 规划没有产生任何 alias 边，边表仍然是原来的规范顺序，省去第二遍排序和去重。

### 5.5 复用分割端点存储，省去重复的资源外边界

纹理 mip/layer 端点和 buffer 端点改为在资源之间复用临时数组容量。每个资源开始时重新填入 `0` 和完整尺寸；访问只需再添加内部切分端点，重复的 `0` 和完整尺寸不必反复插入、排序。

资源的两个外边界始终存在，所有内部端点仍保留并排序去重，因此生成的原子区间不变。归一化访问数组也根据已知声明数预留容量，减少搬移。

### 5.6 Lowering 直接定位 Pass 指令容器

在建立 `candidate.passes` 时，同时建立按 Pass ID 索引的容器位置表。原来的线性 Pass 查找改为先验证 graph ID/Pass ID，再读取索引。没有 GPU 指令容器的 Pass 仍返回空，前面的无效或重复执行顺序检查保留。

每个 Pass 的保活列表在完成资源去重后，按已知数量一次预留容量。资源引用、释放时机和最终输出顺序都保持不变。

### 5.7 验证

新增 `TestDependencyLevelOrdering`：覆盖长短分支汇合、较晚声明的独立节点、新就绪节点的最小序号优先，以及每层内部顺序。现有六个 RDG CTest 覆盖子资源区间、依赖、alias、lowering、异步 setup、并行录制、失败与生命周期契约；带探针的完整候选版本六项均通过，Raster 运行没有发现 error、critical、VUID 或 Validation Error。

所有临时计时探针和测试入口已从正式源码移除。最终代码再次通过全部六项 RDG CTest。测试版对照构建曾因复制文件保留旧时间戳而复用旧对象，造成内部布局不一致；对应比较已作废。重新触发依赖编译后，前后 DLL 哈希不同，下面只采用重新验证的结果。

本轮没有改为共享帧命令流，没有引入外部只读资源模式、跨帧图缓存或图内存池，也没有关闭验证来换取性能。

### 5.8 正式对比

正式基线是第一轮全部优化后的工作区实现，不是 RDG 接管之前的版本。

#### 固定图编译基准

独立进程使用同一个基准程序，分别加载前后版本的 Render DLL。每次运行预热 5 次，再测量 80 次。图包含 64 个 Pass、64 个纹理和 64 个 buffer；每个 Pass 访问 16 个纹理和 16 个 buffer，每四个 Pass 有一个写入，其余读取。编译得到 2048 条访问、244 条边和 1652 条屏障。计时只覆盖 Compile，不包含建图、Dump 或图析构。

| 交替运行 | 优化前编译 p50 ms | 优化后编译 p50 ms | 优化前 p95 ms | 优化后 p95 ms |
| --- | ---: | ---: | ---: | ---: |
| 1 | 16.6076 | 10.3488 | 18.0181 | 11.1580 |
| 2 | 16.6419 | 10.5629 | 17.6682 | 11.5337 |
| 3 | 17.0417 | 10.2306 | 18.7321 | 11.2867 |

取三次 p50 的中位数，编译耗时从 16.6419 ms 降至 10.3488 ms，减少约 **37.8%**。建图 p50 约 3.3–3.5 ms，未出现对应幅度变化。前后六次运行的 Dump FNV-1a 摘要均为 `15891381205224239550`；这验证该固定输入的完整文本计划一致，不替代其他输入的契约测试。

核验 DLL SHA-256：

- 优化前：`7C5CFD6362F85ECE5814A2F9B4486A2635F15DBEB5526C1B96A959D2AD4A2D3E`
- 优化后：`AE47F7249AF1DBA0D9F2EB13AB19C11E94D033BFC7E9E570539622A29C4CC7A4`

原始输出为 `build/rdg-round2-verified-{before,after}-{1,2,3}.txt`。固定图属于 CPU 编译压力基准，**37.8% 不能解释为编辑器帧时间或 FPS 的改善比例**。

#### 编辑器运行

同轮编辑器运行出现明显波动：未修改的 PrepareFrame 有时从不足 1 ms 升至约 11 ms，并出现长帧。单次整体 FPS 不足以归因。以下四次交替运行使用同一场景、配置和原有 ProfileDump，未包含临时探针。

| Raster 运行 | FPS | RenderFrame p50 ms | 每帧 Compile 合计 p50 ms |
| --- | ---: | ---: | ---: |
| 优化前 1 | 27.05 | 24.23 | 3.72 |
| 优化后 1 | 31.46 | 20.98 | 2.74 |
| 优化前 2 | 30.35 | 21.64 | 3.36 |
| 优化后 2 | 32.69 | 23.25 | 2.87 |

每帧 Compile 合计通过把 Compile scope 归入其所属 RenderFrame 后求和，再计算帧样本的中位数，避免把三个图的单次调用均值当作每帧耗时。FPS 仍按帧起始间隔计算。

实际场景中，编译合计 p50 从 **3.36–3.72 ms** 降至 **2.74–2.87 ms**。RenderFrame 和 PrepareFrame 的变化没有保持一致，不能把 FPS 的全部变化归因于本次优化，也不能据此报告已经恢复到原先约 90 FPS。四次运行均正常退出，日志没有发现 error、critical、VUID 或 Validation Error。

运行目录依次为 `round2-before-final`、`round2-after-final-raster`、`round2-before-repeat-raster`、`round2-after-repeat-raster`，位于 `build/rdg-bisect-results/`。每个目录的 `per-frame-summary.json` 记录完整统计和实际加载的 DLL 哈希。

Raytracing 最终版本也完成了 110 秒运行并正常退出，日志没有发现 error、critical、VUID 或 Validation Error。记录为 21.25 FPS、RenderFrame p50 23.15 ms、每帧 Compile 合计 p50 2.23 ms。该次主要验证 active recording 路径；没有同轮的 RT 基线对照，不能把它与第一轮的 24.27 FPS 直接相减来归因。原始记录在 `round2-after-final-ray`。

测试结束后，所有测试编辑器已关闭，根目录和可执行目录配置均恢复为原先的 Raster 配置，正式 DLL 为上述优化后版本。

### 5.9 剩余问题与下一步边界

本轮优化了编译器的重复查找、排序、遍历和临时存储，没有解决全部帧时间回退。当前 Compile 只占 RenderFrame 的一部分，继续只压缩编译器不足以消除剩余差距。后续应继续测量录制、lowering、合并及非 RDG 的场景准备开销。

如果进一步优化需要改变逐 Pass 命令流、资源声明契约、图边界或数据所有权，应先给出责任转移与失败/生命周期处理方案，取得用户同意后再实施。这些架构调整不包含在本次代码中。

## 6. 第三轮：省去无同步作用的只读边界屏障

### 6.1 约束与实施计划

本轮只处理用户选定的第一项：边界状态证明不必一律转换成 GPU 屏障。起点为 `29340ec4`，保留 RDG 对依赖和屏障的所有权、现有录制路径和资源生命周期。

参考本机 UE 5.7.1（`684b4c133ed8`）的 `FRDGSubresourceState::IsTransitionRequired`：相同状态并不总需要 transition，但 UAV、管线变化等仍可能要求同步。此处不直接移植 UE 的状态规则，而从本项目已有编译标志推导可省略条件。

实施顺序：

1. 固定当前配置、构建并保存基线，检查编译边界、lowering、物化和 Present 发布之间的契约。
2. 在 RDG 输出实际屏障的位置过滤已证明无同步作用的只读边界；保留完整边界验证以及物理资源、范围合法性检查。
3. 保留写入可见性、执行依赖、布局或访问状态变化、跨队列与所有权同步、alias 和 Present 状态发布；保活集合仍从资源访问产生。
4. 增加只读 buffer/texture 的正向测试，以及写入、跨队列、Present、非法边界等反向验证，覆盖合并录制和 active recording。
5. 完成 Debug 构建、相关测试与 Raster/RT 运行；使用一致配置比较实际录制屏障数量和帧时间。记录未验证事项，不把屏障减少比例当作 FPS 提升比例。

### 6.2 输出规则与责任归属

`CompiledPlan` 和 `LoweredPlan` 保留原来的边界、依赖、来源和诊断数据，`Lower` 的完整性验证不变。过滤放在 `MaterializeInstruction`：成功时输出 `std::optional<BarrierCreateInfo>`，有值表示实际 RHI 屏障，无值表示边界已经验证但无需 GPU 命令；失败仍返回 false 并给出错误。每次进入先清空输出，避免复用旧结果。

只有普通 Barrier 类型的 import/export 边界，且前后都是 Read、资源状态相等，才进入省略判断；存在 state transition、memory dependency、execution dependency、queue acquire、discard 或 transient alias 中任意一项时都保留。QueueRelease/QueueAcquire 不属于普通 Barrier，因此不会被省略。前后 shader stage 不要求相等：同队列、同状态的纯读取不会仅因着色阶段不同而产生 hazard。

纹理物理绑定、aspect、范围和 uint8 TextureView 可表示性，以及 buffer 绑定与字节范围仍先检查。只读快速路径不会接受此前因为非法范围而被拒绝的指令。

Present 导出仍保留实际屏障与 `publish_external_state`。除显式 PresentationSource 状态外，对带 PRESENTATION_SOURCE usage 的纹理导出也保守保留，因为 Vulkan 的提交检查需要这条记录发布呈现就绪状态。

合并录制和 active recording 两处都只追加有值输出；保活集合继续由访问声明生成，不能依赖屏障是否存在。没有把屏障决策移回 Vulkan，没有更改逐 Pass 命令流、图边界或外部资源访问契约。本轮减少的是屏障物化、复制、录制和后端处理；编译与 lowering 的诊断记录成本仍然存在。

### 6.3 契约验证

完整 Debug 构建通过。本机现有 build 使用 VS2022/MSVC，使用 `cmake --build build --config Debug --parallel 30`，未把本轮结果当作 Clang 或 Release 的测量。

相关 8 项 CTest 全部通过：RenderGraph、ParameterAccess、AsyncSetup、Lowering、ActiveLowering、ResourcePool、PresentationSurface 和 RHIExplicitBarrier。新增测试编译后单独重跑 ActiveLoweringContract 也通过。

新增用例检查：

- 两个只读 Pass 同时读取 buffer 和 texture，覆盖完整/局部范围及合并/active 两条录制路径。编译计划仍有 2 条 import 和 2 条 export 边界证据，实际提交没有屏障命令，两个 Pass 的正文顺序不变。这里的 4→0 是测试图的结果，不代表 Sponza 全帧的屏障数量。
- 回调和正文不持有资源引用；图与外部引用销毁后，资源由录制结果和 submit 保活，最后一个 submit 释放后销毁。省掉屏障不会使资源提前释放。
- 同 Storage 状态的初始 Write→Read 仍有 1 条屏障；初始 Write→Pass Write→导出 Read 仍有 2 条屏障。
- 显式 PresentationSource 导出，以及带 PRESENTATION_SOURCE usage 的纹理导出，都保留 `publish_external_state`。
- 256 层纹理不能由当前 uint8 TextureView 表示；即使全程同状态只读，两条录制路径仍在调用正文或发布结果之前拒绝它。

已有 Lowering 与 RHI 契约测试继续覆盖同原生队列、同 family 不同队列、跨 family ownership transfer、缺失跨队列同步和显式屏障提交。测试不能替代真实 GPU 验证，运行记录另列如下。

### 6.4 编辑器对照与收益边界

在同一 Sponza 场景、1920×1080 窗口、关闭 VSync/锁帧及 Raster 并行录制的配置下，依次运行优化前、优化后、优化前、优化后。每次运行 110 秒，剔除启动和关闭附近样本后取 30 秒统计。使用相同编辑器和固定的两份 Render DLL，运行前确认没有其他编辑器；切换 DLL 时所有测试进程均已退出。

| Raster 运行 | FPS | RenderFrame p50 ms | 每帧 Compile 合计 p50 ms | 每帧录制/合并合计 p50 ms |
| --- | ---: | ---: | ---: | ---: |
| 优化前 1 | 40.90 | 23.2052 | 3.1963 | 6.4171 |
| 优化后 1 | 46.72 | 20.6080 | 2.9333 | 5.6377 |
| 优化前 2 | 45.16 | 21.2496 | 2.8590 | 5.9482 |
| 优化后 2 | 46.39 | 20.6681 | 2.9230 | 5.8214 |

“每帧合计”将包含于同一 RenderFrame 的各图 scope 先求和，再计算中位数；录制/合并对应 `RenderGraph.RecordAndMergeFrontendCommands`，包含任务创建、回调录制和命令合并等工作，不是单独的屏障耗时。该 scope 在 `Validate()` 之后开始，不包含其中的 lowering 和屏障物化。FPS 由帧起始间隔计算。

第一组未修改的 Compile 也变快，且两次基线 FPS 差异较大，说明存在运行波动。第二组 Compile 接近，录制/合并 p50 减少约 **0.127 ms（2.1%）**，FPS 从 45.16 到 46.39。可以确认测试图的冗余边界屏障已去除，两组运行也都观察到录制/合并耗时下降；但这些样本不足以确定稳定的整体加速比例，更不能报告已恢复原先约 90 FPS。

本轮没有测量 Sponza 全帧的实际屏障条数，也没有给编译或 lowering 增加新的计时探针。保留边界证据及其遍历成本是本次小范围优化的明确取舍；若后续要消除这部分成本，应单独讨论边界证明的表示方式，不能直接删除目前依赖它的验证。

四次 Raster 均正常退出，日志未发现 error、critical、VUID 或 Validation Error。原始配置、进程信息、capture 和 `per-frame-summary.json` 在 `build/rdg-bisect-results/` 的以下目录：`boundary-before-raster`、`boundary-after-raster`、`boundary-before-repeat-raster`、`boundary-after-repeat-raster`。解析脚本为 `build/analyze_rdg_boundary.py`，沿用既有 ProfileDump 格式和时间窗口规则。

DLL SHA-256：

- 优化前：`04F32FF087DCC43893C608911B4DC5626F5AC1493090DE7178C122D6F6463437`
- 优化后：`11B375BB8FEE3DE356A3B657FC258DDBA3903764883F230989D517883BC7DB0F`

Raytracing 最终版本也完成 110 秒运行并正常退出，日志未发现 error、critical、VUID 或 Validation Error。记录为 24.38 FPS、RenderFrame p50 22.1827 ms、每帧 Compile 合计 p50 2.2204 ms、active recording 合计 p50 4.9032 ms，原始记录为 `boundary-after-ray`。本轮没有 RT 优化前对照，因此只作为 active recording 路径的运行验证，不据此报告 RT 性能提升。

结束时所有测试编辑器已关闭，正式 Render DLL 为上述优化后版本。运行使用独立配置覆盖文件，仓库默认 Raster 配置未改动。本轮只实施只读边界的屏障物化过滤；共享命令流、外部只读资源模式和边界数据结构重组均未实施。

## 7. 第四轮：串行共享暂存流，并行按工作量分组

### 7.1 约束、授权范围与计划

用户在第三轮之后明确授权第二步：串行区域共用暂存命令流，并行区域按工作量分组。本轮基线包含第三轮尚未提交的只读屏障过滤，Render DLL SHA-256 为 `11B375BB8FEE3DE356A3B657FC258DDBA3903764883F230989D517883BC7DB0F`，不能拿第二轮或第一轮的旧二进制作为本轮基线。

逐 Pass 独占 CommandList 并非 GPU API 的强制要求。真正需要保留的是：并发写入时各任务独占自己的流；跨队列 wait/signal 和 SerialControl 保持原来的提交边界；每个 Pass 的 before/body/after、性能标记与资源生命周期准确；任一录制失败时不提交半张图。串行 Pass 可以按编译顺序写入同一条私有流。

实施顺序：

1. 保留编译计划中的逐 Pass 身份和 CPU 调度约束，在执行时派生实际录制区域及流范围。
2. 合并路径让串行区域共享暂存流，并行区域每块一条流、一个任务；全部成功后再合并到调用方目标。
3. active 路径让每条实际流共用 producer gate、managed lease 和生命周期包；保留全图 commit gate，内部队列 wait/signal 拆分流。
4. 将 source 配置回调统一为流级回调，提供该流内全部 Pass；迁移 RT 的 graph/NRD/UI 提交回执，保留逐流失败检查和 UI 独立判断。
5. 更新原来假设“一 Pass 一 source”的测试，验证共享流、工作量分组、顺序、失败原子性、生命周期和 profiling；完成 Debug 构建、契约测试、Raster/RT 和并行录制验证，再记录同配置性能对照。

### 7.2 分组方法与责任边界

`CompiledFrontendRecordUnit` 仍表示一个 Pass。执行时，关闭并行或没有足够 TaskGraph worker 的相邻 CPU 组可以合并为一个串行区域；开启并行时，串行区域和并行区域分开，按原有 CPU 边界等待已启动任务。GPU 资源依赖本身不阻止 CPU 并行录制，最终命令顺序和屏障仍来自 RDG。

`PassBuilder::RecordWorkload(units)` 声明相对 CPU 录制工作量，默认 1，必须大于 0。一次普通 draw/dispatch 可使用默认值；循环产生大量命令的回调可以用操作数量估计。它不是 GPU 耗时，也不通过资源访问数猜测 CPU 工作。参考 UE 5.7.1 的 `FRDGPass::Workload` 与 `SetupParallelExecute`，采用显式估计，未引入跨帧历史缓存及其身份/失效管理。

并行区域目标块工作量取 `max(8, ceil(区域总工作量 / worker 数))`，按编译顺序累积切块；单个重 Pass 不拆开。8 是初始任务粒度策略，不是正确性约束。串行区域不按工作量拆块。所有区域遇到队列变化或 SerialControl 两侧都拆流；active 路径还在后一 Pass 的 wait、前一 Pass 的 signal 处拆开，保证 wait 位于流开头、signal 位于流末尾。

GPU profiling 每条实际流只绑定一次，使用首 Pass 的编译位置作为 source order，允许编号有空洞；流内仍逐 Pass 记录名称和 marker。配置回调接收 `span<const ExecutedPassInfo>`，从而能够将某 Pass 的提交回执挂到包含它的实际流。RT 每流登记 graph/NRD 回执；包含 UI draw 的流登记 UI 专属回执，后续仍单独检查 UI 是否被原生队列接受，不能用整图成功代替 UI 生命周期判断。

配置回调中的 span 及 Pass 名称是借用视图，只在回调期间有效。当前多次复制的 `ProbeUpdate.Upload` 按实际非空上传项数声明工作量，其余单 draw/dispatch Pass 使用默认值；后续可以根据录制测量完善重 Pass 的估计，不把 GPU 工作规模直接当作 CPU 录制成本。

合并路径仍先录私有暂存流，整图成功后才写目标；active 路径仍有全图 commit gate，每流 gate 只在整条流停止写入后终结。资源引用和回调捕获统一由流的生命周期包保活。将分配、回收和 producer 同步移到更粗的流边界，不改变 RDG 对 GPU 依赖、资源状态和跨队列同步的所有权。

### 7.3 契约检查

完整 Debug 构建通过，仍使用本机现有 MSVC 构建目录。9 项相关 CTest 全部通过：上一轮的 8 项加 RHIGpuScopeContract。

新增或调整测试验证实际责任，而非原来的“一 Pass 一 source”实现假设：串行 Pass 共享私有流但不直接修改调用方目标；工作量 `{1,1,6}` 与 `{1,7}` 形成两组，组间逆序完成仍按编译顺序输出；跨 queue 或 SerialControl 不合流；一条流配置时可见其中全部 Pass，profiling 只绑定一次但每 Pass 保留 scope；回调和资源仍活到最后的 submit/completion owner 释放。需要验证真实并发的测试显式声明工作量，单个小任务允许在调用方执行。

串行第一段失败后，后续未启动段不再调用正文，其 producer gates 全部终结为 Failed；整图 commit 也失败。任务派发发生异常时，先保存 `exception_ptr`、终结未派发槽并等待所有已启动 worker，再格式化错误，防止错误处理再次分配失败而绕过 join。该处理保护共享 executor/lease 的生命周期，不依赖新的同步层。

`TestRHIParallelRecordVulkan.exe --parallel` 完整默认集成测试通过，启用了 Vulkan validation，日志未发现 VUID 或 Validation Error。覆盖实际显式屏障读回、Graphics/Compute 所有权转移、Graphics/Copy 往返、buffer/texture transient alias、array subrange，以及拒绝和资源回收路径。记录位于 `build/rdg-stream-vulkan-parallel.log`；CPU 契约记录为 `build/rdg-stream-contracts-final.log`。

### 7.4 实际运行对照

使用同一 Sponza、1920×1080 窗口、关闭 VSync/锁帧的 Debug 配置。每次运行 110 秒，沿用原有 ProfileDump，在排除启动和关闭附近样本后取 30 秒。逐帧先汇总各图 scope，再计算 p50；FPS 来自帧起始间隔。串行与并行配置分别比较，不把开关变化计作代码收益。

固定二进制 SHA-256：

- 本轮优化前：`11B375BB8FEE3DE356A3B657FC258DDBA3903764883F230989D517883BC7DB0F`
- 本轮优化后：`41477C3B113D03811F1CF5CD125D38E4783FA9935A92DEED5CDD667E4F9D295E`

| 配置与运行 | FPS | RenderFrame p50 ms | 每帧 Compile 合计 p50 ms | 每帧录制/合并合计 p50 ms |
| --- | ---: | ---: | ---: | ---: |
| Raster 串行，前 1 | 49.47 | 19.3224 | 2.6353 | 5.5509 |
| Raster 串行，后 1 | 59.45 | 16.0586 | 2.6199 | 3.1607 |
| Raster 串行，前 2 | 49.65 | 19.2900 | 2.6425 | 5.5210 |
| Raster 串行，后 2 | 59.30 | 16.0604 | 2.6301 | 3.1557 |
| Raster 并行，前 | 51.01 | 18.8359 | 2.6324 | 5.0147 |
| Raster 并行，后 | 60.21 | 15.8440 | 2.6435 | 2.7524 |

两组串行对照的 Compile 接近，录制/合并 p50 均减少约 **43%**，FPS 提升约 **19–20%**。并行对照的录制/合并 p50 减少约 **45%**，FPS 提升约 **18%**。首组新版本运行期间曾并发解析旧 capture，因此另做了稳定测量窗口内不运行分析进程的串行复测，结果一致。并行和 RT 各只有一组前后对照，不把单场景 Debug 数字外推为其他场景或 Release 的固定收益。

Raster 的录制/合并 scope 不包含 Validate 内的 lowering；RT 的 active recording scope 包含 lowering，两者不能当作相同计时口径比较。

| RT 串行 | FPS | RenderFrame p50 ms | PrepareFrame p50 ms | 每帧 Compile 合计 p50 ms | active recording p50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| 优化前 | 25.27 | 21.4459 | 17.5575 | 2.1683 | 4.7266 |
| 优化后 | 33.73 | 11.7182 | 17.5213 | 2.1345 | 3.8336 |

RT 的 PrepareFrame 基本不变。active recording 约减少 0.893 ms，RenderFrame 则约减少 9.728 ms，说明收益不只发生在前端录制函数内。更少 source 同时改变了下游翻译、提交、profiling 和回执处理的工作量，本轮没有逐项计时，不能将全部差额都归入某一项。

从 GPU scope 事件按帧聚合 `(logical_queue, source_order)`，Raster 串行的 profiling source 数为 51→2，并行为 51→11；RT 串行为 32→3。这是实际 profiler source 数，不等同于 `vkQueueSubmit` 次数。稳定完整帧中，Raster 前后均保留 53 个同名 GPU scope，RT 均保留 37 个；没有通过删除逐 Pass 标记降低开销。流级保活可能把个别资源的释放延后至整个流完成，但原有编译生命周期、alias 屏障和 completion 约束仍然成立。

八次编辑器运行均完成 110 秒并正常退出，日志未发现 error、critical、VUID 或 Validation Error。RT 编辑器对照使用串行录制；active 并行由契约测试和真实 Vulkan 集成测试覆盖。

原始记录位于 `build/rdg-bisect-results/`，各目录保存实际配置、DLL 哈希、进程信息、capture 与 `per-frame-summary.json`：

- 串行 Raster：`stream-before-raster`、`stream-after-raster`、`stream-before-repeat-raster`、`stream-after-repeat-raster`。
- 并行 Raster：`stream-before-parallel-raster`、`stream-after-parallel-raster`。
- 串行 RT：`stream-before-ray`、`stream-after-ray`。

沿用 `build/run_rdg_bisect.ps1`，并行 Raster 使用 `-Parallel`，分析使用 `build/analyze_rdg_boundary.py`。结束时所有测试编辑器已关闭，正式 DLL 恢复为本轮优化后版本；根目录和可执行目录默认配置仍为 Raster、关闭并行录制，未修改用户配置。

## 8. 第五轮：编译器独占的 CPU 暂存 arena

### 8.1 真实约束、范围与实施计划

用户同意先为编译器临时数据引入独立分配器。本轮基线包含第四轮共享录制流，Render DLL SHA-256 为 `41477C3B113D03811F1CF5CD125D38E4783FA9935A92DEED5CDD667E4F9D295E`。这是 CPU 编译暂存内存优化，不涉及 GPU 资源池、显存 alias 策略或 RDG 调度架构。

编译内部的规范化访问、原子区间、依赖边索引和临时排序数组，只需存活到本次编译返回。`RenderGraph::Compile()` 等待 setup 完成后，在调用线程构造临时 `RenderGraphCompiler`，返回时销毁。可以直接在这个所有权边界统一回收，无需全局分配器、TLS、锁或跨帧缓存。

实施顺序：

1. 在 `RenderGraphCompiler` 内置 `std::pmr::monotonic_buffer_resource`，先构造资源、再构造使用它的容器，析构顺序相反。
2. 迁移内部成员表及不逃逸的局部数组；嵌套 PMR 容器通过标准 allocator 传播使用同一资源。循环内重复使用的数组保留容量并复用，避免每轮循环扩容都占据 arena 到编译结束。
3. 保留 `CompiledPlan`、`LoweredPlan`、进入输出的 barrier sources/frontier，以及录制回调、KeepAlive、GPU completion 数据的原有所有权。`AtomicCell::readers` 本轮也保留普通 vector，不为一个嵌套成员增加自定义 allocator 传播协议。
4. 验证重复编译、声明修改后的重编译、失败后恢复，以及编译器销毁后输出仍可 lowering/录制。构建、运行相关契约测试与 Vulkan 集成测试。
5. 使用独立固定图基准分开测量编译耗时和分配；随后用相同场景与配置对比编辑器运行。报告实际收益、保留内存成本和样本限制。

### 8.2 分配与回收模型

arena 按块向上游堆分配器申请内存，容器分配在块内按对齐推进；单次 `deallocate` 不归还空间。编译器退出时，先正常析构全部容器及其元素，再由 arena 一次归还持有的内存块。失败提前返回使用同一条 RAII 析构路径。每次公共 `Compile()` 都创建独立编译器，因此不会遗留上一张图的 arena 容量。

代价是旧 vector 容量和 hash rehash 前的内存都保留到编译结束，峰值占用可能高于逐次释放。它减少的是堆分配/释放工作，不保证申请字节数更少，也不消除容器元素的析构、算法遍历或输出分配。本轮不预置大块栈缓冲，不引入跨帧保留策略；先测实际 chunk 数与峰值，再判断是否有必要调整。

不能对仍然存活的容器直接调用 arena `release()`：`clear()` 只析构元素，vector 仍可能持有指向已释放内存的 capacity。本实现只依赖成员析构顺序释放 arena，不在 `InvalidateCompile()` 或 `CompiledPlan::Clear()` 中插入 release。

### 8.3 验证与测量口径

固定图基准在编译计时之外构图；计时覆盖公共 `Compile()` 调用，也包含临时 Compiler 和 arena 的析构，但不包含 Graph/CompiledPlan 的最终析构。使用相同头文件构建的基准程序分别加载本轮前后 Render DLL，避免复用旧轮次程序引入布局差异。

分配计数单独运行，不混入计时样本。独立单线程进程临时设置 PMR 默认资源并在作用域退出时恢复，记录分配次数、申请字节、峰值和返回时未归还字节。这些数字代表 Compile 期间对被替换 PMR 默认资源的请求，本次主要来自 arena 上游，也可能包含标准库代理；不等同于全部 RDG 分配，也不能仅靠它证明每个嵌套成员均已迁移。Windows/MSVC Debug 下另记录完整 Compile 的 CRT 分配事件，包含输出容器等仍走普通堆的分配；过滤 `_CRT_BLOCK`，不覆盖绕过 CRT 的分配器。产品代码不增加统计接口或全局 allocator 切换。

完整 MSVC Debug 构建通过。9 项相关契约最终均通过；其中新增 UI lowering 回归首次运行暴露了测试 fixture 缺少 imported `SceneColor` final state，补齐声明后重跑 `RenderGraphContract` 通过，其余 8 项在同一产品二进制下已通过。新增断言先执行操作、再读取错误字符串，避免参数求值顺序造成悬空 `string_view`。

`TestRHIParallelRecordVulkan.exe --parallel` 完整默认集成测试通过，启用了 Vulkan validation，未发现 VUID 或 Validation Error。日志中的预期拒绝错误对应负向测试，相关用例均 PASS。覆盖真实跨队列读回、texture array subrange、transient alias、并行录制与 completion 回收。记录为 `build/rdg-arena-build-final.log`、`build/rdg-arena-contracts.log`、`build/rdg-arena-contract-final.log` 和 `build/rdg-arena-vulkan.log`。

### 8.4 固定图的分配与编译对照

新增可复用的手动目标 `TestRenderGraphCompilerBenchmark`，不加入 CTest。用 `cmake --build build --config Debug --target TestRenderGraphCompilerBenchmark` 构建。默认 64 个 Pass、64 个 texture 和 64 个 buffer，每 Pass 访问每类资源中的 16 个，5 次预热、80 次测量。每四个 Pass 中一个写，其余读；texture 有 4 mip、2 layer，buffer 为 4096 bytes。测试使用强绑定的虚拟 CPU 资源，计时外可执行真实 Lowering，无需 GPU。

可用 `--passes`、`--resources`、`--accesses`、`--warmup`、`--samples` 调整输入，例如小图为 `--passes 16 --resources 16 --accesses 4`。每个版本、每种规模均执行三次独立进程，顺序为前/后、后/前、前/后；每次仍包含 80 个计时样本。以下统计未使用分配 hook 进行计时。

固定 Render DLL：前为本节开头的 `41477C3B...`，后为 `DDCE23C29E4A3BA9131DE97ED842116688A68E6C07376F15BC51F87395B7C564`。两侧使用完全相同的新 benchmark EXE，仅更换同目录的 Render DLL。

| 图规模（Pass / 每类资源 / 每 Pass 每类访问） | CRT 分配请求，前→后 | CRT 申请总字节，前→后 | 新版 PMR 上游请求 | 新版 PMR 峰值保留字节 |
| --- | ---: | ---: | ---: | ---: |
| 16 / 16 / 4 | 2196 → 1662 | 195489 → 215237 | 13 | 50616 |
| 64 / 64 / 16 | 22272 → 19671 | 3421478 → 3583302 | 15 | 443184 |
| 128 / 128 / 16 | 44455 → 39135 | 6563177 → 6870323 | 15 | 880960 |

三次计数结果一致。CRT 分配请求分别减少 **24.3%、11.7%、12.0%**；申请总字节增加 **10.1%、4.7%、4.7%**，包含对齐/块容量等开销，不能把它解释为峰值堆内存。PMR 的全部上游请求在 Compile 返回前均对应释放，`live_bytes_after_compile=0`。旧版 PMR 请求数为 0，表示当时未走这个资源，不表示没有内存分配。

当前 MSVC PMR 实现的上游块在 arena 析构时统一释放，因此本次 `peak_live_bytes` 等于 PMR 申请总字节。它只衡量这个暂存资源，不包含 `CompiledPlan` 等普通堆内存。工具输出字段明确命名为 `pmr_default_resource`：如以后标准库代理或其他 PMR 容器访问默认资源，也会被计入；不能把统计值无条件当成任意平台的纯 arena chunk 数。

本轮没有测量前后版本 Compile 的完整堆峰值，不能用 CRT 累计申请字节或新版 PMR 峰值推导总内存峰值的增减。

| 图规模 | 前三次 Compile p50，ms | 后三次 Compile p50，ms | 三次 p50 的中位数，前→后 |
| --- | --- | --- | --- |
| 小 | 0.8504 / 1.0720 / 1.2484 | 1.1772 / 0.8022 / 1.1355 | 1.0720 → 1.1355 |
| 中 | 16.0252 / 14.7650 / 15.3416 | 15.5164 / 14.9097 / 13.2781 | 15.3416 → 14.9097 |
| 大 | 29.5226 / 33.1323 / 31.7132 | 28.2415 / 30.6284 / 27.7784 | 31.7132 → 28.2415 |

大图三次均变快，中位数约下降 **10.9%**；中图中位数约下降 **2.8%**，但有一次略慢；小图中位数增加 **5.9%**，且前后范围重叠、波动较大，不能报告小图稳定加速或据此精确估计回退幅度。分配数量的确定性改善没有变成所有规模上的确定性耗时改善。

前后编译 Dump、LoweredPlan Dump 的 FNV-1a 均一致。分配 probe 在 Compiler 和 arena 已销毁后继续 Dump/Lower，验证输出没有依赖暂存内存。固定输入分别产生 128/2048/4096 条 compiled access。原始 JSON 及各自 DLL/EXE 保存于 `build/rdg-arena-variants/`，执行脚本为 `build/run_rdg_arena_benchmark.ps1`；产品实现未加入计数器、测试注入参数或默认 allocator 的全局修改。本轮尚未测量 Release，不将 Debug 结果外推到 Release。

### 8.5 编辑器运行对照与结论

同一 Sponza、1920×1080 窗口、关闭 VSync/锁帧，Raster/RT 均为串行录制，使用独立配置覆盖文件。沿用 ProfileDump 的启动/关闭排除规则，逐帧汇总 Compile 和录制 scope 后取 p50。运行期间未并发构建、基准或大型 capture 解析。

| 配置与运行 | 可用采样秒数 | FPS | RenderFrame p50 ms | 每帧 Compile 合计 p50 ms | 每帧录制 scope 合计 p50 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Raster 前 1 | 30.00 | 37.95 | 24.7620 | 4.1285 | 5.1353 |
| Raster 后 1 | 20.77 | 35.79 | 26.3138 | 4.1898 | 5.4253 |
| Raster 前 2 | 30.00 | 40.94 | 22.9859 | 3.8855 | 4.5925 |
| Raster 后 2 | 30.00 | 39.48 | 23.6412 | 3.6393 | 4.9418 |
| RT 前 | 30.00 | 24.13 | 16.6206 | 2.8874 | 5.2058 |
| RT 后 | 30.00 | 23.90 | 17.5018 | 2.7199 | 5.6508 |

Raster 录制 scope 是 `RecordAndMergeFrontendCommands`，RT 是包含 lowering 的 `ExecuteFrontendRecordingPlan`，两列同版本内可比较，跨 renderer 不当作相同口径。第一组各运行 110 秒，但新版 Raster 启动较慢，只留下 20.77 秒可用样本；保留原始结果，不将这组作为完整 30 秒的正式比较。随后 Raster 前后均延长到 140 秒；RT 前 110 秒、后 140 秒，二者均得到完整 30 秒稳定窗口。

Raster 复测的 Compile p50 减少 **0.2462 ms（6.3%）**；RT 减少 **0.1675 ms（5.8%）**。两者 FPS 均没有提高，Raster 复测反而下降约 3.6%，RT 下降约 0.9%。两次旧版 Raster 的 FPS 自身相差约 7.9%，其他未修改 scope 也有明显变动，因此这些样本不足以确定整帧性能变化的归因或稳定幅度。不能把局部编译耗时下降表述为恢复帧率，更不能将上一轮不同时间测得的约 59 FPS 当作本轮对照。

本轮可以确认：编译暂存改为按生命周期统一分配和回收，完整 Compile 的 CRT 分配请求减少，较大固定图和本轮完整窗口中的 Compile 耗时下降；代价是累计申请字节增加，小图没有稳定加速，**尚未证明编辑器整帧收益**。保持该实现的理由是它用少量标准库代码表达真实暂存生命周期，并有可复现的分配及局部耗时收益；它不是当前 RDG 开销问题的完整答案。若继续优化，应继续根据阶段耗时和分配来源选择目标，不能仅为扩大 arena 覆盖率而迁移生命周期不同的输出、回调或提交数据。

六次编辑器运行均正常退出，日志未发现 error、critical、VUID 或 Validation Error。profiling source 数仍为 Raster 2、RT 3。原始配置、二进制哈希、进程信息、capture、`timings.txt` 和 `per-frame-summary.json` 在 `build/rdg-bisect-results/`：`arena-before-raster`、`arena-after-raster`、`arena-before-repeat-raster`、`arena-after-repeat-raster`、`arena-before-ray`、`arena-after-ray`。

结束时所有测试编辑器已关闭，正式 Render DLL 为上述 `DDCE23C2...` 新版。根目录及可执行目录默认配置仍为 Raster、关闭并行录制；本轮没有修改用户配置。

## 9. 第六轮：编译计划及派生数据去重

### 9.1 约束与实施计划

本轮基线是 `22182607`，Render DLL 为 `DDCE23C29E4A3BA9131DE97ED842116688A68E6C07376F15BC51F87395B7C564`。目标是消除同一结果的重复存储、计算和转换；保留一次 Kahn 拓扑排序，以及现有的声明顺序合法性检查。支持逆声明顺序的输入、改变资源版本语义或增加 GPU 异步调度均不属于本轮范围。

依赖图提供约束，拓扑排序选择合法顺序，执行层消费编译计划。这三者的职责保持独立。不能因为目前成功图的执行位置等于声明序号，就把 `pass_id` 当成执行位置；统一的位置表仍从实际 `execution_order` 构造。资源使用、队列同步和 alias 安全信息继续保留。

实施顺序及原有保证的承接者：

| 删除的重复工作 | 合并后的责任与有效期 |
| --- | --- |
| `topological_order` 与重新生成的声明顺序数组 | Kahn 直接生成唯一 `CompiledPlan::execution_order`；原声明顺序限制独立检查，成功图的行为不变 |
| 生命周期、alias 规划、alias 校验各建一份执行位置表 | 编译器暂存一份 `pass → execution_position`，各阶段共享，到 Compile 返回时回收；Lowering 的独立校验不合并进该生命周期 |
| `levels[pass] → dependency_levels 分组 → pass_to_dependency_level` 的往返 | 保留最终依赖图上计算得到的原始 level 表，直接供录制单元使用；分析用分组仍正常输出 |
| alias 阶段重新审计全部原始依赖边 | 原始边的拓扑合法性由 Kahn 保证；每条新增 alias 依赖由 frontier 校验保证句柄有效、源早于目标、实际边存在，同时保留生命周期、描述符和屏障完整性检查 |
| 最后一次边排序后回写 `edge_indices` | 排序完成后已无 `AddEdge` 消费者，省掉该回写；第一次排序后仍要修复索引，供后续 alias 插边使用 |

保留 Kahn 后，当前的 `OutgoingEdges` 仍需要按源 Pass 排序的边，因此不能直接把所有排序推迟到 alias 之后。alias 会增加真实依赖，后续的最终规范化仍然必要。依赖层级也仍按所选拓扑顺序传播，不假定声明编号就是拓扑排名。

### 9.2 验证方案

更新顺序输出的消费者，并验证确定性顺序、依赖层级、重复编译、失败恢复、alias 前沿及混合语义/alias 边。完整 Debug 构建后执行相关 RDG/RHI 契约、Vulkan 并行集成测试，以及新版本 Raster/RT 运行检查。

复用 `TestRenderGraphCompilerBenchmark` 的同一构图和计时逻辑，分别测量编译耗时与分配。由于删除 `CompiledPlan` 成员会改变 C++ 对象布局，旧版 benchmark EXE 必须与旧 Render DLL 配对，新版 EXE 与新 DLL 配对；不能像上一轮只更换 DLL。旧版 EXE 的 SHA-256 为 `B830BCAACA10E04DB235657178FC1FD3E8C6ECF0C518B985756E8D8779A60AB4`。比较相同输入下的编译/Lowering Dump 摘要及计划规模，检查语义没有变化。

本轮真实编辑器运行用于正确性验证，编译性能对照使用固定图；不据此报告 FPS 提升。

### 9.3 实现与契约验证

`BuildExecutionOrder()` 保留最小声明序号优先的 Kahn 算法，直接写唯一顺序，并在出队时填写执行位置。随后独立执行 `ValidateDeclarationOrder()`，保留原来对逆声明顺序依赖的拒绝和环优先的诊断。这是本轮当时保留的输入契约，后续第 10 节取消了该额外限制。失败图的部分编译输出不作为执行计划使用。

位置表由 compiler arena 拥有，生命周期和 alias 阶段共同读取。`BuildDependencyLevels()` 在 alias 完成后返回 PMR level 表，`Compile()` 在自身作用域内通过 `span` 交给录制单元生成函数；没有保存借用视图到输出中。首次边排序后的插边索引修复并入已有的入度扫描，最终规范化不再回写没有消费者的索引。

完整 MSVC Debug 构建通过，9 项相关 CTest 全部通过。新增公开 API 回归分别构造环和无环逆向依赖，检查重复编译维持原诊断且不录制任何回调。扩展原 transient alias 链用例，覆盖边排序位置变化、向已有语义边追加 alias reason、新增 alias 边、最终依赖层级、录制元数据和重复编译的确定性。Vulkan 并行集成测试 29 个用例全部 PASS，启用 validation，未发现 VUID 或 Validation Error；负向用例预期的拒绝日志不算运行故障。

构建及验证日志为 `build/rdg-dedup-build.log`、`build/rdg-dedup-contracts.log`、`build/rdg-dedup-vulkan.log`。本轮新 Render DLL SHA-256 为 `137A5D0C68B2EEA897E4664963EF4753FBD0FF500F5B13761E2089F5E64487FC`，新 benchmark EXE 为 `FFE3347BB9F150B3741EE6E507AF51D7F0E8CA53F0E6673BE984CF6E7E1FE945`。

### 9.4 固定图测量及限制

benchmark 源码未修改，前后各自使用匹配其头文件布局的 EXE/DLL。输入沿用上一轮三种规模，每次新建图，5 次预热、80 次样本，独立进程交替执行三组。计时包含 Compiler 和 arena 析构，不包含构图、Dump、Lowering 和 Graph 最终析构；分配计数在另一次 Compile 中测量。

| 图规模（Pass / 每类资源 / 每 Pass 每类访问） | 前三次 Compile p50，ms | 后三次 Compile p50，ms | 三次 p50 的中位数，前→后 |
| --- | --- | --- | --- |
| 16 / 16 / 4 | 0.7215 / 0.7147 / 0.7370 | 0.7330 / 0.7231 / 0.7206 | 0.7215 → 0.7231 |
| 64 / 64 / 16 | 10.3926 / 10.0788 / 10.1622 | 10.4378 / 10.5529 / 10.1464 | 10.1622 → 10.4378 |
| 128 / 128 / 16 | 20.7167 / 20.7339 / 20.7304 | 23.8491 / 23.3115 / 21.5092 | 20.7304 → 23.3115 |

初轮中位数分别增加约 0.2%、2.7%、12.4%，没有显示加速。针对大图回退，随后在本机 Ryzen 9 9950X 上将进程固定到逻辑处理器 0（实际检查子进程 affinity mask 为 1），增加到 20 次预热、200 次样本，仍按前/后、后/前、前/后交替：

| 大图固定逻辑处理器复测 | 第一次 p50 ms | 第二次 p50 ms | 第三次 p50 ms | 三次中位数 ms |
| --- | ---: | ---: | ---: | ---: |
| 前 | 26.1215 | 25.8288 | 30.4631 | 26.1215 |
| 后 | 25.6072 | 25.7083 | 26.3752 | 25.7083 |

复测没有重现初轮约 12% 的回退，三次中位数约下降 1.6%，但仍有显著运行波动，不能报告稳定的耗时改善或退化，也不能把结果归因于未经测量的调度、频率或缓存因素。两种采样条件分别比较，不能将固定处理器的绝对耗时与未固定处理器的耗时直接当作优化差值。仍未测量 Release 或本轮整帧 FPS 对照。

| 图规模 | CRT 分配请求，前→后 | CRT 累计申请字节，前→后 | PMR 默认资源请求数，前→后 | PMR 峰值保留字节，前→后 |
| --- | ---: | ---: | ---: | ---: |
| 小 | 1662 → 1661 | 215237 → 214981 | 13 → 13 | 50616 → 50616 |
| 中 | 19671 → 19669 | 3583302 → 3432982 | 15 → 14 | 443184 → 293888 |
| 大 | 39135 → 39134 | 6870323 → 6868275 | 15 → 15 | 880960 → 880960 |

分配结果在同规模的重复运行中一致，统计口径沿用 8.3 的 `pmr_default_resource`。中图的编译暂存 arena 向上游申请的峰值减少约 33.7%，其他两个规模的上游请求数和保留量未变；内部暂存需求减少，不一定会让上游块数下降。CRT 计数仍包含输出分配、排除绕过 CRT 的分配器，累计申请字节不是完整堆峰值。各次独立分配 probe 的 PMR 请求均在 Compile 返回前对应释放。

前后编译 Dump、LoweredPlan Dump 摘要和访问/边/屏障数量一致。当前固定图全部使用 import 资源，没有 alias，因此它能测重复位置表、全边复审等共同成本，不能单独量化最终 alias 排序去重的收益；该路径由专门回归和 Vulkan alias 测试验证。

本轮确定的结果是：减少重复表示和重复计算，保留单一编译顺序及原有调度契约，部分规模减少暂存内存；**尚未证明稳定的编译耗时收益**。原始 EXE/DLL 配对、元数据与 JSON 保存在 `build/rdg-dedup-variants/`，普通对照脚本为 `build/run_rdg_dedup_benchmark.ps1`，固定处理器复测文件以 `large-pinned-` 开头。

### 9.5 编辑器运行检查

完整新版本分别以 Raster、Raytracing 运行 Sponza，各运行 110 秒后正常关闭，退出码均为 0。两次日志均未发现 error、critical、VUID 或 Validation Error。使用独立配置覆盖文件和串行录制；运行期间未并发构建或基准测试。本轮没有旧版本编辑器对照，这两次运行只用于正确性检查，不作为帧率提升的依据。

配置、进程信息、二进制哈希、日志和 capture 保存在 `build/rdg-bisect-results/dedup-after-raster/` 与 `build/rdg-bisect-results/dedup-after-ray/`，汇总日志为 `build/rdg-dedup-raster.log` 与 `build/rdg-dedup-ray.log`。两次均使用 9.3 所列的新 Render DLL，测试结束后没有遗留编辑器进程。根目录和可执行目录的默认配置仍为 Raster、关闭并行录制，未修改用户配置。

## 10. 取消依赖必须沿声明顺序向前的限制

### 10.1 真实约束与清理范围

当前 `Read(R)`、`Write(R)` 没有由调用方指定资源版本。编译器按声明顺序解释访问，生成 RAW、WAR、WAW 以及状态和所有权依赖；这些边已经包含保持资源版本含义所需的先后约束。构图完成后，合法拓扑顺序可以直接作为执行顺序，无需再次要求每条边的 `src.pass_id < dst.pass_id`。

原来的 `ValidateDeclarationOrder()` 维护了一个更强的输入限制：即使显式逆向依赖没有与任何资源语义冲突，也会拒绝该图。本轮取消这一限制；依赖环仍由 Kahn 拒绝，新增 alias 边仍必须沿选定执行顺序向前。资源版本分析、状态与所有权同步、生命周期和 alias 检查保持各自职责。

这不改变资源访问声明的含义。先声明 transient reader、后声明 producer，仍会因为读取未初始化内容而失败；逆向显式依赖也不能要求某次读取改读另一个版本。如果显式依赖与已有资源依赖形成环，编译仍失败。多个就绪 Pass 继续优先选择较小声明序号，保持无额外约束时的确定性。

串行录制回调跟随编译顺序，`Serial` 表达等待前组、在调用线程执行、再放行后组；GPU 依赖不成为 CPU 录制任务之间的数据传递协议。沿用现有 Prepare/Record 约束，不新增 CPU 调度层。

### 10.2 实施与验证计划

1. 删除声明顺序检查的声明、定义及调用，执行顺序继续由 Kahn 直接生成。
2. 将无环逆向依赖的拒绝用例改为正向验证，覆盖重复编译、Lowering 和串行录制顺序；检查逆序兼容读者的 export 边界，以及逆序 transient 资源的同槽复用。保留真实环、未初始化 transient 读取及稳定顺序用例。
3. 核查队列批次、屏障、生命周期及 alias 使用实际执行位置，修正仍把声明顺序当作普遍执行契约的描述。
4. 完整 Debug 构建，运行 RDG/RHI 契约、Vulkan 集成测试及 Raster/RT 编辑器检查，验证后独立提交。

本轮移除一次全边扫描并扩大合法输入范围，不引入新的调度启发式或 GPU 异步执行策略，不将这项清理表述为已经取得帧率收益。

### 10.3 保证的承接与回归覆盖

顺序合法性由同一套依赖边和 Kahn 维护。队列批次从 `execution_order` 构建，生命周期和 alias 读取执行位置；Lowering 的 ownership release 选择实际最后的源队列批次，export 屏障选择实际最后的来源位置。`CompiledBarrier::src_pass` 是代表源，不承诺执行最晚，注释已相应修正；完整 `sources` 仍是同步依据，无需为了改变代表源而增加新的排序或扫描。

新增正向用例构造声明顺序 A、B、C，其中 A、C 兼容地读取 X，B 写入 Y，C 读取 Y，A 显式依赖 C。编译顺序成为 B、C、A，验证 Y 的版本关系和跨队列所有权同步、X/Y 生命周期、Lowering 和串行回调顺序；X 的 export 屏障必须放在实际最后的 A 之后，即使 A 的 Pass ID 最小。重复 Compile/Lower 的输出保持一致。

另一个用例让两个相同描述符的 transient buffer 按逆声明顺序写入，验证生命周期及 alias 方向沿实际执行顺序、原显式边正确追加 alias 原因、只申请一个物理 buffer，并把 alias 屏障放在真正的后继之前。负向用例继续拒绝纯显式环，以及显式逆向边与资源 RAW 边构成的环，失败后不会录制回调。

### 10.4 验证结果

完整 MSVC Debug 构建通过，9 项相关 CTest 全部通过，新增逆序 Lowering 和 alias 用例均通过。`TestRHIParallelRecordVulkan.exe --parallel` 的 29 个用例全部 PASS，启用 validation，未发现 VUID 或 Validation Error。日志为 `build/rdg-order-build.log`、`build/rdg-order-contracts.log` 与 `build/rdg-order-vulkan.log`。

Raster、Raytracing 分别以独立配置运行 Sponza 110 秒，均正常退出，日志未发现 error、critical、VUID 或 Validation Error。运行记录位于 `build/rdg-bisect-results/order-after-raster/`、`build/rdg-bisect-results/order-after-ray/`，汇总日志为 `build/rdg-order-raster.log`、`build/rdg-order-ray.log`。这些运行只验证正确性，没有进行本轮前后性能对照。测试编辑器均已关闭，根目录和可执行目录的默认配置仍为 Raster、关闭并行录制。

## 11. Raster 编译热点定位与优化

### 11.1 三次编译的来源与优先级

`order-after-raster` 的 1292 个完整稳定帧各包含三次 Compile，分别是 `RasterFrame`（50 个 Pass）、`UI.FrameTail`（1 个 Pass）与嵌在 UI 录制回调中的 `UI.PrivateBuffers`（2 个 Pass）。根据调用位置和 scope 嵌套映射，三者编译 p50 分别为 3.0076、0.1254、0.1434 ms。主图占累计编译时间约 91.7%。这不是主图被重复编译三遍，也不对应三次 GPU 提交；正常路径的命令最终合并进同一个 Raster command list。

主图负责场景和后处理，帧尾图声明 UI 图像访问，私有 buffer 图声明顶点、索引和参数上传及读取。UI backend 到录制阶段才取得/扩容 upload-ring buffer，因此当前封装选择了独立小图；Vulkan 不要求这样的图边界。合并需要改变准备时机、资源声明和 slot 生命周期，本轮优先定位主图，不调整这些边界。多 viewport、资源上传或启用 CUDA 会改变图数量，不能把三次当成固定架构要求。

### 11.2 计划与测量口径

1. 复用已有 ProfileDump，补充图名和 compiler 阶段计时，不创建新的统计框架；保留公共 Compile 的原有计时。
2. 在固定 Sponza、Debug、Raster 串行配置下采样主图，区分规范化、区间构造、依赖/屏障生成、排序、生命周期、alias 和队列计划。
3. 根据实际热点选择局部优化，保持资源版本、依赖、输出顺序和同步架构；相同计时配置下对比前后结果。
4. 验证契约和真实 Vulkan 行为，将收益和测量限制落入本节，再提交。

Compile 仍包含 setup 等待及 compiler 析构；阶段计时覆盖各自函数，不把函数外的初始化/析构差额冒充某个算法耗时。性能对照双方使用相同新增 scopes，避免把增加计时本身与优化混在一起。

### 11.3 实测热点与本轮改动

新增计时后的 `raster-stages-before` 稳定窗口中，主图 Compile p50 为 2.5542 ms。`BuildSemanticDependencies` 为 1.0004 ms，`BuildAtomicCells` 为 0.3301 ms，`BuildFinalBarriers` 为 0.2756 ms，包含边排序的 `BuildExecutionOrder` 为 0.1363 ms。主热点是访问依赖和屏障来源分析，不能把整个编译成本归给拓扑排序。

同一 Pass 对同一区间的访问在进入分析前已合并为一份 `CellUsage`，每个 Pass/cell 只处理一次。写访问清空 readers 后建立 writer，只读访问追加新的 Pass 身份。因此 reader 身份不重复，writer 和 readers 可以同时存在，但身份不会重叠。原实现仍在追加 reader 时线性查重，并在收集屏障来源时多次加入 writer/readers 再线性去重。

本轮直接追加已唯一的 reader，writer 只加入一次，写访问收集 readers 一次；只读状态/所有权转换仍在原来的内存依赖判定之后收集 readers，保留该判定的语义。Token 不再收集不会生成物理屏障的临时来源列表，但 RAW/WAR/WAW 边照常产生。owner-family 过滤和空前沿补充保持原有顺序。不增加缓存、状态字段或新的图边界。

完整 Debug 构建、9 项 RDG/RHI 契约和 29 项真实 Vulkan 用例均通过，未发现 Vulkan validation 错误。对应日志为 `build/rdg-raster-frontier-build.log`、`build/rdg-raster-frontier-contracts.log`、`build/rdg-raster-frontier-vulkan.log`。

### 11.4 同口径前后对照

两次 Raster 均运行 110 秒，以最后的稳定 30 秒窗口统计，保持 Sponza、场景视口 1500×1002、1920×1080 输出、串行录制和相同新增 CPU scopes。前后 Render DLL SHA-256 分别为 `1A9A766E3F9A18C87A6EC0CA50194B9B2611033621209037C7F7C9BE4811E3D1`、`D986843622C9FFE5A35AD91EFDA2789BB3365E010D9F5B86BDEAFCFA32E5A337`。

| 指标 | 前 | 后 |
| --- | ---: | ---: |
| FPS | 56.74 | 57.33 |
| 主图 Compile p50 ms | 2.5542 | 2.4706 |
| 依赖来源分析 p50 ms | 1.0004 | 0.9374 |
| 区间构造 p50 ms | 0.3301 | 0.3254 |
| 最终屏障生成 p50 ms | 0.2756 | 0.2681 |
| 执行顺序生成（含边排序）p50 ms | 0.1363 | 0.1353 |

本组主图 Compile 减少约 0.084 ms（3.3%），目标阶段减少约 6.3%；FPS 仅增加约 1.0%，单组前后对照不足以证明稳定整帧收益。本次基线本来就高于先前夜间采样的约 43 FPS，不能把不同时间运行的差额归到这次优化。两次正常退出且无运行错误；原始配置、进程、哈希、capture 和 `compiler-stages.json` 位于 `build/rdg-bisect-results/raster-stages-before/`、`raster-stages-after/`，分析脚本为 `build/analyze_raster_compiler_stages.py`。

固定图基准沿用未修改的构图、5 次预热和 80 个计时样本，每种规模做三组独立进程，执行顺序前/后、后/前、前/后。两侧均使用相同 profiling 代码，基准未启动 CPU capture；分配 probe 与计时分开。以下为三次 p50 的中位数：

| 图规模（Pass / 每类资源 / 每 Pass 每类访问） | Compile p50，前→后 ms | CRT 分配请求，前→后 |
| --- | ---: | ---: |
| 16 / 16 / 4 | 0.7189 → 0.6793 | 1661 → 1643 |
| 64 / 64 / 16 | 10.0882 → 9.0547 | 19669 → 18829 |
| 128 / 128 / 16 | 20.4280 → 18.2497 | 39134 → 37568 |

三个规模的三次运行都变快，中位数分别减少约 5.5%、10.2%、10.7%。编译/Lowering Dump 摘要与访问、边、屏障数量全部一致；PMR 返回后 live bytes 仍为 0，上游块数和保留量未变。分配请求减少来自来源集合更少的增容，不能据此推导全 RDG 内存峰值变化。原始配对二进制、全部 JSON 与哈希汇总在 `build/rdg-raster-compiler-variants/`，执行脚本为 `build/run_rdg_raster_frontier_benchmark.ps1`。

本轮证明了局部编译工作减少及固定图的耗时改善，尚未解决 Raster 整帧开销，也未测量 Release。根目录和可执行目录默认配置保持 Raster、关闭并行录制，测试编辑器均已关闭。

## 12. Raster 帧内开销覆盖与区间容量优化

### 12.1 分步计划与约束

上一轮稳定窗口中，RenderFrame p50 为 16.6681 ms；Compile 与录制入口按时间区间去重后为 5.8638 ms，其外仍有 10.7932 ms。各项中位数不能直接相加。尤其 merged recording 的 `Validate()` 在原录制 scope 之前执行，Lowering 和物理屏障转换都没有计入，剩余时间也包含场景更新、提交及等待，不能全部归因于 RDG 编译。

1. 补齐粗粒度计时并独立提交：图导入与 Pass 声明、主图生命周期、Lowering、merged recording 准备及批量屏障转换、场景更新、帧尾录制、提交请求和提交接受等待。复用 ProfileDump，不在每个 cell 或 barrier 上增加事件。
2. 在相同计时配置下，为已知完整区间数量的 cell 容器预留精确容量。用前后配对的固定图和 Raster 运行检查验证性能、输出和内存分配，独立提交。
3. 依据新数据选择下一热点。只有访问扫描确实值得优化时才改区间查找；跨帧缓存、UI 图边界及 GPU 调度架构仍需单独讨论。

`Lower` scope 包含函数内临时数据析构；`PrepareMergedRecording` 包含 Lower 和 Materialize，不能相加。`GraphLifetime` 包含构图、编译、准备、录制与图析构，用子区间并集计算残余，不能把残余全部视为析构。`SubmitFrame` 覆盖封装命令、创建 present 请求和交给 executor，不等于 native queue submit；`WaitFrameSubmitted` 等待 native 提交接受结果，不表示 GPU 完成。各阶段保留原有顺序、资源生命周期和同步保证。

### 12.2 补齐计时后的基线

`raster-frame-scopes-before` 运行 110 秒，使用同样的 Debug、Sponza、串行录制配置。最后稳定 30 秒共有 1685 个完整帧，FPS 为 56.19，RenderFrame p50 为 17.0343 ms。只对 RenderFrame 所在线程归因；嵌套 UI 图按时间区间并集扣除重复，逐帧独占阶段之和严格等于该帧时长。

| 独占阶段 | 每帧 p50 ms |
| --- | ---: |
| 主 Pass 声明与准备 | 4.9382 |
| 所有图 Compile | 3.0635 |
| 前端录制与合并，扣除嵌套图编译和准备 | 2.7990 |
| 所有图 Lowering | 1.6865 |
| 提交接受等待 | 1.4922 |
| GPU profiling Seal/Drain | 1.2340 |
| 主图生命周期内其余工作 | 0.5102 |
| 后处理 Pass 声明与准备 | 0.2958 |
| 主图资源导入 | 0.2507 |
| 屏障物化 | 0.1253 |
| 主线程提交封装与入队 | 0.0701 |

表中为分别计算的中位数，不能相加。稳定窗口未发生 SceneUpdate。其余准备检查、帧尾包装和帧内未覆盖部分分别约 0.0613、0.1304、0.3127 ms。由此可见，不能把原来未覆盖的时间全部归为 compiler；`AddToGraph` 包含 Pass 业务准备，后续应进一步定位该阶段。profile Seal/Drain 和提交等待也不能算作 RDG 算法成本。

完整 Debug 构建、9 项 RDG/RHI 契约与 Raster 运行通过，正常退出，未发现 error、critical、VUID 或 Validation Error。二进制 SHA-256 为 `4AA289BCFA93E133DB56A152E665B551A0DFACC1FEBF180D8F54EEB9D6D9D991`。日志为 `build/rdg-frame-scopes-build.log`、`build/rdg-frame-scopes-contracts.log`、`build/rdg-frame-scopes-raster.log`，原始 capture、配置和进程信息位于 `build/rdg-bisect-results/raster-frame-scopes-before/`，分析脚本为 `build/analyze_raster_frame_stages.py`。本步仅改计时边界，不声称性能改善。

### 12.3 精确容量预留

区间端点去重后，Buffer 的 cell 数恰好为端点数减一；Texture 的 cell 数恰好为有效 aspect 数、mip 区间数与 layer 区间数的乘积。当前生成循环会完整构建这个网格，因此可以直接 reserve，无需估计访问密度或新增缓存。有效 descriptor 已保证 aspect 位合法，数量直接用 `std::popcount` 计算；乘积用 `size_t`，先用 `max_size()` 除法检查避免溢出。一对一规范化的初始/最终状态声明也按输入数量 reserve。

该调整避免 vector 增长时搬迁 cell。monotonic arena 不能提前回收扩容前的旧存储，精确预留也减少这部分暂存需求。容器仍由单次 Compiler 拥有，输出仍独立持有内存；cell 顺序、初始化、版本、来源和依赖规则不变。

原固定图只访问 Whole，每个资源仅一个 cell，无法有效检验本次优化。基准增加可选 `--subranges`：从现有 4 mip × 2 layer 中选择一个子资源，从 4096 字节 Buffer 中选择一个 512 字节区间，保留资源数、访问条数和读写周期。默认输入不变；新模式在原三种规模中让每个资源形成 8 个 cell。前后使用同一新版基准 EXE，分别配对旧/新 Render DLL，比较耗时、分配和两个 Dump 摘要。

### 12.4 Raster 对照及测量限制

最终版本 `raster-cell-reserve-final` 同样运行 110 秒，正常退出且无运行/validation 错误。与 12.2 的相同 profiling 配置基线比较：

| 指标 | 前 | 最终版本 |
| --- | ---: | ---: |
| 稳定窗口 FPS | 56.19 | 55.78 |
| RenderFrame p50 ms | 17.0343 | 17.1675 |
| 主图 Compile p50 ms | 2.5436 | 2.5221 |
| 全部图 Compile，每帧 p50 ms | 3.0635 | 3.0599 |
| BuildAtomicCells p50 ms | 0.3320 | 0.3222 |
| 主 Pass 声明与准备 p50 ms | 4.9382 | 5.0397 |

区间构造约减少 0.010 ms；整帧和全部编译时间没有显示稳定收益。不能将固定多区间图的收益比例外推到 Raster 帧率。两边都是 MSVC Debug 并启用 capture，尚未测量 Release 或关闭 profiling 的帧率。

在最终版本之前，使用逐项 aspect 计数的中间实现也采样了一次（`raster-frame-scopes-after`，DLL `7C4860FE769717073C9BA2F6592F6B922DAF96CC0FF912371DE8AEC323AD889D`）。该窗口 FPS 为 52.64，主图 Compile p50 为 2.4929 ms，却有一次 2044.4249 ms 的公共 Compile 停顿，发生在 compiler 函数计时之外。公共入口还包含 setup 派发/等待及外围工作，当前 scope 不能确定具体原因，不能归因于 reserve 或作为其加速证据。保留原始样本，不用挑选后续窗口替代它；最终版本另用 `std::popcount` 简化 aspect 数量计算，并重新运行全部对照。

最终 DLL SHA-256 为 `83FCA96E2BCC76F1689778426E81965E3E6DF5A6F2B1FC789C072F2AF3FB4C06`。运行日志为 `build/rdg-cell-reserve-final-raster.log`，最终 capture、配置、进程信息及分析结果在 `build/rdg-bisect-results/raster-cell-reserve-final/`。根目录配置仍为 Raster、关闭并行录制。

### 12.5 固定图结果与验证

每种模式、每种规模运行三组独立进程，顺序为前/后、后/前、前/后；5 次预热、80 次计时，计时与分配 probe 分开。运行期间无编辑器、构建或其他测试。配对 EXE SHA-256 均为 `83ACC19E07EBEEA36309D9DE1722DDB93B88C48FC78663148FE20144C733F76C`；旧/新 DLL 分别对应 12.2 和 12.4 的最终哈希。表中耗时为三次 p50 的中位数，规模依次为 16/16/4、64/64/16、128/128/16（Pass / 每类资源 / 每 Pass 每类访问）。

| 模式与规模 | Compile p50，前→后 ms | 变化 | PMR 峰值保留字节，前→后 | CRT 分配请求，前→后 |
| --- | ---: | ---: | ---: | ---: |
| Whole 小 | 0.6889 → 0.6914 | +0.4% | 50616 → 50616 | 1643 → 1643 |
| Whole 中 | 9.0239 → 9.1249 | +1.1% | 293888 → 293888 | 18829 → 18829 |
| Whole 大 | 18.2240 → 18.4668 | +1.3% | 880960 → 880960 | 37568 → 37568 |
| Subranges 小 | 1.2363 → 1.1258 | −8.9% | 172904 → 76360 | 3985 → 3471 |
| Subranges 中 | 8.4086 → 8.0278 | −4.5% | 667128 → 443184 | 21152 → 19103 |
| Subranges 大 | 16.9652 → 16.1365 | −4.9% | 1326352 → 880960 | 43702 → 39605 |

多区间模式的九组配对全部变快，PMR 峰值保留量约减少 56%、34%、34%，上游请求数分别从 16 降为 14、15、15。Whole 模式不减少分配，耗时有轻微增加，因此不将本次调整描述为所有输入上的普遍加速。Debug 的 CRT 结果还包含容器调试设施，不能直接外推 Release 收益。PMR 计数口径沿用前文，它不是全部 RDG 堆峰值。

全部 36 次运行的同输入 Compile/Lowering Dump 摘要、访问/边/屏障数量一致，每次 PMR live bytes 在 Compile 返回后均为 0。最终完整 Debug 构建、9 项 RDG/RHI 契约和 29 项真实 Vulkan 用例全部通过，Vulkan validation 错误为 0；未添加只检验 capacity 的实现镜像测试。日志为 `build/rdg-cell-reserve-final-build.log`、`build/rdg-cell-reserve-final-contracts.log`、`build/rdg-cell-reserve-final-vulkan.log`。

前后配对二进制、逐次 JSON 和 `summary.json` 保存在 `build/rdg-cell-reserve-variants/`；执行及分析脚本为 `build/run_rdg_cell_reserve_benchmark.ps1`、`build/analyze_rdg_cell_reserve_benchmark.py`。中间逐项计数版本的结果另保留在 `count-if-after/`，不与最终版本合并统计。

### 12.6 下一热点

当前数据将下一优先级指向主 Pass 的同步准备，而非继续调整拓扑排序或直接引入区间索引。`ShadowDepthPass`、`ProbeUpdatePass`、`GeometryPass` 的 `AddToGraph` 都调用业务 `Prepare`，应先分别计量 Prepare 和声明注册，再判断工作能否复用。只读检查还发现同步 setup callback 中的 vector 值捕获，以及同一图内重复导入 Scene 读取集合，可作为局部候选；实际 Pass 的资源读取声明与录制时的强引用所有权仍须保留。

本轮完成计时覆盖和精确容量预留两个独立步骤，未调整图边界、跨帧生命周期或队列调度架构。约 2 秒公共 Compile 异常仍需单独定位 setup 和外围等待，不能通过删除等待来掩盖。

## 13. 主 Pass 准备与 setup 等待定位

### 13.1 计划与测量边界

主 Pass 声明阶段约 5 ms，但它包含同步业务准备，不能直接当作 RDG 注册成本。本轮先为各 Raster `AddToGraph`、Shadow/Probe/Geometry 的同步 Prepare、公共 Culling Prepare、CSM 准备及 Scene 资源导入补充粗粒度 scope。父子 scope 按区间并集扣除重复，分别报告 Prepare、资源导入和声明注册残余；残余也可能包含参数所有权构造/销毁，不能假定全是图算法。

公共 Compile 另外区分 setup 派发、等待、各 worker 的任务执行及 batch 完成。worker 时间不能加到 render thread 的墙钟耗时上；如果长停顿未重现，只报告观察到的分布和审计结果，不推断原因。等待仍保证所有准备结果成功发布后才能编译/录制，保持原有错误传播与所有权。

完成同配置基线后独立提交计时与记录，再根据热点选择局部优化。保留资源版本、逐 Pass 访问声明、录制强引用和已有同步要求；不引入跨帧缓存或调整队列架构。

### 13.2 细分基线

`raster-prepare-before` 使用相同 Debug、Sponza、串行录制配置运行 110 秒，最后 30 秒有 1644 个完整帧，54.83 FPS，主 Pass 声明 p50 约 5.0523 ms。各项按同线程完整帧归因：

| Pass | AddToGraph p50 ms | 同步 Prepare p50 ms | Scene 导入 p50 ms | 其余注册/包装 p50 ms |
| --- | ---: | ---: | ---: | ---: |
| ShadowDepth | 2.0362 | 1.9679 | 0 | 0.0688 |
| ProbeUpdate | 2.0408 | 1.0111 | 0.6931 | 0.3335 |
| Geometry | 0.4869 | 0.0281 | 0.2505 | 0.2094 |

表中分别取中位数，不能直接相加。Shadow 的 Prepare 几乎都在 CSM（p50 1.9436 ms）；稳定窗口 shadow cache 命中、没有 Shadow draw 的 Scene 导入，不能按每帧四个 shadow draw 估算注册工作。Probe 与 Geometry 仍各自导入同一 Scene 读取集合。约 5 ms 中有约 3 ms 来自同步业务 Prepare，说明整个 DeclareMainPasses 不应都算作 RDG 构图算法。

setup 派发每帧 p50 0.1138 ms，等待 p50 0.1861 ms、p95 0.3156 ms。主图析构还会再调用 Wait，分析按帧汇总，不能直接使用混合两次调用的单次中位数。最长稳定窗口 Compile 为 11.9360 ms：Wait 9.4109 ms，同窗口 `UploadLightingData.Prepare` 9.4084 ms，compiler 函数 2.2912 ms。整个 capture 最长 Compile 11.9744 ms，本轮未重现原先约 2 秒异常；只能说本轮最长等待与 lighting prepare 重叠，不能据此证明上轮异常同因。

只读审计未发现 setup/TaskGraph 的固定 2 秒超时。Wait 的真实条件是全部 job 终结、completion task 发布结果。另一个测量限制是 `ScopedCpuProfile` 先采结束时间再 Emit：Emit 的锁/分配/调度延迟会进入父 scope，却不在该子 scope 内。分析保存 Wait.end 到 Compiler.begin、最后 Job.end 到 BatchComplete.begin 的间隙，避免把所有间隙都算作业务或排队成本。

完整 Debug 构建、9 项 RDG/RHI 契约和 Raster 运行通过，正常退出，无 error、critical、VUID 或 Validation Error。DLL SHA-256 为 `AC659DD08E72DB32A721F89442C3BE383AA462F36BB8F010063FC5E7BA820651`。日志为 `build/rdg-prepare-scopes-build.log`、`build/rdg-prepare-scopes-contracts.log`、`build/rdg-prepare-before-raster.log`；原始记录和 `raster-prepare-hotspots.json` 在 `build/rdg-bisect-results/raster-prepare-before/`，分析脚本为 `build/analyze_raster_prepare_hotspots.py`。

### 13.3 Probe 占用饱和后的重复工作

Probe 准备中的 `ProbeGeometryClassifier::Analyze` 需要两个不同结果：准确的相交 primitive 数，以及小网格的联合占用 mask。原实现即使 64 个 voxel 全部已占用，仍对后续每个相交 primitive 执行 voxel 坐标转换和三重置位循环。Sponza 的密集 cell 能达到全占用，因此这部分工作不再改变结果。

本步在原相交判断及计数之后检查满 mask，满时只跳过坐标转换和置位。继续遍历全部 primitive，保留 invalid/outside 过滤、padding、相交数量、占用率和 subdivision 判断，不提前结束整个分类。满 mask 由现有 `MaxValue<uint64_t>` 推导，支持 1～64 个 voxel，避免左移 64 位；不增加缓存或状态生命周期。

回归在网格先被填满后继续加入相交、不相交、无效及仅 padding 后相交的 bounds，并将完整覆盖 primitive 分别放在首尾，检查准确计数、占据率及分类。原有空、稀疏、密集和 padding 用例保留。使用相同细分计时配置比较修改前后的 Probe Prepare 与完整 Raster 帧，再独立提交。

### 13.4 Probe 优化结果

同配置 `raster-probe-occupancy-after` 运行 110 秒、正常退出，无运行或 validation 错误。稳定窗口中的 Probe Prepare p50 从 **1.0111 降至 0.5424 ms**，约减少 46.4%；Probe AddToGraph 从 2.0408 降至 1.5539 ms；主 Pass 声明从 5.0523 降至 4.5227 ms。前后实际 cell 日志均为 2071/2330 个相交 primitive、64/64 个 occupied voxel、occupancy=1、desired_level=0。

RenderFrame p50 从 17.3626 降至 16.6707 ms，FPS 从 54.83 到 57.19。本组观察到约 4.3% 的 FPS 增加，但只有一组整帧对照，不能把全部差额归给此改动；目标阶段约 0.47 ms 的减少更直接。Shadow Prepare 基本不变（1.9679→1.9543 ms），lighting setup 仍有约 9 ms 的长尾，未宣称停顿问题已解决。

完整 Debug 构建及六个 Probe 测试程序通过：GeometryClassifier（含新增语义回归）、AdaptiveLayout、PhysicalAllocator、DirtyTracker、Clipmap、Streaming。这些程序当前未注册为 CTest，直接运行对应 EXE 并检查退出码。日志为 `build/rdg-probe-occupancy-build.log`、`build/rdg-occupancy-TestProbe*.exe.log`、`build/rdg-probe-occupancy-raster.log`。

新 DLL SHA-256 为 `4A8C4F48D56EA11D5386FF46A90BF1F5632D0AEA51F8427A2B2403B3A9696826`；前后 EXE/DLL 快照在 `build/rdg-probe-occupancy-variants/`，新 capture 与分析在 `build/rdg-bisect-results/raster-probe-occupancy-after/`。本次收益来自业务准备中删去不会改变结果的工作，没有改变 RDG 图、缓存失效或资源生命周期。

### 13.5 将诊断格式化放到节流判定之后

`UploadLightingData.Prepare` 的常见耗时仍约 0.24 ms。审计发现 Probe GI 开启时每帧会构造约 80 个字段的 `ostringstream`，再交给“两秒一次”的日志函数。未输出的帧同样支付统计读取、数值转换和字符串分配；Release 虽关闭 `LOG_DEBUG`，原调用方的字符串构造仍会执行。

将已有 helper 改为 `ShouldLogDebugEverySeconds` 判定，并在调用方构造日志之前使用它。编译期关闭 Debug 时直接不构造；运行时同时考虑 logger level 和 backtrace，保留 backtrace 捕获被 level 过滤消息的能力。真实的 `FillLightingData`、矩阵、灯光和 shader 参数赋值不受判定控制。现有 profiling 日志调用点同步适配，不引入通用日志框架。

沿用原有按调用站点保存的 timer map，不增加状态或同步机制；首次插入也执行 Tick，修复原 `inserted || Tick()` 短路造成的首次连续输出两次。该调整只消除不需要输出时的格式化；到期后的真实日志输出仍可能阻塞，不能据此宣称已解释或解决旧的 2 秒停顿。

### 13.6 日志优化结果与长尾边界

首次修改后运行 `raster-lighting-log-after` 110 秒，启动较慢，按既有规则得到的可用窗口只有 27.6759 秒、1105 个完整帧。FPS 为 39.97，RenderFrame p50 为 22.8158 ms；未修改的 Shadow Prepare 也从上一轮 1.9543 增至 2.2915 ms。lighting job 的 p50 降到 0.0659 ms、p95 为 0.1111 ms，但存在 322.5410 ms 的峰值，对应公共 Compile 326.5496 ms、setup Wait 322.5271 ms。保留该样本，不能只报告中位数或用后续较快窗口替代。

随后恢复保存的旧 EXE/DLL，再切换新 EXE/DLL，分别运行 140 秒。两侧使用相同 Sponza、Debug、串行录制和 CPU capture，稳定窗口各 30 秒，完整帧分别为 1127、1420。运行期间未并发执行构建、测试或 capture 分析；未暂停用户的其他应用。前后统计如下，worker job 与 render thread 等待分别统计，不能相加：

| 指标 | 旧日志实现 | 延后格式化 |
| --- | ---: | ---: |
| FPS | 37.62 | 47.36 |
| RenderFrame p50 ms | 25.3777 | 19.2441 |
| lighting job p50 ms | 0.2807 | 0.0569 |
| lighting job p95 ms | 0.4465 | 0.0849 |
| lighting job 平均 ms | 0.3158 | 0.0867 |
| lighting job 最大 ms | 9.6097 | 9.6346 |
| setup Wait 每帧 p50 ms | 0.1523 | 0.0791 |
| setup Wait 每帧 p95 ms | 0.3204 | 0.1697 |
| setup Wait 每帧最大 ms | 9.5644 | 10.5119 |
| Shadow Prepare p50 ms（未改动） | 2.5996 | 2.0431 |

两次新版本采样中，lighting job 的常见耗时都降至约 0.06 ms；复测目标阶段 p50 减少约 0.224 ms。但保存的同一旧二进制也从先前 57.19 FPS 降至 37.62 FPS，且复测中未修改的阶段同时变快，说明整帧测量存在明显运行间波动。不能把 37.62→47.36 FPS 全部归给日志优化，也不能将第一次新版本的低帧率单独归因于该改动；当前还没有稳定的整帧收益估计。

复测新版本的最长公共 Compile 为 14.9317 ms，其中 Wait 10.5106 ms；最后一个 setup job scope 结束到 batch completion scope 开始之间有 10.5228 ms 的间隙。这是进一步定位 completion 路径的依据，但 job scope 的结束早于 profiler Emit 和任务最终完成，不能直接认定为 TaskGraph 唤醒故障。首次新版本约 323 ms 的 lighting job 峰值与旧的约 2 秒异常均未解释，延后格式化并未解决全部长尾。

首次新版本的诊断日志间隔最短 2.000 秒；复测新版本首个间隔 2.009 秒，后续为 2.000～2.029 秒。旧版本复测的前两条仅相隔 0.047 秒，确认首次重复输出已消除。所有实际光照数据准备仍逐帧执行。

完整 Debug 构建通过；三次日志对照运行均正常退出，无运行或 validation 错误。最终二进制另通过 29 项真实 Vulkan 用例，0 FAIL、0 VUID、0 Validation Error。本轮前两步的 9 项 RDG/RHI 契约和六个 Probe 程序也已通过。日志为 `build/rdg-lighting-log-build.log`、`build/rdg-lighting-log-raster.log`、`build/rdg-lighting-log-repeat.log`、`build/rdg-prepare-final-vulkan.log`。

新 Render DLL SHA-256 为 `C47BFB1047AABF0526466AD10FFE55CED30763B2F63F6697015B669EC6D8C41F`，旧 DLL 为 13.4 的 `4A8C4F48...`。配对 EXE/DLL 保存在 `build/rdg-lighting-log-variants/`；执行脚本为 `build/run_rdg_lighting_log_pair.ps1`。原始 capture、配置、进程及 `raster-prepare-hotspots.json` 分别位于 `build/rdg-bisect-results/raster-lighting-log-after/`、`raster-lighting-log-repeat-before/`、`raster-lighting-log-repeat-after/`。对照结束后已恢复最终新二进制，测试编辑器均关闭，默认配置仍为 Raster、串行录制。

### 13.7 后续热点

当前还值得优化的稳定工作是约 2 ms 的 CSM 准备、Probe/Geometry 合计约 0.94 ms 的重复 Scene 导入，以及上一轮约 1.69 ms 的全部图 Lowering。CSM 可先检查同一 Prepare 内跨 cascade 重复的包围盒变换；Scene 导入可检查同一图内的句柄复用，仍需保留各 Pass 的读取声明和录制所有权。同步声明 callback 的值捕获也可继续审查。这些是候选方向，尚未实现或证明收益。

长尾则需要继续区分 lighting job 内部、profiler Emit 和 batch completion 发布/唤醒，不能通过删除必要的等待处理。本轮三个步骤只补充计时和删去不会改变结果的业务准备工作，未改变 RDG 依赖、图边界、资源生命周期或队列架构。

## 14. 关闭 Probe GI 后重新比较 RDG 接管前后

### 14.1 对照计划与口径

用户先要求默认关闭 Probe GI（`d0fed580`），随后要求重新比较 RDG 接管前后的耗时。本轮比较 Raster 的直接父提交 `e971a229`、完整接管提交 `15e64157` 和当前优化后版本 `d0fed580`。历史两版仅在独立 worktree 将 Probe GI 默认值改为 false；当前主工作区不回退，历史二进制使用独立输出目录。

三个版本均使用 MSVC Debug、Sponza、1920×1080 输出、固定 ImGui 布局、串行录制、关闭垂直同步与帧率锁、Vulkan validation 和 ProfileDump。旧版 `render_graph = false` 选择原 linear 路径；完整接管后该字段不再关闭 RDG。保持各版本自己的匹配二进制与资源，不混用 Render DLL ABI。每次预热后取完整的稳定 30 秒；旧路径与当前版本重复运行，以暴露运行间波动。构建、其他测试和大 capture 解析不与采样并发。

对照先比较相同的 RenderFrame 和帧间隔，再对当前版本按同线程区间并集拆分构图、编译、Lowering、录制、提交与等待。历史版本没有的细分 scope 标为不可测，不能记成零。当前版本有更多诊断 scope，其测量开销也可能影响跨版本差值；本轮结果属于开启 capture 的 Debug 配置，不能外推 Release 或断言全部差额都是 RDG 算法本身。接管前后的相邻提交用于识别原始回退；当前与旧版之间还包含其他重构。

为估计细分 CPU capture 的影响，在两次当前完整计时运行之间增加一组诊断：临时让 CPU producer 仅记录 `Raster.RenderFrame` 和 `Raster.PrepareFrame`，与旧路径的两个 CPU scope 一致，GPU profiling 保持原样。只重建 Core，不修改 RDG 或渲染算法；运行后恢复原始源码、重建，再恢复保存的匹配二进制。未激活的 scope 对象构造和快速返回仍有成本，该诊断不是完全关闭 profiling 的测量，运行间波动也不能忽略。

### 14.2 接管前、接管初版与当前结果

第一轮旧版运行 130 秒，其余每次 140 秒，全部取得完整稳定 30 秒。实际顺序如下，帧间隔根据相邻完整 RenderFrame 的开始时间计算，RenderFrame 列为 CPU scope p50：

| 运行目录 | 版本 / CPU capture | FPS | 帧间隔均值 ms | RenderFrame p50 ms |
| --- | --- | ---: | ---: | ---: |
| `takeover-legacy-1` | `e971a229` / 原有帧级计时 | 110.46 | 9.0533 | 8.2580 |
| `takeover-initial-1` | `15e64157` / 原有计时 | 29.99 | 33.3463 | 31.7998 |
| `takeover-current-1` | `d0fed580` / 完整计时 | 54.06 | 18.4986 | 17.3175 |
| `takeover-current-frame-scopes` | `d0fed580` / 仅帧级 CPU 计时 | 62.31 | 16.0477 | 15.0236 |
| `takeover-current-2` | `d0fed580` / 恢复完整计时 | 54.88 | 18.2228 | 17.2405 |
| `takeover-legacy-2` | `e971a229` / 原有帧级计时 | 113.76 | 8.7903 | 8.0789 |

相邻提交确认原始接管回退依然可复现，Probe GI 并非必要条件。当前版本恢复了相当一部分性能，但在仅保留帧级 CPU 计时的诊断中，RenderFrame 仍比旧版多约 6.77～6.94 ms。它是同画质下两个实现的差额，包含 RDG、其他 RHI 重构及剩余测量成本，不能直接命名为“纯 RDG 算法税”。

两次完整计时的当前版本结果接近，夹在中间的帧级诊断比它们少约 2.22～2.29 ms。每个完整 RenderFrame 内主线程 CPU 事件数分别为：旧版 1、接管初版 57、当前 152、帧级诊断 1；当前还有 15 个 setup worker 事件。PrepareFrame 在 RenderFrame 之外，未计入上述帧内事件数。这表明细分 CPU 采集的影响不可忽略，但只有一组帧级诊断，不能把该差额当作精确、恒定的 profiler 成本。后续比较优化收益应固定计时集合，并用较少计时的运行核对整帧结果。

### 14.3 当前开销分布

下表是同线程完整帧的区间归因，录制已扣除嵌套 UI 图的 Compile、Lowering 和物化。每列是各自的中位数，不能相加；worker 时间不与 render thread 时间相加。

| 阶段 | 当前第 1 次 p50 ms | 当前第 2 次 p50 ms |
| --- | ---: | ---: |
| 主 Pass 声明及同步准备 | 3.9692 | 3.9387 |
| 全部图 Compile | 3.3661 | 3.3811 |
| 前端命令录制与合并 | 3.1603 | 3.1219 |
| 全部图 Lowering | 1.6797 | 1.6234 |
| 提交接受等待 | 1.5519 | 1.4703 |
| GPU profiling Seal/Drain | 1.4127 | 1.4185 |
| 主图生命周期内其余工作 | 0.5217 | 0.5082 |
| 后处理声明及准备 | 0.3996 | 0.3940 |
| 主图资源导入 | 0.2588 | 0.2585 |
| 屏障物化 | 0.1253 | 0.1219 |

主 Pass 的约 3.94～3.97 ms 中，同步 Prepare 占约 2.18～2.20 ms，其中 CSM 约 2.01～2.02 ms；Scene 导入约 0.74～0.75 ms，其余注册/包装约 0.94～0.96 ms。Probe Prepare 已降至约 0.10 ms。不能将整段 3.9 ms 都作为 RDG 元数据工作。关闭 Probe 后，每帧只观测到一次 Scene 读取集合导入，来自 Geometry；此前多个 Pass 共享 Scene 导入结果的候选已不再优先。

主图公共 Compile 约 2.69～2.72 ms，内部 compiler 函数约 2.30～2.33 ms。依赖分析约 0.80 ms，最终屏障生成约 0.33～0.34 ms，区间构造约 0.33～0.34 ms，含边排序的执行顺序生成约 0.13 ms。setup Wait 每帧 p50 约 0.079 ms，最大 0.320 / 0.496 ms；本轮稳定窗口最长主图 Compile 为 4.69 / 5.53 ms，没有重现先前的大停顿，不代表原长尾问题已解决。

接管初版与当前都每帧编译三个图。初版全部 Compile p50 为 6.9513 ms，当前约 3.37 ms；仅扣除嵌套 Compile 后的录制区间为 5.9515→3.2820 / 3.2360 ms。后者尚包含嵌套图的准备，与上表进一步扣除 Lowering/物化的录制口径不同。旧路径没有这些 CPU scope，也没有 Vulkan reorder/native-record/native-submit 的 CPU scope，其成本不可测，不能填成零或从总帧差额反推。

GPU 也不能假设已经恢复。共同的 `Raster Geometry Draw` scope p50，旧版两次为 0.3645 / 0.3523 ms，当前为 0.5673 / 0.5724 ms，帧级 CPU 诊断为 0.5181 ms。对应 Lighting Pass 的 p50，旧版约 0.63～0.65 ms、当前约 1.19 ms；AO 约 0.53～0.55→1.51～1.52 ms。GPU 数据没有 host 时间戳，使用独立的 `frame_id > 300` 窗口，且新旧 Pass 边界不同，不能与 CPU 最后 30 秒精确对齐、相加为 GPU 帧时间，或认定 shader 本身变慢。后续需控制 GPU 运行状态并检查屏障、流水线停顿与时间戳边界，区分真实 GPU 工作和同步/测量影响。

### 14.4 保持架构的下一步

当前仍有明确的优化空间，先针对已看到的重复工作，而不是减少必要的依赖和生命周期保证：

1. **Lowering 的资源集合整理与暂存分配。** `RenderGraphCompiler.cpp` 已按 pass/resource/cell 输出访问；`RenderGraphLowering.cpp` 仍按 cell 收集重复资源，再逐 Pass sort/unique。可顺序跳过相邻重复资源，另行合并 identity-only references。对已知来源数量的 `source_frontier`、指令列表预留容量，再测量局部 scratch 使用现有 arena 思路是否有效。最终 LoweredPlan 和物理强引用必须继续独立拥有内存。先用少量粗粒度计时区分验证、keepalive 收集、屏障转换和边界覆盖检查，不能将整个约 1.6 ms 都视为可删除。
2. **录制准备的所有权复制与查找。** `RenderGraphRecording.cpp` 在图已经进入不可再次执行状态后，仍把每个 `std::function` callback 复制到已有 recording lifetime；可检查改为移动所有权。逐 Pass 的 `lowered.Keepalive()` 又线性查找 Pass，现有 execution-order 索引可以避免重复查找；同一串行 range 内重复强引用也可只保留一次。需要验证图析构后录制数据仍有效、并行任务独占、失败时不提交半张图及跨队列 release 所有权，不能直接删掉保活。
3. **按实际访问形状继续优化编译。** 依赖分析仍是主图 compiler 内最大的阶段。其每条访问扫描对应资源全部 cells 的成本值得计量，但 Raster 多为 Whole/少量 cells 时，新增复杂区间索引未必值得。应先记录访问量与 cell 分布，避免再次为小输入引入额外组织成本。

GPU 同步与 profiling 应单独排查；它们可能解释部分整帧差额，不能被 CPU 编译优化掩盖。Lowering 的物理绑定、状态完整性和所有权检查具有执行前验证职责，不因为 Compiler 已分析依赖就一概删除。上述候选尚未实施或证明毫秒收益；跨帧缓存、合并图边界或队列架构调整仍需先征求用户同意。

### 14.5 验证和证据

历史两版编辑器构建、诊断 Core 构建及恢复构建均成功。六次 Raster 正常退出，日志中运行/validation 错误为 0。六个 capture 均有完整尾包，CRC 校验通过，无截断、packet-index 缺口或 Loss 通知，SessionEnd 的 dropped records 均为 0。本轮最终仅修改本文，未保留 profiler filter 或渲染实现改动；不重复运行与文档无关的契约测试。

Render DLL SHA-256：旧版 `ADE524A6B9FA641F9CB42C0B305A1E8ACB8ABDE80A028715DA4818420D7E8F0A`，接管初版 `E7752D2C1709ACAB016C283E7E0C7308AD0D618605AEDA34FC4B57FAB4C303E5`，当前 `7C1BFB9710A9A1D6CD464252C91E630556409C4C0183CBD8B2050F6FBCD39F7E`。匹配 EXE/全部 engine DLL 及 manifest 保存在 `build/rdg-takeover-variants/`；历史 Probe 配置补丁也保存在该目录。主工作区最终 EXE/DLL 全部与 current manifest 匹配，ProfileScope 源码恢复；历史 worktree 恢复原 `1244d4f4` 且干净，测试编辑器已关闭。

运行目录位于 `build/rdg-bisect-results/`，名称见 14.2；每个目录保存 capture、配置、日志、进程信息和 `takeover-summary.json`。旧版重复运行的 metadata 同时保留采样时 checkout 和实际构建 commit，避免切换二进制后误标版本。汇总为 `build/rdg-takeover-summary.json`，分析日志为 `build/rdg-takeover-analysis.log`，当前细分结果为各目录的 `raster-frame-stages.json`、`raster-prepare-hotspots.json`、`compiler-stages.json`。执行脚本为 `build/run_rdg_bisect.ps1`、`build/run_rdg_takeover_remaining.ps1`，分析脚本为 `build/analyze_rdg_takeover.py` 及前文三个细分脚本。以上为本机忽略目录内的证据，关键条件与结果由本文保留。
