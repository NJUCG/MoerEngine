# RDG CPU 开销归因记录（2026-10-30）

## 目标与实施顺序

以 `04091acc` 为起点，解释第 16 节 UE 对照中剩余的 CPU 开销。此轮先取得归因证据，不预设需要改写架构；图边界、队列、资源状态和生命周期调整仍先征求用户同意。

1. 保留 MSVC Release 的 `/O2 /Ob2 /DNDEBUG`，补齐匹配 PDB，采样完整生命周期及 Compile/Lower 调用栈。
2. 细分 Lower 的验证、保活和屏障输出，并记录编译、构图、录制及析构阶段。
3. 分开采集分配次数/字节与操作数量，不把开启统计时的耗时当作性能基线。
4. 对 UE 复核相同输入的实际工作量及命令排空边界，区分实现差距和计时口径差异。

正式耗时使用固定逻辑 CPU 2、预热及重复运行；采样/分配诊断与正式计时分开，不与构建、编辑器或大文件解析并发。原始产物保存在 `build/rdg-profile/`；本文保留可独立阅读的条件、结果和限制。

## 1. 优化构建和调用栈

独立构建继续使用 `build/rdg-release`，输出在 `build/rdg-release-target/bin/Release`。保留原有 CUDA/NRD 关闭配置，增加 `/Zi`，链接增加 `/DEBUG:FULL /OPT:REF /OPT:ICF` 并继续 `/INCREMENTAL:NO`。显式保留 REF/ICF，避免调试符号选项间接关闭链接优化。不以 Debug/Release 的差异作为代码优化收益。

本机具有 WPR/WPA/xperf，启动前 WPR 无其他活动会话。`wpr -start CPU -filemode` 返回 `0xc5585011`，错误为“Failed to enable the policy to profile system performance”。没有修改系统策略或中断他人的采样。改用用户态工具采样本轮启动的基准子进程；这种采样会暂停目标线程，不能充当无扰动计时或系统级线程等待分析，须报告成功展开数量和采样开销。

构建日志：`build/rdg-profile-symbols-{configure,build}.log`。后续结果按以上顺序补充。

### 1.1 首轮调用栈结果

`tools/rdg_benchmark/sample_process.cpp` 启动自己的隐藏子进程，只暂停选定的基准线程，获取 x64 上下文并用 DbgHelp 展开；在线程恢复后解析符号。没有修改被测算法。`analyze_stacks.py` 输出叶子与包含子调用的采样次数；这些是受到采样干扰的墙钟观察，不是精确 CPU 周期。

| 输入 | 样本数 | 采样窗口 s | 累计暂停 ms | 最长暂停 ms | 少于 3 帧的栈 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Whole 64/64/16 | 1873 | 29.330 | 4838.94 | 25.391 | 0 |
| Whole 128/128/16 | 2547 | 39.868 | 6279.17 | 39.986 | 0 |

两轮各有一次退出边界上的采样失败，子进程正常退出；预热 200 次，生命周期分别重复 24000 / 16000 次，固定 CPU 2，采样器在启动 300 ms 后开始，轮询间隔带抖动。采样暂停约占窗口 16%，不能把其下的生命周期耗时当作性能结果。Windows 系统 DLL 未配置私有符号，部分底层地址只能归入附近导出符号；本工程函数已由匹配 PDB 展开。

128 Pass 中包含 Compile 的栈占 45.90%，BuildSemanticDependencies 占 32.08%，Lower 占 24.50%，AddEdge 占 13.03%，BuildGraph 占 10.09%，图析构占 5.03%。这些包含关系有重叠，不能相加。堆分配/释放路径也频繁出现，需要下一步用请求次数、调用位置与阶段计时核实，不能直接将采样比例宣称为可删除成本。

符号版 Render DLL SHA-256：`229CFCC58437CF86BC91342B9D5F382491109CDBDFFAE89398A0501FCFCA2BD1`；匹配 PDB：`2D25449BF87CEE568B19AF18F52A114158FCAD3DEC497BCD89CB0F1C67FABC94`。原始栈、元数据、子进程日志与汇总为 `build/rdg-profile/moer-lifecycle-{64,128}.*`。

