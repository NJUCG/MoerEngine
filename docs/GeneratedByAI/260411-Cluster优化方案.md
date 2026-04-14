# MoerEngine Cluster 优化方案

> 本文面向对 MoerEngine 还不熟悉的读者，先解释当前渲染数据是如何组织和绘制的，再说明为什么“直接把 cluster 伪装成 primitive”不是一个完整解法，最后给出一套更正规的 cluster 优化方案。

---

## 1. 当前方案梳理

这一节的目标很简单：先回答一个问题。

**MoerEngine 现在到底是按什么粒度在绘制？**

答案是：

- 逻辑层的最小几何去重单元是 `CPrimitive`
- CPU/GPU 渲染层的 draw command 也是按 `Primitive` 建立的
- 可见性裁剪时，真正被筛选的是“这个 primitive 下的哪些 instance 可见”

所以更准确地说，当前的绘制粒度不是单独的 instance，也不是单独的 primitive，而是：

**按 primitive 建立 draw，再按 instance 决定是否参与这个 draw。**

### 1.1 当前 Logical Component 结构

MoerEngine 的场景数据分三层：

- `LogicalScene`：逻辑层，适合编辑和运行时修改
- `CpuScene`：面向渲染准备的 CPU 缓存
- `GpuScene`：真正上传到 GPU 的资源和句柄

顶层说明可以看 [source/runtime/render/scene/Scene.h](../../source/runtime/render/scene/Scene.h)。

在逻辑层，和 mesh / 顶点最相关的是下面几个结构，定义在 [source/runtime/render/scene/LogicalComponents.h](../../source/runtime/render/scene/LogicalComponents.h)：

#### `CPrimitive`

`CPrimitive` 可以理解为“一个可绘制的几何片段”。它保存：

- 顶点数量
- 各种顶点属性在 MegaBuffer 里的偏移
- index buffer 的偏移
- 一个局部空间 AABB
- 材质引用

这里要注意一个实现细节：

`CPrimitive` 虽然叫“最小可渲染单元”，但它更像是**当前渲染系统选定的最小几何去重单元**。这句话非常重要，因为 cluster 优化的本质，就是把这个“当前的最小单元”继续往下切。

#### `CMesh`

`CMesh` 只是若干个 `CPrimitive` 的集合。

它的职责是：

- 把多个 primitive 组织成一个 mesh
- 提供 mesh 级别 AABB
- 作为被多个节点复用的去重资源

#### `CRenderable`

`CRenderable` 只保存一个 `mesh_entt`。

它的作用是：

- 一个场景节点可以通过 `CRenderable` 引用一个 `CMesh`
- 多个实体可以共享同一个 mesh
- instance 的差异主要体现在各自的 transform 上，而不是重复存几何数据

### 1.2 GPU 中的对应数据类型

逻辑层进入渲染层后，会被压缩成 GPU 更容易消费的结构体。它们定义在 [source/runtime/render/shaderheaders/shared/scene/SharedSceneStruct.h](../../source/runtime/render/shaderheaders/shared/scene/SharedSceneStruct.h)。

#### `GPrimitive`

`GPrimitive` 和 `CPrimitive` 一一对应。

它包含：

- local-space AABB
- material index
- attribute mask
- position / normal / tangent / uv / index 的 buffer 起始偏移

你可以把它理解为：

**“这个 primitive 的几何数据在大缓冲里从哪里开始取，以及它的局部包围盒是什么。”**

#### `GInstance`

`GInstance` 和逻辑层的一个可渲染节点实例对应。

它包含两个核心信息：

- `world_transform`
- `primitive_id`

其中 `primitive_id` 很关键。当前实现里，它不仅表示“这个 instance 对应哪个 primitive”，还承担了一个更强的约束：

**它默认把 primitive_id 当成了 draw index 的反向索引。**

这意味着当前很多渲染阶段都隐含依赖下面这个事实：

> 一个 draw command 对应一个 primitive，instance 只是在这个 primitive 下面重复绘制。

### 1.3 当前绘制流程

这一段建议把它看成一条数据流水线：

`LogicalScene -> CpuScene -> GpuScene -> GPU Culling -> GeometryPass`

#### 第一步：导入到 LogicalScene

