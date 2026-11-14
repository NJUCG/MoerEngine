# RDG CPU 性能分析：工具选择、原理与使用指南

日期：2026-11-01。对象：Windows 上的 Moer RDG；当前机器为 AMD Ryzen 9 9950X。

## 1. 先确定要解释哪一种时间

RDG 的 CPU 问题有四个不同层面，不能只靠一个函数耗时或一个火焰图回答。

| 问题 | 需要观察的事实 | 首选工具 |
| --- | --- | --- |
| Compile/Lower 的计算花在哪里？ | 运行期间的调用栈、调用者、源代码位置 | Superluminal；免费替代为 WPR + WPA |
| 并行录制为什么没有变快？ | Running / Ready / Waiting、唤醒关系、任务完成的关键路径 | Superluminal 或 WPA 的 CPU Usage (Precise) |
| 已经找到循环，但它为什么慢？ | 指令数、周期、缓存访问、分支失误；必要时 IBS | AMD uProf，具体事件以本机驱动和 CPU 支持为准 |
| RDG 优化是否真的让 GPU 更早开始工作？ | CPU Vulkan 调用、提交时刻、GPU 队列活动和空洞 | Nsight Systems |
| 是否在为原因列表、来源列表等反复分配？ | 分配请求数量、字节、调用位置及对象生命周期 | 项目现有的独立 allocation/operation 诊断，结合上述采样 |

这些是不同的观测机制，不是四套必须常驻的框架。日常使用一套 CPU 时间线工具；遇到具体假设时，再启用硬件事件或 GPU 时间线。

### 1.1 墙钟时间、CPU 时间与关键路径

对一个选定线程、选定区间，可以近似分解为：

`墙钟时间 = Running + Ready + Waiting`。

- **Running**：线程在逻辑 CPU 上执行。执行可能推进业务，也可能忙等、处理重复数据或等待内存。
- **Ready**：线程可运行，但还没有获得 CPU。此时优化 RDG 内部某个函数未必解决调度争用。
- **Waiting**：线程阻塞，等待锁、任务、I/O、GPU fence 等。应沿唤醒关系调查生产者，而不是只优化等待函数。

CPU 栈采样主要解释 Running 中的成本；上下文切换和 ReadyThread 事件解释后两者。仅凭一个等待函数的长条，不能区分“工作还没完成”和“工作已完成但线程没有被及时调度”。

多个录制线程的 CPU 时间总和可以超过帧时间。应观察决定最终提交时间的任务链和汇合点；将所有并行任务耗时相加，不会得到实际帧延迟。纯串行 RDG benchmark 不能验证并行录制的负载均衡。

### 1.2 栈采样、埋点和硬件事件分别承诺什么

**栈采样**在若干时刻观察线程。足够多的样本可以估计热点分布，不要求给每个函数加计时器。它不等于函数调用次数，也无法保证捕获每次很短的调用。优化编译后的内联、尾调用、代码合并会影响函数/行的归因。

**埋点**记录明确的起止或操作。它适合精确识别 Build、Compile、Lower、Materialize、Record、Destroy 的边界，但埋点本身、字符串处理和日志写入也有成本。已有 `MOER_PROFILE_SCOPE` 接入的是项目 `ProfileDump`，不能假设它会自动出现在 NVTX、Superluminal 或 Tracy 中。本轮不增加生产代码埋点。

**硬件事件采样**回答执行效率问题。以 retired instructions 和 cycles 为例，`IPC = instructions / cycles`；比较时还应计算 instructions/graph、cycles/graph。低 IPC 可能有多种原因，不能直接等同于缓存差；缓存 miss 和分支事件需要同时看分母、调用位置和输入规模。不同采集轮次、事件复用及采样误差也会影响比值。

**Inclusive** 包含子调用，**Self/Exclusive** 只计算本函数自身。Compile、BuildSemanticDependencies、AddEdge、堆分配往往是嵌套的，不能将其 inclusive 百分比相加。不同阶段的 p50 相加也不等于完整生命周期 p50。

## 2. 本机试用记录与可复核边界

本轮不重建主工作区，不覆盖正在开发的 DLL。已有 Release EXE/DLL/PDB 被复制到：