## 2. 阶段计时及采集开销

新增默认关闭的 `MOER_RDG_DIAGNOSTICS`，只用于 Windows 测试构建。普通构建中的阶段/操作/分配宏全部展开为空，不改变 RDG 算法。诊断构建输出在 `build/rdg-profile-target`，不替换普通 Release 或常用 Debug。`--diagnose timing`、`operations`、`allocations` 分别采集，计数和分配栈均不混入计时轮次。

计时使用每线程的固定统计表，在图结束之后汇总；记录 inclusive 和扣除嵌套阶段后的 self，析构单独计量。Lower 分为计划验证、Pass/保活构造、屏障转换与放置、边界/跨队列配对验证、输出提交及暂存销毁。Compiler 沿用既有阶段并补记 SortEdges。200 次预热、600 样本、三轮；表中为各轮 p50 的中位数，单位 ms。

| 阶段 | 64 Pass | 128 Pass |
| --- | ---: | ---: |
| 完整生命周期 | 1.0406 | 2.1630 |
| 构图 | 0.2063 | 0.4126 |
| Compile 公共入口 | 0.4351 | 0.9483 |
| 其中依赖分析 | 0.3026 | 0.6732 |
| 其中边/原因排序 | 0.0392 | 0.0934 |
| 其中 QueuePlan | 0.0253 | 0.0495 |
| Lower：计划验证 | 0.0187 | 0.0377 |
| Lower：Pass 与保活 | 0.0212 | 0.0434 |
| Lower：屏障转换与放置 | 0.1739 | 0.3420 |
| Lower：边界与转移验证 | 0.0184 | 0.0378 |
| Lower：提交及暂存销毁 | 0.0011 | 0.0015 |
| Record 入口 self（已扣 Lower） | 0.1107 | 0.2309 |
| 命令流析构 | 0.0096 | 0.0189 |
| 图析构 | 0.0352 | 0.0737 |

这些是**诊断构建**的数值，不能替换第 16 节的正常性能结果。同期符号版普通 Release 生命周期为 0.8803 / 1.8405 ms；诊断二进制关闭采集时为 1.0311 / 2.1495 ms，仍有约 17% 干扰，来自额外探针调用、分配入口及代码布局等，尚未逐项隔离。启用阶段计时后相对该诊断二进制约再增 0.9% / 0.6%。因此本轮用阶段结果定位工作，最终优化收益必须回到关闭诊断选项的二进制验证。阶段中位数也不能直接相加为生命周期中位数。

结果支持依赖分析和 Lower 屏障输出优先。计划、边界验证有实际成本，但这批图中不是 Lower 的主要部分；不能据此先删除执行前校验。原始结果为 `timing-{64,128}-{1,2,3}.json`、`diagnostics-inactive-*`、`baseline-lifecycle-*`，阶段汇总为 `summary-{64,128}.json`。

## 3. 分配请求与操作数量

分配诊断只在单独轮次开启，按最内层阶段记录请求、字节和调用栈。普通 new/delete 在 Render DLL 与基准 EXE 内替换为保持 CRT 配对及 new-handler 行为的计数入口；`Memory` API 记录自定义分配请求；默认 PMR 上游单独包装。自检最初发现 `std::pmr::new_delete_resource` 从标准库 DLL 分配，未经过本模块 new，已经补充独立 PMR 包装，并验证普通、对齐、PMR、Memory 四种路径与对应释放。没有把未覆盖路径伪装为零。

覆盖边界：不包含 Core 内部直接的 new、C malloc、绕过 Memory API 的分配器调用、其他 DLL 的分配和驱动内存。以下是所覆盖入口的**累计请求字节**，不是存活/峰值内存；PMR 上游请求不等于 arena 内部子分配次数。3 次预热、3 个相同图样本，统计表来自末轮；全部操作计数逐轮一致，调用栈表无丢失。