当前场景导入入口在 [source/runtime/render/scene/loader/LoaderInterface.cpp](../../source/runtime/render/scene/loader/LoaderInterface.cpp)，主解析器是 Assimp 路径 [source/runtime/render/scene/loader/assimp/Parser.cpp](../../source/runtime/render/scene/loader/assimp/Parser.cpp)。

这一阶段主要是把文件格式中的 mesh / primitive / material / node，转成 LogicalScene 里的 ECS 组件。

#### 第二步：LogicalScene 转 CpuScene

这一层是当前 cluster 话题最关键的一层，代码在 [source/runtime/render/scene/CpuScene.cpp](../../source/runtime/render/scene/CpuScene.cpp) 和 [source/runtime/render/scene/CpuScene.h](../../source/runtime/render/scene/CpuScene.h)。

CpuScene 里最重要的三个数组是：

- `m_draw_cmd_buf`：和 `GPrimitive` 一一对应
- `m_primitive_buf`：和 `GPrimitive` 一一对应
- `m_instance_buf`：多个 instance 共享同一个 primitive

也就是说，当前关系是：

- `draw_cmd_buf` : `primitive_buf` = 1 : 1
- `instance_buf` : `primitive_buf` = N : 1

CpuScene 的构建流程大致是：

1. 先遍历所有 `CPrimitive`，生成 `GPrimitive`
2. 再遍历场景树，把每个 renderable 节点展开成若干 `GInstance`
3. 按 primitive 把这些 instance 分组
4. 最后为每个 primitive 生成一个 `DrawIndexedCmdData`

所以当前 draw command 的语义是：

> “把这个 primitive 画出来，并且画它对应的全部 instance。”

#### 第三步：CpuScene 转 GpuScene

在 [source/runtime/render/scene/GpuScene.cpp](../../source/runtime/render/scene/GpuScene.cpp) 中，这些 CPU 数组被上传到 GPU Buffer。

上传后，GPU 看到的就是：

- 一份 primitive buffer
- 一份 instance buffer
- 一份 draw command buffer

#### 第四步：GPU Culling

当前 raster culling 在 [source/runtime/render/renderer/raster/CullingPass.h](../../source/runtime/render/renderer/raster/CullingPass.h)。

它的输入可以简化理解为：

- source draw commands
- primitive buffer
- instance buffer

它的输出主要是：

- `visible_instance_ids`
- 裁剪后的 draw command buffer

注意这里一个非常重要的点：

当前可见性 remap 的对象是 **instance id**，不是 primitive id，也不是 cluster id。

#### 第五步：Geometry Pass

Geometry Pass 在 [source/runtime/render/renderer/raster/GeometryPass.h](../../source/runtime/render/renderer/raster/GeometryPass.h)。

这一阶段会拿到：

- primitive buffer handle
- instance buffer handle
- visible instance id buffer handle

如果启用 GPU culling，shader 会通过 `visible_instance_id_buf` 对 `SV_InstanceID` 做一次 remap，再从 `GInstance` 里读出 `primitive_id`，然后去访问对应 `GPrimitive` 的几何数据。

所以当前真正的执行模型是：

1. 先决定哪些 instance 可见
2. 然后通过 instance 反查 primitive
3. 再由 primitive 定位顶点/index 数据

### 1.4 用一句话总结当前结构

当前系统不是“纯 instance 绘制”，也不是“纯 primitive 绘制”。

更准确的描述是：

**Primitive 负责定义 draw 的几何内容，Instance 负责定义这个几何内容被摆放到哪里。**

这正是 cluster 优化为什么不能只停留在“导入时把 primitive 切碎”这一步的原因。

---

## 2. 一个最直接的过渡方案，以及它的问题

这一章只讲一个很直观的过渡方案。

这个方案是：

**把 cluster 当作 primitive，其他运行时结构全部不改，只在 load scene 时把 primitive 切细。**

### 2.1 过渡方案：只在加载时切分 Primitive

这个方案的思路非常直接。

当前系统已经完整支持：

- `CPrimitive -> GPrimitive`
- 每个 primitive 对应一个 draw command
- 每个 instance 通过 `primitive_id` 反查几何数据

那么最省事的做法就是：