`build/rdg-tool-evaluation-20261101/bin/`。

快照来源是 `build/rdg-release-target/bin/Release`。哈希在 `binary-manifest.json`，工作区起点在 `source-head.txt`，初始修改列表在 `initial-git-status.txt`。记录时 HEAD 为 `8d1e84fd6946cd34c06ea9773cd9a3c0448fe980`，但已有二进制不被宣称为从该 HEAD 或当前未提交源码重建的结果。分析源码行时必须结合对应 PDB 和旧版源码，不直接套用正在改变的源码行号。

| 工具 | 本机版本/状态 | 已执行的试用 | 当前结果 |
| --- | --- | --- | --- |
| WPR / WPA | WPR 10.0.26100；WPA exporter 11.7.383.39833 | 检查无活动会话后运行 `wpr -start CPU -filemode` | 非管理员启动失败：`0xc5585011`，无有效 ETL |
| Superluminal | 本轮安装 1.0.7359.1293；CLI 可调用 | `run windows --freq 1000` 启动隔离 benchmark | exit 1，明确要求管理员权限；未取得采样 |
| Nsight Systems | 已安装 2025.1.3.140 | CPU 采样 + context switch，`--trace=none` | exit 0 并产生报告，但警告自动关闭两种 CPU 采集；该报告不能用于 CPU 热点分析 |
| Nsight Systems / Raster | 同上；独立资源、配置和设置目录 | `--sample=none --cpuctxsw=none --trace=vulkan --delay=60 --duration=10` | 报告生成，但 SQLite 没有 Vulkan API/GPU marker 数据，诊断明确报告没有 Vulkan 事件；不能用于 GPU 归因 |
| AMD uProf | 官方 5.3.521 安装包已下载 | 验证 AMD 数字签名和官方 MD5；尝试启动安装程序 | UAC 授权未完成，尚未安装、尚未采样 |

这里的安装/采集限制不是工具性能结论。特别是 Nsight 的“成功退出”和“报告文件存在”不能证明请求的数据存在。

Superluminal 官方安装包的签名已验证；实际位置为 `C:/Program Files/Superluminal/Performance`。CLI 支持无商业许可证的采集/符号处理；GUI 查看许可另行处理，未声称已激活试用。AMD 安装包 MD5 为 `c20f7d40ad49a96b5e3bf728ed2201fe`，SHA-256 为 `88A7DE6FBF1E3761D4B2C7254C0A081E27C3DE4E8D3CAC0C53C62595FCAFF572`。

原始日志在同一目录的 `logs/`；下载验证记录在 `downloads/`。已有 `nsys-cpu.nsys-rep` 是上表的无 CPU 样本报告，应保留作失败证据，不能混入有效样本。

Raster 的 `nsys-raster.nsys-rep`、SQLite、三个空 CSV 和 `logs/nsys-raster-validation.json` 同样保留。日志确认编辑器进入 compiled Raster 的帧循环、执行了 present，并在关闭后完成资源释放；取得的 Process 对象未提供有效退出码，因此不将其记为已验证 exit 0。报告的 injection-success 事件只说明公共注入库初始化，不证明 Vulkan 跟踪层已成功拦截调用。此问题尚未定位，不归咎于 RDG 或单独归咎于权限。

为管理员采集准备的脚本是 `build/rdg-tool-evaluation-20261101/collect-elevated.ps1`。它顺序运行三套工具，前后做短基线检查，不修改安全策略、注册表或源码；发现活动 WPR 会话时退出，不取消他人的会话。采集将写入新的 `elevated-时间戳/`，避免覆盖已有记录。

**尚未取得有效管理员采集前，本文不提供四款工具的实测开销排名，也不把旧的 RDG 热点当作这轮新采样结论。**

## 3. 固定输入与正确的对照方法

### 3.1 优先用纯 CPU 图验证算法成本

隔离快照支持以下命令；从仓库根目录执行：

```powershell
$case = Join-Path $PWD 'build/rdg-tool-evaluation-20261101'
$bench = Join-Path $case 'bin/TestRenderGraphCompilerBenchmark.exe'
& $bench --lifecycle --cpu 2 --passes 128 --resources 128 --accesses 16 --warmup 200 --samples 600
```