| Whole 输入 | 普通分配 | 普通请求字节 | PMR 上游分配 / 字节 | Memory 分配 / 字节 |
| --- | ---: | ---: | ---: | ---: |
| 16/16/4 | 1028 | 346948 | 19 / 45896 | 114 / 17712 |
| 64/64/16 | 8864 | 6050754 | 22 / 525400 | 404 / 193800 |
| 128/128/16 | 18533 | 11776970 | 22 / 1046560 | 792 / 375520 |

128 Pass 中，依赖分析阶段仍有 **8762 次普通分配**，Lower 的屏障转换/放置有 **4334 次**，构图有 2345 次，QueuePlan 有 1202 次。Lower 中 3042 次分配来自指令来源列表；Compiler 多处来自 CompiledBarrierSource 和 CompiledEdgeReason 的容器扩容。普通分配数远大于 PMR 上游请求数，说明此前 arena 优化覆盖了暂存，未覆盖大量独立拥有内存的输出及嵌套列表；不是“已有独立 allocator，因此分配已经不是问题”。

| Whole 工作量 | 64 Pass | 128 Pass |
| --- | ---: | ---: |
| 编译后访问 / cell 相交测试 | 2048 / 2048 | 4096 / 4096 |
| 输出依赖边 | 244 | 658 |
| AddEdge 调用 / 最终原因条目 | 4660 / 4660 | 9287 / 9287 |
| 原因列表容量增长（含首次分配） | 1905 | 4412 |
| 屏障描述 / 来源条目 | 1652 / 2744 | 3298 / 5459 |
| Lower 指令列表容量增长 | 636 | 1292 |
| 资源保活条目 | 2048 | 4096 |
| MapScope 调用 | 4524 | 9013 |
| 边界候选检查 | 6144 | 12288 |

这两类 Whole 图每条访问只检查一个 cell，因此不能把此处主要差距解释为区间扫描复杂度爆炸。658 条边背后仍保留 9287 条资源/区间原因，产生 4412 次原因列表增长；AddEdge 的显著采样占比有了操作量和分配证据。原因条目在最终排序去重后没有减少，不能未经语义审计就称它们是重复垃圾。

Subranges 128 图相同 4096 条访问需要 32768 次 cell 测试、45056 次边界检查，但普通分配降至 11650 次，原因条目只有 1536；这解释了为什么“更多 cells”不必然意味着该基准更慢，输入的依赖形状同样重要。六类输入的完整结果在 `{operations,allocations}-{whole,subranges}-{16,64,128}.json`，包括所有调用栈、字节、计数和计划摘要。

## 4. UE 的实际工作量和计时边界

UE 继续使用本机 5.7.1 Development、NullRHI、CPU 2，关闭图内并行与 trace。新增记录图析构、RHI 命令排空，以及包含排空的完整总计；正式矩阵为 200 次预热、600 样本、三轮中位数，不启用 WorkStats 或调用栈采样。

| Pass 数 | 原口径生命周期 ms | 图析构 ms | 命令排空 ms | 含排空总计 ms |
| --- | ---: | ---: | ---: | ---: |
| 16 | 0.038303 | 0.003099 | 0.001799 | 0.040103 |
| 64 | 0.363100 | 0.007100 | 0.001900 | 0.365097 |
| 128 | 0.742398 | 0.011999 | 0.002000 | 0.744402 |

计入排空后，中、大图的主要差距仍存在：与本轮普通符号版 Moer 的 0.8803 / 1.8405 ms 相比，约为 2.41 / 2.47 倍。不能用排空边界解释掉差距；此处依旧是模拟/NullRHI 的 CPU 管理对照，不是 Vulkan 或整体引擎性能。

### 4.1 工作量

新增 `-WorkStats`，在计时循环外通过匹配 PDB 只读解析 RenderCore 已有的 `GRDGStat*` 变量；不修改 UE 引擎二进制、不重置计数，也不使用 FRDGBuilder 的私有内存布局。缺少符号时诊断失败，不把缺失读成零。每种规模单独进程，1 次预热加 3 个样本；累计计数差额除以 4 得到每图值。MemoryWatermark 在源码中为 max，直接报告 after 值，不能除以图数。