1. 在 load scene 的时候，把一个原始 primitive 切成多个 cluster
2. 但运行时不要引入新的 `Cluster` 概念
3. 而是把每个 cluster 直接当成一个新的 primitive
4. 后面的 `CpuScene / GpuScene / Culling / GeometryPass` 全部按现有 primitive 流程继续走

这样做的好处很明显：

- 代码改动面最小
- 现有 draw command 流程可以直接复用
- 现有 culling 和 geometry pass 主体都不用重写

从运行时视角看，这个方案相当于：

> 引擎根本不知道“cluster”这个词，它只会觉得 scene 里 primitive 变多了。

也就是说，cluster 只存在于导入阶段；一旦进入运行时，这些 cluster 就全部伪装成 primitive。

这是一个合理的第一版方案，也确实适合拿来做 POC 或 profile。

### 2.2 这个方案的问题在哪里

这个方案最大的问题，不是“不能跑”，而是它把问题藏起来了。

第一，**primitive 的语义被污染了**。

原本 primitive 表示导入后的一个逻辑几何片段。采用这个过渡方案后，运行时的 primitive 已经不再是原始 primitive，而是“被切碎后的 cluster”。名字没变，但含义变了。短期可以接受，长期会越来越难维护。

第二，**draw command 和相关 buffer 会明显膨胀**。

原来一个 primitive 对应一个 draw command。切成很多 cluster 之后，就会多出很多“伪 primitive”，于是：

- primitive buffer 变大
- draw command buffer 变大
- culling 的遍历对象变多

cluster 粒度变细，确实可能提升裁剪精度；但 draw 管理成本也会同步上升。

第三，**primitive_id 在其他系统里也是基础索引**。

现在 primitive_id 不只给 raster draw 用，像 [source/runtime/render/renderer/raytracing/PreprocessLightPass.cpp](../../source/runtime/render/renderer/raytracing/PreprocessLightPass.cpp) 这样的链路，也默认 primitive_id 是稳定的基础编号。于是“把 cluster 伪装成 primitive”虽然看起来只改了加载阶段，实际上会影响一整条运行时索引语义。

第四，**最容易走偏的补丁，是让 instance 去保存 cluster id 数组，而这是非常不对的。**

很多人看到这里，会自然想到一个补丁：

- primitive 还是那个 primitive
- 但给每个 instance 多挂一个 cluster id 数组
- 这样 instance 就知道自己要画哪些 cluster 了

这个补丁的问题很严重。

它会带来两件最糟糕的事：

- 高耦合：instance 本来只该描述 transform 和对几何资源的引用，现在却要直接依赖“primitive 被如何切分”
- 高冗余：同一个 primitive 的 cluster 列表，本来应该是共享的静态几何信息，却会被重复复制到每一个 instance 上

举个最直接的例子：

- 一个 primitive 被切成 64 个 cluster
- 一个树模型在场景里被实例化 1000 次

如果 cluster id 数组挂在 instance 上，那么同一份 64 个 cluster 的信息就会被重复存 1000 遍。这不只是内存浪费，更意味着以后 cluster 重新划分时，这 1000 份 instance 数据都要跟着调整。

所以这一章真正想说明的是：

**“把 cluster 当作 primitive”可以作为过渡方案；但一旦为了修补这个方案，又把 cluster 列表挂到 instance 身上，就说明设计已经走偏了。**

---

## 3. 正规方案：把“可见性单位”升级为 Instance x Cluster

这一节讲推荐方案。

核心思想是：

**不要再让 instance 直接反查 primitive 作为唯一绘制单位，而是显式引入 cluster 作为新的渲染子单元。**

### 3.1 先说结论

正规方案里：

- LogicalScene 层可以基本不动
- `CPrimitive` 依然表示导入后的逻辑几何片段
- **cluster 只进入渲染缓存层，不污染 ECS 语义**
  - 【重点，只和Render有关，和LogicalScene完全解耦。和CpuScene/GpuScene还是有关系的，需要缓存数据】

- 运行时的可见性单位从“instance of primitive”升级为“instance of cluster”

可以把它理解成：

- `Primitive`：逻辑资源边界
- `Cluster`：渲染优化边界

两者职责不同，不必强行合并成一个概念。