该图含 128 Pass、128 textures、128 buffers，每 Pass 各声明 16 条 texture/buffer access。`--lifecycle` 计入构图、Compile、串行录制和销毁；资源是 fake binding，回调不执行真实渲染，不能据此计算 Raster FPS。`--cpu 2` 固定 benchmark 主线程，用来减少迁移；不要给真实多线程编辑器套同样的单线程假设。

建议依次使用 16/16/4、64/64/16、128/128/16，并单独保留 `--subranges`。记录访问量、资源范围和读写形状，不能只比较 Pass 数。Whole 与 Subranges 可能产生完全不同数量的依赖原因和屏障来源。

普通基线与 profiler 运行分开：预热 200、600 样本、至少三次独立运行；报各轮分布与“各轮 p50 的中位数”。采样时使用更多重复图，如 12000 次，使短函数获得足够样本。采样中的 benchmark 时间只用于估计观测扰动，最终收益仍由无 profiler 的普通 Release A/B 确认。

避免同时运行构建、另一个编辑器、多个 profiler 或大规模符号解析。CPU 频率、后台负载、温度和固定逻辑 CPU 的占用仍可能造成波动；固定 affinity 不能消除全部噪声。

### 3.2 符号与构建

本轮使用已有优化构建及匹配 PDB。后续重新构建时沿用 [benchmark README](../tools/rdg_benchmark/README.md) 的独立输出目录。

- MSVC Release 保持 `/O2 /Ob2 /DNDEBUG`，增加 `/Zi`；链接保留 `/DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO`。
- 这不是要求将项目通常使用的 Clang/Ninja 构建改为 MSVC；具体符号格式和选项要匹配当次工具链。
- 普通计时关闭 `MOER_RDG_DIAGNOSTICS` 和 ProfileDump 采集。诊断构建即使运行时关闭探针，也不自动等价于普通 Release。
- 把 EXE、参与模块的 DLL、PDB、命令、配置和哈希一起保存。只有同名 PDB 不够，必须匹配二进制的调试标识。
- 栈中大量 unknown、地址或导出符号时，先解决符号问题。不要把不完整的栈解释成被测函数不存在。

### 3.3 真实 Raster 用来验证关键路径

CPU 图解释图管理成本；真实 Raster 用来判断它是否卡住提交。真实场景保持相同分辨率、相机、GI/阴影/AA、VSync、限帧、并行录制配置。先完成场景加载与 shader warmup，再采集稳定区间。将启动、shader 编译和稳态帧分开。

这一步应看整帧 Build/Compile/Lower/Record、任务汇合、驱动调用、submit 以及 GPU 队列活动。GPU 有空洞只是现象，还需要排除限帧、present、fence 等原因。不同帧和跨队列的数据不能仅凭横坐标接近就建立因果关系。

## 4. WPR + WPA：免费且覆盖线程调度的基础方案

### 原理与适用场景

WPR 采集 Windows ETW 事件，WPA 组织成时间线和表。本机 `wpr -profiledetails CPU` 已验证包含 `SampledProfile`、`CSwitch`、`ReadyThread` 及相关栈，适合同时看计算热点和线程调度。

