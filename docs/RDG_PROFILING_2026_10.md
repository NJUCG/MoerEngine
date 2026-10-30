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