### 3.2 正规方案的数据流

推荐的数据流如下：

1. 导入模型，仍然先生成 `CPrimitive`
2. 在 LogicalScene -> CpuScene 的过程中，或在其之前的渲染资源准备过程中，对每个 primitive 做 cluster 切分
3. 为每个 cluster 生成独立的 cluster metadata 和 cluster bound
4. 保留 instance buffer，但 instance 不再直接“决定几何范围”
5. 新增 instance-cluster 可见性结果 buffer
6. Geometry Pass 根据“可见的 instance-cluster 对”来发起绘制

此时真正的绘制最小单位就变成了：

**instance x cluster**

### 3.3 相比当前方案，需要改变什么？

相比现在的 primitive-instance 模型，正规方案至少有四个关键变化。

#### 变化 1：新增 Cluster 层数据，而不是偷改 Primitive 含义

当前：

- `primitive_buf`
- `instance_buf`
- `draw_cmd_buf`

正规方案会新增：

- `cluster_buf`
- `cluster_bound_buf`
- `primitive_to_cluster_range`

这样 primitive 仍然是 primitive，cluster 也有自己的正式身份。

#### 变化 2：可见性输出不再只是 `visible_instance_ids`

当前 culling 输出的是“哪些 instance 可见”。

正规方案更合理的输出是：

- `visible_instance_cluster_pairs`

比如每个元素是：

- `instance_id`
- `cluster_id`

或者：

- `instance_id`
- `primitive_id`
- `cluster_local_id`

这样 geometry pass 才能真正做到按 cluster 绘制。

#### 变化 3：Geometry Pass 不能再只靠 `SV_InstanceID -> GInstance -> primitive_id`

当前的 geometry shader 路径默认是：

`SV_InstanceID -> visible_instance_id -> GInstance -> primitive_id -> GPrimitive`

正规方案需要改成：

`SV_InstanceID / draw index -> visible pair -> GInstance + GCluster -> 顶点范围`

也就是说，geometry pass 要显式消费 cluster 信息。

#### 变化 4：draw command 的语义要重新定义

当前 draw command 代表“一个 primitive 的全部 instance”。

正规方案里，draw command 更适合代表：

- 一个 cluster 的绘制模板
- 或者一批同类 cluster 的间接绘制任务

它不应该再默认等价于 primitive。

### 3.4 正规方案的优势

正规方案的优势主要有四个。

#### 优势 1：语义干净

primitive 继续表示逻辑几何片段。

cluster 只表示渲染优化用的更细粒度单元。

这样代码会更好理解，也更容易维护。

#### 优势 2：扩展性强

以后如果要做：

- cluster LOD
- meshlet cone culling
- hierarchical culling
- cluster streaming

都可以继续沿着 cluster 层扩展，而不必回头拆 primitive 语义。

#### 优势 3：能更精准地控制开销

如果把 cluster 伪装成 primitive，很多旧结构会被动膨胀。

正规方案则可以更明确地决定：

- 哪些 buffer 按 primitive 建
- 哪些 buffer 按 cluster 建
- 哪些结果按 instance-cluster 建

这样更容易调节性能和显存占用。

#### 优势 4：更适合跨系统协同

当前 primitive_id 已经被 raster、RT、light preprocess 等系统共享使用。

正规方案把 cluster 作为新增层，而不是替换 primitive，可以显著降低对现有系统的破坏性。

---

## 4. 如何实现：建议修改哪些代码

这一节偏工程实现，按“先后顺序”来写。

### 4.1 第一阶段：保留 Logical Component，不改 ECS 语义

这里建议**不修改** [source/runtime/render/scene/LogicalComponents.h](../../source/runtime/render/scene/LogicalComponents.h)。

原因是：

- `CPrimitive` 现在的职责很清晰
- cluster 是渲染优化数据，不一定要进入 ECS
- 先把 cluster 放在 CpuScene/GpuScene 侧，更符合当前架构

也就是说，逻辑层继续保持：

- `CRenderable -> CMesh -> CPrimitive`

### 4.2 第二阶段：在 CpuScene 中新增 Cluster 渲染缓存

重点文件：