优先用它回答：编译线程是否一直占用 CPU；主线程在何处等录制任务；任务完成后是否迟迟不能运行；慢帧是否被其他进程、磁盘或驱动活动干扰。Microsoft 的 [CPU 分析文档](https://learn.microsoft.com/en-us/windows-hardware/test/wpt/cpu-analysis) 区分 Sampled 与 Precise，两张表不能互相替代。

### 采集

以下在管理员 PowerShell 执行，`$case` / `$bench` 按第 3 节设置，输出文件使用新名字：

```powershell
wpr -status
# 只有确认没有其他活动会话后才执行下一行。
wpr -start CPU -filemode
try {
    & $bench --lifecycle --cpu 2 --passes 128 --resources 128 --accesses 16 --warmup 200 --samples 12000
} finally {
    wpr -stop "$case/wpr-cpu.etl" 'RDG lifecycle 128 passes'
}
```

实际自动化应像本轮脚本一样检查 start 的退出码，只有自己 start 成功才 stop。不要把 `wpr -cancel` 当作启动前的清理步骤。

### 分析顺序

1. 打开 ETL，配置本轮 PDB 目录并加载符号，先过滤目标进程和有效时间区间。
2. **CPU Usage (Sampled)**：按 Process / Thread / Stack 展开；先找 Compile、Lower 和分配路径的 inclusive 成本，再看叶子和调用者。
3. **CPU Usage (Precise)**：查看线程 Running、Ready、Wait 区间；按切出栈与唤醒线程追踪等待原因。
4. 检查采集丢事件和栈完整性。存在明显丢失时缩短窗口或调整采集配置后重采，不从缺失事件推导“没有等待”。
5. 保存 `.wpaProfile` 固定筛选和表布局。`wpaexporter` 能复用布局导出数据，适合多轮对照。

本机 `xperf` 的辅助命令已核对帮助，但对有效 ETL 的导出仍须实际验证：

```powershell
$xperf = 'C:/Program Files (x86)/Windows Kits/10/Windows Performance Toolkit/xperf.exe'
$env:_NT_SYMBOL_PATH = "$case/bin"
& $xperf -i "$case/wpr-cpu.etl" -a tracestats -detail -o "$case/trace-statistics.txt"
& $xperf -i "$case/wpr-cpu.etl" -symbols -a profile -detail -o "$case/cpu-functions.txt"
```

WPR CPU profile 不自动提供完整 C++ 对象分配账本。Windows heap tracing 是另一项采集，可能需要额外设置且扰动更大；自定义 arena/mimalloc 的子分配也未必映射为逐次 Windows heap 事件。本轮没有启用 IFEO 或改注册表。

## 5. Superluminal：日常 C++ 热点与任务等待调查的候选

### 原理与适用场景

它将栈采样、线程状态和可选 instrumentation 放入同一视图，适合从一个慢区间进入调用栈，再沿等待关系追到负责唤醒的工作线程。相较只按函数排序，更有利于调查 RDG 的 setup/record worker、汇合点和线程利用率。功能说明见 [官方文档](https://superluminal.eu/docs/documentation.html)。

这是工具能力上的推荐；本机当前尚未取得有效采样，不能把“更便于交互调查”写成已经测得的低开销结论。

### 采集及符号处理

本机 CLI 帮助已验证以下参数。管理员采集先使用 1000 Hz，避免一开始就采用默认 8190 Hz：

```powershell
$super = 'C:/Program Files/Superluminal/Performance/SuperluminalCmd.exe'
& $super run windows --freq 1000 --max-duration 45 --keep-running `
    --capture-path "$case/superluminal.etl" --working-dir "$case/bin" `
    $bench --lifecycle --cpu 2 --passes 128 --resources 128 --accesses 16 --warmup 200 --samples 12000

& $super resolve windows --symbol-location "$case/bin" `
    --symbol-cache-dir "$case/symbol-cache" "$case/superluminal.etl"
```

`--keep-running` 表示达到采集时长后保留目标程序；此处目标是会自行完成的 benchmark。命令行采集/解析的许可与 GUI 的查看许可不同，参见 [官方 CLI 说明](https://superluminal.eu/SuperluminalCmd/)。如 GUI 要求账户、试用激活或许可证，由用户处理。

进入有效区间后，按以下顺序阅读：线程状态 → 关键线程的调用树/热点 → 调用者 → 源码/汇编。查看 malloc 热点时沿父调用追到 RDG 的具体容器构造，而不是直接得出“换分配器即可”的结论。等待分析保留 context-switch stacks；不要为了缩小文件先启用 `--no-cswitch-stacks`，再期待完整解释等待关系。

## 6. AMD uProf：把“函数很热”推进到执行效率分析

### 原理与适用场景

uProf 可以利用时间采样及 AMD 硬件事件。PMU 统计事件或在阈值触发时采样；IBS 提供指令/操作相关的执行信息。具体能力随 CPU、OS、驱动和采集配置变化，不能因为使用 Ryzen 就假设每项计数器都可用。

适合在已经定位 `BuildSemanticDependencies`、来源合并或排序循环后检验具体假设：

| 假设 | 需要对照的指标 | 可以支持的优化方向 |
| --- | --- | --- |
| 每个输入被反复处理 | instructions/graph，调用及操作次数，输入规模曲线 | 减少遍历、复制和重复推导 |
| 散布的小对象导致访问局部性差 | 相关 load/cache 事件、每图 miss、源码/栈归因 | 连续布局、减少指针追逐；需要 A/B 验证 |
| 分支不可预测 | 分支数与 mispredict，相关代码位置 | 简化数据分类、按路径分组；先确认不是工作量变了 |
| 逐项分配太多 | 分配路径样本 + 项目分配请求账本 | 预留容量或按正确生命周期集中管理 |

只看百分比可能误导：其他工作变快后某个热点占比会上升，而其绝对时间没变。只看 cache miss 百分比也可能掩盖更多总访问。优先报告每图工作量与总成本，再解释比率。

### 本机状态及后续试用步骤

安装包来自 [AMD 官方下载页](https://www.amd.com/en/developer/uprof.html)，5.3.521；签名和官方 MD5 均通过。安装需要用户完成 Windows 授权，当前没有可运行的 `AMDuProfCLI.exe`，因此不提供伪装成已验证的 collect 命令。

安装完成后，先检查 `AMDuProfCLI.exe --help`、`info` 和本机可用配置，再分别进行：

1. 时间采样，验证目标模块和 PDB 是否能解析出 RDG 函数；与其他 CPU sampler 交叉核验热点。
2. 少量硬件事件的一轮采集，检查 cycles/instructions 等事件是否真实产生数据。
3. 针对已发现的循环增加缓存/分支或 IBS 采集；每轮都记录支持情况、事件配置、有效样本和采集扰动。

AMD 当前平台能力表将 Windows 的 Threading Analysis、OS Tracing 标为不支持；不能把 Linux 上的线程等待工作流直接照搬到 Windows。此处线程调度分析由 WPA/Superluminal 负责。[官方功能表](https://www.amd.com/en/developer/uprof.html) 和 [EBP 说明](https://docs.amd.com/r/en-US/68658-uProf-getting-started-guide/Introduction-to-Event-Based-Profiling-EBP) 是版本检查入口。

遇到 PMU/IBS 驱动不支持或与现有系统配置冲突时，记录限制，不为一次采集关闭安全功能。本机 AMD CPU 的硬件事件调查优先试 uProf；不预设 Intel VTune 的硬件 EBS 可用于此 CPU。

## 7. Nsight Systems：连接 CPU 图管理与 GPU 执行

### 适用边界

Nsight Systems 的价值是时间关联：CPU 工作、Vulkan API、提交与 GPU 活动。它不是 Nsight Compute 的逐 kernel 微架构分析，也不能仅凭 Vulkan 调用之间的空白识别 RDG Compile。

本机 2025.1.3 的 CPU sampling/context switch 需要管理员权限；本轮实际看到它们在无权限时被关闭。`--trace=none` 的纯 CPU benchmark 仍需要采样数据才有意义；这个 benchmark 没有 GPU 工作，不能用来评价该工具的 GPU 时间线能力。

### CPU 对照命令

```powershell
$nsys = 'C:/Program Files/NVIDIA Corporation/Nsight Systems 2025.1.3/target-windows-x64/nsys.exe'
& $nsys status --environment
& $nsys profile --sample=process-tree --cpuctxsw=process-tree --trace=none `
    --duration=40 --kill=false --resolve-symbols=true --output="$case/nsys-valid-cpu" `
    $bench --lifecycle --cpu 2 --passes 128 --resources 128 --accesses 16 --warmup 200 --samples 12000
```

本机 CLI 的 `--kill` 使用 `true/false`，backtrace 选项是 `auto/none`，不要直接复制 Linux 文档的参数。每次采集检查日志里是否有 `disabling`、环境失败、空数据或符号失败。

### Raster 应用方法

对准备好的独立 Raster 实例，使用 `--trace=vulkan`，在加载和 shader warmup 之后取短窗口。没有管理员权限时可以显式设 `--sample=none --cpuctxsw=none` 尝试 API 路径，但成功与否仍须检查实际 Vulkan 事件；即使成功，也不具备 CPU 栈归因能力。

分析时先看有效帧的提交节奏及 GPU 队列，再关联 CPU 线程。如果要给 Compile/Lower 等阶段加 NVTX，需作为独立的小改动评估开关与扰动；现有 ProfileDump 不会自动变成 NVTX。不能把 GPU 空洞直接记为 RDG 的成本。

本机 `nsys stats --help-reports` 已列出 `vulkan_api_sum`、`vulkan_api_trace`、`vulkan_gpu_marker_sum`、`wddm_queue_sum` 等报告。先确认报告所需的数据存在，再导出。CPU 调用栈分析以 GUI/对应导出表为准；不要把 `osrt_sum` 当作所有 Windows CPU 函数的汇总。

版本入口：[Nsight Systems User Guide](https://docs.nvidia.com/nsight-systems/UserGuide/)、[安装与权限说明](https://docs.nvidia.com/nsight-systems/InstallationGuide/)。实际命令优先匹配安装版本的 `--help`。

## 8. 怎样将证据用于优化当前 RDG

此前的 [RDG CPU 开销归因记录](RDG_PROFILING_2026_10.md) 已提供三个相互补充的线索，它们是旧快照结果，不是本轮工具的新发现：

- 128 Pass Whole 图的用户态采样中，Compile inclusive 约 45.90%，依赖分析约 32.08%，Lower 约 24.50%。旧采样器暂停目标线程约占窗口 16%，只适合提供调查方向。
- 独立分配诊断中，普通分配 18533 次，其中依赖分析 8762 次，Lower 屏障转换/放置 4334 次；PMR 上游只有 22 次。已有 scratch arena 不能证明整个流程已没有分散分配。
- 658 条图边背后有 9287 条原因，4412 次原因列表容量增长；4096 条访问只有 4096 次 Whole cell 检查。应优先核验原因/来源的构建和存储，不预设是区间扫描爆炸。

进一步工作应形成可证伪链条：

1. 用有效 CPU 采样确认同一快照、同类输入是否仍在这些路径耗费大量运行时间。
2. 从调用者确认成本属于必要同步语义、调试解释信息、数据结构维护还是重复转换，不能用函数名称替代职责审计。
3. 用 operations/allocations 单独记录次数和规模。采样百分比不能给出精确分配次数。
4. 用 uProf 检验“指令太多”还是“访问局部性差”等具体假设；没有硬件数据时保持为假设。
5. 每次只改变一项，在普通 Release 下比较 lifecycle 与输出一致性，再到真实 Raster 检查关键路径和 GPU 提交。

如果一个机制只服务调试解释，可以评估按需生成；如果输出逃逸 Compile 的 scratch 生命周期，就必须保留正确的所有权。不能用更短生命周期的 arena 换掉输出分配而留下悬垂引用。删除原因或合并状态前，先指出其消费者与同步责任。

### 什么不该从这些工具直接推导

- malloc 热并不证明“只换 allocator”就解决；可能是本可避免的列表复制和扩容。
- 锁的 Waiting 很长不证明锁实现慢；生产者可能在处理过多工作。
- CPU 总时间降低不证明帧延迟降低；改动可能不在关键路径。
- 每帧有 GPU barrier 不证明 RDG 无益；其必要性由实际资源 hazard 和 API 要求决定。
- UE 某个 Compile 函数较短不证明整个图管理较短；应比较一致的构图到销毁边界。

## 9. 每次报告需要留下什么

记录二进制与 PDB 哈希、对应源码/工作区状态、构建配置、场景或图输入、工具版本、精确命令、权限及采集功能；附原始 trace、目标进程退出码、符号解析和丢事件情况。保存无 profiler 基线、采集中观测值和重复轮次，不只保留一张百分比截图。

结论分成三类：**已观察事实**、**据此提出的原因假设**、**经 A/B 证实的改进**。这种区分比增加更多 profiler 更能避免错误优化。