| UE 每图工作 | 16 Pass | 64 Pass | 128 Pass |
| --- | ---: | ---: | ---: |
| 统计 Pass（含首尾哨兵） | 18 | 66 | 130 |
| 被裁剪 Pass | 0 | 0 | 0 |
| Texture / Buffer 数（各） | 16 | 64 | 128 |
| Texture / Buffer 引用数（各） | 64 | 1024 | 2048 |
| PassDependencyCount | 15 | 110 | 282 |
| transition 描述条目 | 369 | 4932 | 9981 |
| transition batch | 8 | 32 | 108 |
| RDG allocator watermark 字节 | 121924 | 1092516 | 2264180 |

两边回调数和声明的资源访问数一致，没有 UE 裁剪掉空 Pass 的问题。UE 的纹理有 4 mip × 2 layer，其状态处理会枚举子资源；其 transition 条目数甚至高于 Moer 的屏障描述数。但这些不是同一表示层级，也不是 GPU barrier 调用数：Moer Whole 输入通常保持整体 range/cell；UE 的 batch 也不能等同于 Moer queue batch。UE 的 PassDependencyCount 与 Moer 的显式 RAW/WAR/WAW/状态边和完整原因列表同样不是可逐条比较的指标。

源码核验位置（本机安装版）：`RenderCore/Private/RenderGraphPass.cpp:225` 附近在 AddTransition 中计条目；`RenderGraphBuilder.cpp:2219` 更新 allocator 水位；`RenderGraphBuilder.cpp:1156` 计 PassDependencyCount。UE 水位是图 allocator 的 max，不能与 Moer 的累计分配请求字节比较内存倍数。

### 4.2 UE 调用栈

最终有效运行采样 RenderThread，200 次预热、40000 个 128 Pass 图样本；采样器启动后等待 8 秒，2350 次采样，展开栈均不少于 3 帧，子进程 exit 0。一例退出边界采样失败。采样窗口 36.8322 秒，累计暂停 9833.46 ms（约 27%），最长 79.485 ms；仍只用于定位，不采用其计时值。

包含 Execute 的采样约 63.45%，CollectPassBarriers 33.74%，SetupPassResources 22.30%，Compile 9.87%，SetupPassDependencies 9.74%，图析构 2.94%。这些是嵌套占比，不能相加。UE 已在 AddPass/SetupPassResources 做了部分资源准备，所以单独比较两边名为 Compile 的函数会遗漏 UE 工作。

本机 UE 的底层分配栈显示 **FMallocMimalloc**，不能笼统假设为 Binned。FRDGAllocator::GetTLS 包含占比约 3.28%，FMemory::Realloc 约 0.98%；没有出现 Moer 样本中同等显眼的逐项系统堆分配/释放热点。结合 `RenderCore/Public/RenderGraphAllocator.h:207` 的 ResizeAllocation 实现，UE 的许多图内数组向 FRDGAllocator 申请空间、在图生命周期统一管理。它说明可借鉴的是减少分散分配及匹配生命周期，不能直接推论“只把 malloc 换成 mimalloc 就能达到 UE”。本轮未采集 UE 的逐次堆分配账本，因此不声称其分配次数是 Moer 的某个比例。

正式 UE 结果为 `ue-timing-matrix.json`、`ue-work-{16,64,128}.json`；有效调用栈为 `ue-lifecycle-128-final.*`。前两轮采样的基准计算完成，但结果保存失败、exit 1，保留为无效完成样本：根因是 PowerShell 转发未引用的 `-Output=D:/...` 参数时，将盘符与后续路径拆开。采样器现记录完整子命令，调用脚本引用整个键值参数；独立小图验证保存成功后才重新采样。原失败文件未覆盖。

## 5. 可执行结论与边界

