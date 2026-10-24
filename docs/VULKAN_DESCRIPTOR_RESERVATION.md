# Vulkan 临时描述符：提交预留与录制游标

## 从约束推导边界

GPU 按描述符表的基址和偏移访问数据，存储必须满足对应布局和对齐要求。当前 binding template 把一次 bind 的资源描述符打包为一个表，通过统一基址加 set/资源偏移寻址，因此本实现以一次 bind 为最小连续分配单位；把多个 set 合并分配是当前实现的组织选择，并非 Vulkan 要求整个 bind 的所有 set 相邻。不同 bind 的表不必相邻。

描述符一旦被录制的命令引用，必须保留到该命令不再可能执行，或其 GPU 执行完成。对于非 coherent 内存，flush 的边界还必须避免触及其他仍在写入的提交所拥有的 memory atom。

这些约束要求连续的单次绑定、明确的所有权和完成后的回收，并不要求整个 context 或整个提交连续，也不要求每次 bind 都向全局 allocator 申请空间。

原来的路径在 `BindShaderArguments` 中锁定提交 lease，推进末尾 block 的游标；尾部不足时，再从共享 free list 申请按 64 KiB 取整的 block。因此容量检查发生在录制中途，小提交也会保留一个完整 block。共享 lease 的并行 context 还需要逐次争用同一把锁。

## 新的所有权模型

| 对象 | 责任 | 有效期 |
|---|---|---|
| 全局 allocator | 管理未分配区间，完成整批预留和归还时的相邻区间合并 | Device |
| 提交 lease | 独占预留区间，提供 flush 范围，承接既有 Completion 回收 | 提交准备至 GPU 完成或确认丢弃 |
| Recording plan | 记录每条命令可以使用的不可变区间序列 | CPU 录制及必要的失败重录 |
| Context 的 cursor | 按绑定顺序消耗当前命令的区间，区间用尽后切换下一段 | 当前命令的一次录制 |

预留跟随命令分配，而不是池中的 context 编号。调度器原本就保证一条命令同一时刻只交给一个 context；录制这个命令的 context 因而独占其区间。一个 context 可以依次取得多条命令的游标，一条命令也可以跨多个区间。这样串行录制、并行录制以及失败后的串行重录使用同一份预算，不需要再维护一套与 worker 划分同步的分配状态。

计划只是 CPU 元数据。录制结束后可以销毁；GPU 存储仍由提交 lease 保留。persistent bindless 表继续使用自己的 lease 和 slot retirement，与临时命令游标分开。

## 预留与录制

1. 提交进入 preprocessing 和 native command recording 之前，读取 pipeline 已生成的 binding template，收集各次 bind 的资源表大小。Dispatch 和 SetDrawState 各一次，MultiDraw 按 batch element 逐次计算；条件执行的 bindless 更新最多三次内部绑定，TLAS 实例更新最多一次。Sets、push descriptors、直接地址路径没有临时描述符字节需求时不占空间。
2. allocator 在候选 free list 中按绑定顺序做 first-fit。每次绑定保持连续，同一命令的相邻区间合并；整个批次可以利用多个空洞。
3. 只有各个 lease 占用区间的末端向 `max(descriptor alignment, nonCoherentAtomSize)` 取整。一个 lease 内的命令可以共享 atom，但不同 lease 不共享。临时预留不再逐 block 向 64 KiB 取整。
4. CPU 元数据和 GPU 区间全部准备成功后，一次提交 allocator 状态变更。空间不足或准备时 CPU 分配失败不会留下部分 GPU 预留。
5. 录制时只查找命令的预留并推进本地 cursor。游标越界表示预算与实际绑定不一致，是逻辑错误；此处不再扩容、等待或访问 allocator 锁。描述符编码和 CPU 字节缓存仍有原来的同步责任。

只有预留和归还接触共享 free list。不同提交仍可能同时准备或完成，这一层保留同步。归还所需的 free-list 容量在分配阶段预留，避免 Completion 为归还 GPU 区间再申请 CPU 内存。

## 失败与回收

容量不足现在使用已有的“录制前可恢复拒绝”路径。整批命令尚未执行 preprocessing，不会出现部分 payload 被消耗后再尝试重放的问题；其完成通知仍由现有提交系统处理，不把普通容量不足标记成 device fault。

并行 worker 失败后，原有 join 保证所有 worker 已停止，失败 wave 的命令缓冲不会提交。fallback 重置各命令的 cursor，重写同一批预留地址即可。因此队列不再需要 descriptor checkpoint/rewind：区间所有权从未改变，原机制防止重录额外消耗空间的责任，转移到了不可变预留和“join 后丢弃失败命令”的既有边界上。

成功录制后，在所有写入者结束时 flush。正常提交继续在 GPU 完成后归还 lease；未提交的工作在确认不会执行后归还；GPU 失败而无法确认完成时仍保留原来的不复用规则。

## 改善与代价

容量改善来自两点：允许一个录制任务利用多个空洞，以及去掉临时提交的 64 KiB 尾部取整。去掉逐次 allocator 锁改善的是 CPU 录制竞争，不能代替容量管理。

固定 buffer/heap 的总容量仍然有限。一个 bind 本身过大、碎片不能容纳该 bind，或未完成提交占用过多时，预留仍会失败。First-fit 也不保证找到所有理论上存在的排列。本实现采用立即、可恢复的整批拒绝，没有增加隐式 GPU 等待、自动切分提交或 heap 迁移；调用方若要重试，必须保留或重新构造被拒绝的工作。

前置预留引入一次需求遍历和每条有需求命令的元数据。绑定稀少、单线程录制时，这些成本可能超过省下的锁开销；绑定密集或共享 lease 的并行录制更容易获益。使用固定且明确的容量边界，换取录制期间无需争用 allocator、不会因正常容量不足而半途失败。

可复现的容量、性能和 GPU 验证入口见 [测试说明](../tools/descriptor_reservation/README.md)。