- [source/runtime/render/scene/CpuScene.h](../../source/runtime/render/scene/CpuScene.h)
- [source/runtime/render/scene/CpuScene.cpp](../../source/runtime/render/scene/CpuScene.cpp)

建议新增的 CPU 侧数据包括：

- `Array<GCluster> m_cluster_buf`
- `Array<GClusterBound> m_cluster_bound_buf`
- `Array<uint2> m_primitive_to_cluster_range`
- `Array<DrawIndexedCmdData> m_cluster_draw_cmd_buf`

如果后续想做更正规的 instance-cluster 可见性，还可以再加：

- `Array<VisibleInstanceClusterPair>` 的 CPU staging 结构

这一层要做的事情是：

1. 在构建 `GPrimitive` 后，为每个 primitive 切分 cluster
2. 记录每个 primitive 对应的 cluster 范围
3. 生成 cluster 级的 draw template
4. 不要再把 draw command 默认视为 primitive 的镜像

### 4.3 第三阶段：扩展 SharedSceneStruct，正式引入 Cluster 结构

重点文件：

- [source/runtime/render/shaderheaders/shared/scene/SharedSceneStruct.h](../../source/runtime/render/shaderheaders/shared/scene/SharedSceneStruct.h)

建议新增：

- `struct GCluster`
- `struct GClusterBound`
- `struct VisibleInstanceClusterPair`

一个可行的 `GCluster` 至少应包含：

- `parent_primitive_id`
- `index_start_idx`
- `index_count`
- 可选的 `vertex_start_idx` / `vertex_count`
- 可选的锥体剔除辅助数据

`GClusterBound` 至少应包含：

- local-space AABB 或 sphere
- 可选的 cone 信息

### 4.4 第四阶段：扩展 GpuScene 上传资源

重点文件：

- [source/runtime/render/scene/GpuScene.h](../../source/runtime/render/scene/GpuScene.h)
- [source/runtime/render/scene/GpuScene.cpp](../../source/runtime/render/scene/GpuScene.cpp)

需要新增上传和 bindless 分配：

- cluster buffer
- cluster bound buffer
- cluster draw command buffer
- visible instance-cluster pair buffer

这一层的目标是把 cluster 相关数据正式纳入 `GpuScene::Resource`，让 raster pass 能像访问 primitive/instance 一样访问 cluster。

### 4.5 第五阶段：改 GPU Culling，从“筛 instance”变为“筛 instance-cluster”

重点文件：

- [source/runtime/render/renderer/raster/CullingPass.h](../../source/runtime/render/renderer/raster/CullingPass.h)
- [source/runtime/render/shaderheaders/shared/raster/culling/ShaderParameters.h](../../source/runtime/render/shaderheaders/shared/raster/culling/ShaderParameters.h)
- [source/runtime/render/renderer/raster/RasterGpuCullingResource.h](../../source/runtime/render/renderer/raster/RasterGpuCullingResource.h)

当前 culling 输出是：

- `visible_instance_ids`

建议改为：

- `visible_instance_cluster_pairs`

这意味着 culling shader 逻辑要从：

- 遍历 draw / primitive
- 检查该 draw 下哪些 instance 可见

变成：

- 遍历 primitive 对应的 cluster
- 对每个 cluster 计算局部 bound
- 用 instance 的 world transform 把 cluster bound 变换到世界空间
- 生成可见的 instance-cluster 对

如果担心一步改太大，也可以先做两级 culling：

1. 先做 primitive / mesh 级粗裁剪
2. 仅对通过粗裁剪的 primitive，再做 cluster 级细裁剪

### 4.6 第六阶段：改 Geometry Pass，让它真正消费 Cluster

重点文件：

- [source/runtime/render/renderer/raster/GeometryPass.h](../../source/runtime/render/renderer/raster/GeometryPass.h)
- [source/runtime/render/shaderheaders/shared/raster/geometry_pass/ShaderParameters.h](../../source/runtime/render/shaderheaders/shared/raster/geometry_pass/ShaderParameters.h)
- 对应 geometry shader / vertex shader 的 HLSL 文件

当前 geometry pass 只知道：

- instance buffer
- primitive buffer
- visible instance id remap

要升级成正规 cluster 方案，需要让它还能读到：

- cluster buffer
- visible instance-cluster pair buffer