1. **先优化依赖原因和屏障来源的构建与存储。** 当前 Whole 图的 cell 检查与访问数相同，依赖分析却有大量小分配，输出边的数量掩盖了更大的原因列表。优先试验已知容量预留、减少临时列表和输出之间的重复构造；维持最终计划的内容、确定性及独立所有权。不能直接删除原因条目，因为本轮最终去重后条目数没有减少。
2. **Lower 优先处理输出分配。** 来源列表的逐条分配、指令列表的反复增长已有证据。先优化这些，再决定是否需要缓存 MapScope。边界校验约占 Lower 的一部分，但不是首要热点，不以删除安全检查换分数。
3. **输出 arena 是有依据的候选，但尚未实施。** 现有 scratch arena 生命周期太短，不能直接承载会逃逸的 CompiledPlan/LoweredPlan 输出。若要为输出增加拥有内存的 arena、改变输出布局或合并生命周期，需先说明所有者和销毁边界并征求用户同意；不能简单复用 scratch 后留下悬垂内存。
4. **Debug 可用性与 Release CPU 效率继续分开跟踪。** 本轮没有修复此前 Debug 帧率下降，也没有把诊断版耗时当作新的性能基线。先在关闭诊断选项的匹配二进制做前后对照，再验证真实 Raster 与 GPU 同步行为。

本轮只新增测量能力，没有实施以上生产优化，也没有改变调度、屏障或资源生命周期架构。

## 6. 验证、产物与已知限制

- 普通配置保持 `MOER_RDG_DIAGNOSTICS=OFF`。完整 Debug 构建及 10 项引用/RDG/RHI 契约通过；普通基准明确拒绝 `--diagnose`，没有悄悄启用探针。
- 开启诊断的 Release 构建及 11 项契约通过，包括分配覆盖、计数、嵌套计时的自检。最终构建再次复现六种输入的工作量、成功分配请求/字节和 Compile/Lower 计划摘要；六种摘要也全部匹配加入探针之前的 `04091acc` 基线。
- 真实 Vulkan `--parallel` 为 29 PASS、0 FAIL、0 validation 错误。首轮误从仓库根目录启动，因相对 shader 路径错误在首个用例前退出；改从 `target/bin/Debug` 启动后通过，没有为此修改渲染代码或放宽检查。
- 常用 Debug Raster/Sponza 编辑器用独立设置目录运行 60 秒并正常关闭，运行/validation 错误为零。此轮只作功能验证，没有采集或报告 FPS。用户设置和场景文件没有修改。
- UE 工具最终构建成功；正式矩阵、三种 WorkStats 及最终采样均完成。UE 启动仍有禁用 ACL 插件后的 8 条默认动画压缩资源警告，成功运行没有错误。

构建和验证日志为 `build/rdg-profile/final-{debug,diagnostic}-{build,contracts}.log`、`final-vulkan{,-retry}.log`、`editor-smoke/`，最终 CLI 及工具构建为 `final-cli-*`、`ue-tools-final-build.log`、`sampler-verified-build.log`。`script-smoke`、`script-utf8-smoke` 只验证自动化脚本和计数重现，不作为性能数据；脚本显式输出 UTF-8，避免 Windows PowerShell 5 的默认 UTF-16 重定向影响后续分析。

用于分配/阶段测量的匹配 EXE、Core/Render DLL 和 PDB 保存在 `build/rdg-profile/diagnostic-binaries/`，附 SHA-256 manifest。最终诊断 Render DLL 为 `9F30709A0C4EA7C189B37866735EC5A4793EEB8B708FB4D0AC53E71792AAB3BB`，最终 Core DLL 为 `90D512E12801763164D83A646095CA4751C1058BC6AF50A1848B6D96BBA1958D`；后续只整理了计数入口的重复代码和 CLI，复测确认计数/摘要一致。复现入口为 `tools/rdg_benchmark/README.md`、`run_diagnostics.ps1`、`analyze_stacks.py`。

限制仍保留：没有成功取得系统级 ETW 等待分析；用户态调用栈和诊断构建都有可测扰动；分配覆盖不是进程完整堆账本；UE 与 Moer 的内部状态/屏障表示不完全相同。已取得的证据足以确定下一轮优先调查的具体数据结构，但还不能把所有约 2.5 倍差额精确分摊给每一个机制，也不能据此推算 GPU 或 Debug 帧率收益。
