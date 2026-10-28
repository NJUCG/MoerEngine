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