这样 shader 才知道：

- 当前变换矩阵来自哪个 instance
- 当前要取哪一段 index / vertex 数据来自哪个 cluster

这一改动是正规方案最关键的一步。

### 4.7 第七阶段：检查所有依赖 primitive_id 的旁路系统

重点文件至少包括：

- [source/runtime/render/renderer/raytracing/PreprocessLightPass.cpp](../../source/runtime/render/renderer/raytracing/PreprocessLightPass.cpp)
- [source/runtime/render/scene/GpuScene.cpp](../../source/runtime/render/scene/GpuScene.cpp)

这里的目标不是让 RT 立即支持 cluster，而是先确认：

- 哪些地方必须继续用原始 primitive_id
- 哪些地方可以进一步细化到 cluster
- 哪些地方要加映射，避免 primitive_id 语义被污染

我的建议是：

- 第一版 cluster 优化先只改 raster path
- RT / light preprocess 暂时继续以 primitive 为单位
- 用 `parent_primitive_id` 把两套系统接起来

这样改动面更可控。

---

## 5. 未来可改进方向与后续 TODO

这一节列出推荐的后续路线，按优先级排序。

### 5.1 优先级最高：先做一个“只影响 Raster”的第一版

推荐先完成：

- cluster 数据结构落地
- cluster 级 culling
- geometry pass 能按 cluster 绘制

先不要一上来同步改 RT、光源预处理、缓存系统全部分支。

原因很现实：

- 这样最容易验证 cluster culling 的真实收益
- 出问题时最好排查
- 改动范围更可控

### 5.2 增加调试视图

建议后续加几种 cluster 调试视图：

- cluster AABB 可视化
- 每个 primitive 被切成多少 cluster 的可视化
- cluster culling 前后数量统计
- 屏幕上显示 visible primitive 数 / visible cluster 数

没有这些调试手段，cluster 优化会很难调参。

### 5.3 保留 primitive 级粗裁剪

不要把 cluster 级裁剪理解为“彻底替代 primitive 级裁剪”。

更合理的方向通常是两级：

1. 先用 mesh / primitive 做粗裁剪
2. 再对幸存对象做 cluster 级细裁剪

这样通常比纯 cluster 全量遍历更划算。

### 5.4 未来可以加入更强的 cluster bound

第一版只用 AABB 就够了。

后续可以考虑补充：

- sphere
- cone culling 数据
- normal cone
- backface cluster rejection

这类信息会让 cluster culling 更强，但不建议一开始就做太满。

### 5.5 后续可以继续做层级化

如果第一版 cluster culling 证明收益明显，下一阶段可以考虑：

- cluster hierarchy
- meshlet tree
- streaming-friendly cluster page
- LOD / HLOD 联动

但这些都应该建立在“平面 cluster 方案已经跑通、且收益明确”的基础上。

### 5.6 一个务实的 TODO 清单

建议按下面顺序推进：

1. 保持 LogicalScene 不变
2. 在 CpuScene / SharedSceneStruct / GpuScene 中正式引入 Cluster 结构
3. 让 Raster Culling 输出 `visible_instance_cluster_pairs`
4. 让 Geometry Pass 消费 cluster 数据并完成绘制
5. 添加 cluster 调试视图和统计信息
6. 用 Sponza / 大量重复实例场景做 profile，对比 primitive culling 与 cluster culling
7. 确认收益后，再决定是否把 RT / light preprocess 一起升级

---

## 总结

这份方案的核心观点只有三句。

第一，当前 MoerEngine 的运行时绘制并不是简单的“instance culling”，而是一个以 primitive 为 draw 基础、以 instance 为可见性索引的系统。

第二，直接把 cluster 伪装成 primitive 可以作为过渡方案或验证方案，但它会污染 primitive 的语义，并把很多运行时索引问题偷偷转移到别处。

第三，更正规的做法是：**保持 LogicalScene 中的 primitive 语义不变，在渲染缓存层（CpuScene/GpuScene）显式引入 cluster，把真实的可见性和绘制单位升级为 instance x cluster。**

这条路改动更大，但结构更干净，也更适合 MoerEngine 未来继续做更细粒度的 GPU Driven 渲染优化。