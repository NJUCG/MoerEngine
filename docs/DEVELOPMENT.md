# 开发手册 与 规范

## 目录

- [1. 推荐使用的开发工具](#1-推荐使用的开发工具)
  - [just](#just)
- [2. 设计思路](#2-设计思路)
  - [从真实约束推导设计](#从真实约束推导设计)
  - [ECS框架](#ecs框架)
- [3. 规范](#3-规范)
  - [Git规范](#git规范)
  - [第三方库与依赖项](#第三方库与依赖项)
  - [C++规范](#c规范)
  - [HLSL命名规范](#hlsl命名规范)
  - [CMake命名规范](#cmake命名规范)
- [4. IDE \& IntelliSence配置](#4-ide--intellisence配置)
  - [VSCode配置相关](#vscode配置相关)
- [5. 如何Code Review](#5-如何code-review)

## 1. 推荐使用的开发工具

### just

`just` 可以简单理解为 **快捷命令**。

在本仓库中，`template.justfile` 中预先写好了一些常用命令（例如编译、运行、格式化、打包等），你只需要记住少量入口命令，就可以避免每天和一长串 CMake / Ninja / MSVC 参数搏斗。

推荐的使用方式大致如下：

1. 安装 `just`
   - Windows：可以通过 `cargo` 安装（`cargo install just`）。
   - Linux / macOS：参考官方仓库 `casey/just` 的 README 即可。【安装细节请自行询问AI】
2. 通过简短命令完成日常操作
   - 例如：`just build`、`just run`、`just gbr`（实际命令以 `justfile` 中为准）。

总之，**把常用、复杂、容易忘的命令都写在 `justfile` 里，然后只记住几个简短的 `just xxx` 就好**，既减少心智负担，也方便团队成员共享统一的开发流程。

## 2. 设计思路

### 从真实约束推导设计

设计应当能从少数真实约束推导出来：每个状态有明确含义，每个机制有具体责任，新增复杂度能说明来源和收益。阅读、解释或重构系统时，都应追问：

> 我们是在解决原本存在的问题，还是在不断解决上一个抽象引入的问题？

先区分复杂度的来源：API、硬件的强制要求；业务、并发和资源生命周期的实际要求；实现自行选择的数据组织、缓存、封装和性能优化。第三类可以有价值，但需要说明取舍，不能当成底层强制要求，也不能仅凭 UE 等成熟引擎采用过就认定必要。

解释一个机制时，按以下顺序展开：

1. **真实约束是什么？** 给出具体场景和依据。删除机制后，究竟会违反什么条件？“调用方当前依赖它”只是依赖关系，还需要继续追问调用方为何这样设计。
2. **满足约束的最简可行模型是什么？** 先讲清数据、所有权、有效期和操作顺序，再映射到当前类型与函数。不要只复述调用链来解释必要性。
3. **当前实现额外选择了什么？** 说明新增状态、缓存、包装层或同步逻辑解决的具体问题、引入的维护责任，以及放弃它的代价。性能收益应区分实测结果与推测。
4. **复杂度应该放在哪里？** 检查能否复用已有机制，是否适合由更粗的生命周期边界承担，或者是否遗漏了必须独立处理的约束。

| 判断 | 处理方式 | 需要确认的条件 |
|---|---|---|
| 已有机制能够承担相同责任 | 合并重复状态、接口或流程，保留明确的信息来源 | 现有机制的语义与覆盖范围确实匹配 |
| 细粒度管理并非必要 | 将分配、回收或同步移到更粗的生命周期边界 | 该边界覆盖所有使用者，且资源占用、等待等代价可接受 |
| 此前遗漏了真实约束 | 补充必要的状态或机制，并写清它维护的条件 | 能指出具体失败场景，已有机制无法正确覆盖 |

对每个需要长期保存的状态，应能回答：它表示什么、谁拥有并更新它、何时有效或失效、何时回收、能否从已有状态推导。保存派生数据时，应说明缓存收益以及如何保持一致。

函数名和变量名应体现责任与含义，函数抽取应呈现清楚的处理步骤。仅仅把复杂代码搬进更多辅助函数，不能证明架构变简单。评估重点是需要独立理解和维护的概念、状态及不变量是否减少，而不是类或函数的数量是否减少。

### ECS框架

#### ECS是什么？

暴雪在2017年GDC分享了一个讲座[《守望先锋的游戏架构和网络代码》](https://www.bilibili.com/video/BV1p4411k7N8)，从这个讲座之后，ECS开始变得广为人知。

【ECS的概念，请自行询问AI】

接下来的内容，主要是笔者(YXH咸鱼)对ECS框架的理解，避免AI出现遗漏。

笔者(YXH咸鱼)认为，ECS主要有以下三个核心作用：

1. Cache友好
2. 适合高度并行
3. 数据与逻辑解耦

**Cache友好**：理想情况下，ECS框架中所有需要处理的数据都是排布在一起的。我们可以用相同的逻辑处理一大块连续内存，可以极大提高cache命中率。打过算法竞赛的同学都知道，经典算法ST表，最极端的情况下，Cache友好的ST表 耗时可以达到 烂实现 的十分之一，也就是1000%的性能提升。因此，在极端情况下，Cache友好非常重要。

**适合高度并行**：在ECS的逻辑下，System只会被允许操作一部分Component，并且拥有显式的读写声明。这就使得可以通过一个任务调度器来自动化并行所有System。这一部分，[bevy engine](https://github.com/bevyengine/bevy) 实现的特别优秀！

**数据与逻辑解耦**：

这是笔者认为ECS框架最核心、**也最容易被遗漏的一点**。

暴雪的讲座提出，Component不应该包含任何逻辑、System不应该持有任何数据。

例如，我们有一个Mesh Component，那么这个Component就只能存数据，不能存任何关于Mesh的处理逻辑（例如加载Mesh、计算Mesh哈希值）。对应的，我们应该在 `SystemCalcMeshHash()` 中统一计算所有Mesh的哈希，或者通过传入Entity索引来计算单个Mesh的哈希。在System中，如果出现了需要复用代码的情况，那么我们应该把逻辑抽象成工具函数。

这种做法的目的就是，**保证函数之间没有循环引用**。这是我个人认为的ECS一大优势之一，但是在具体实现时经常被遗漏。

> 另外，这个做法实际上就是 *面向数据编程*。

#### ECS的实现

ECS具体在实现上，有许多不同的设计思路。此处介绍一个最核心的区别 **Archetype**。

【Archetype的概念请自行询问AI】

#### MoerEngine的ECS

MoerEngine采用 `EnTT` 库作为ECS框架。`EnTT` 默认采用非Archetype，但支持通过 `Group` 分组优化，来实现Archetype。

MoerEngine目前只在场景表示（`LogicalScene`）中使用了ECS框架。

`LogicalData.h` 中存储了Component的定义。

`LogicalScene.h/cpp` 中存储了System的定义。

在ECS开发中，笔者更喜欢扁平化划分代码：按照功能划分代码，而不是按照对象划分逻辑，即 **面向数据编程（DOP），而不是面向对象编程（OOP）**。下面是具体的例子：

```c++
// 面向数据编程 DOP【笔者喜欢的】
void Update(Registry& r) {
    r.view<ComponentA&>.each([](){
        // do something for ComponentA
    });
    r.view<ComponentB&>.each([](){
        // do something for ComponentB
    });
    r.view<ComponentC&>.each([](){
        // do something for ComponentC
    });
}

void CacheToDisk(Registry& r) {
    r.view<const ComponentA&>.each([](){
        // do something for ComponentA
    });
    r.view<const ComponentB&>.each([](){
        // do something for ComponentB
    });
    r.view<const ComponentC&>.each([](){
        // do something for ComponentC
    });
}

// 面向对象编程 OOP【传统】
class ComponentA {
    void Update();
    void CacheToDisk() const;
}

class ComponentB {
    void Update();
    void CacheToDisk() const;
}

class ComponentB {
    void Update();
    void CacheToDisk() const;
}
```

在阅读已有代码的时候，你可以发现非常多类似的写法。重构后，新的场景表示，基本上都是以面向数据的方式组织代码的。

【这种编码方式的优缺点，请自行询问AI】

## 3. 规范

以下命名规范均为推荐规范，非强制要求。但请尽量遵循，以保持代码风格的一致性和可读性。

### Git规范

如果你是NJU META的成员（即 拥有仓库读写权限），你可以直接在MoerEngine仓库中创建对应分支进行开发。推荐的分支命名格式为 `feature/xxx` 或 `bugfix/xxx`。同时，也请创建分支对应的Draft PR，以告知其他开发者你正在开发的内容。

Commit信息，请遵循 [Conventional Commits](https://www.conventionalcommits.org/zh-hans/v1.0.0/) 规范。

### 第三方库与依赖项

引入第三方库时，请检查对应的协议。如果是MIT、Apache等宽松协议，可以直接引入。否则，请先和维护者沟通。

此外，**请务必检查头文件是否有额外要求！**例如，`volk` 要求include头文件前定义平台相关宏、`NRD` 要求特定的头文件include顺序。这种情况下，请 **务必创建一个单独的头文件来封装该依赖库的include逻辑**，否则会导致其他开发者错误的include行为。

Vulkan 后端统一通过 `rhi/backends/vulkan/VulkanCommon.h` 获取 Vulkan 声明。Render 模块内不要直接 include `<volk.h>`、`<vulkan/vulkan_core.h>` 或平台 Vulkan 头文件；公开或消费 Vulkan 类型的 CMake target 应链接 `Moer::VulkanHeaders`，由它传递 `VK_NO_PROTOTYPES`、平台宏、头文件路径和 Volk 依赖。`source/profile/VkLayer.cpp` 是独立 Vulkan Layer，不受 Render 模块规则约束。修改该契约后，应构建 `TestVulkanHeaderContract` 并运行名为 `VulkanHeaderContract` 的 CTest。

### C++规范

MoerEngine开发早期使用 `.clang-tidy` 对命名风格进行规范，但强制性较弱，导致风格不统一，历史遗留问题大。

因此，**这里的规范从 2026.1.8 启用**。此前代码暂不做更新，等到以新命名规范的代码数量超过旧代码，再考虑统一修改。

此处规范和先前规范的主要区别在于：形参不应该以 `_` 开头；非POD成员变量应该以 `m_` 开头。此文档优先级高于 `.clang-tidy`

#### 固定宽度整数

引擎自有 C++ 代码统一使用 `misc/Types.h` 中的 `uint8 / uint16 / uint32 / uint64` 和 `int8 / int16 / int32 / int64`。这些名称直接别名到 `<cstdint>` 的对应类型；固定宽度原子整数写作 `std::atomic<uint32>` 等。命名空间外使用 `Moer::uint32` 等限定名。

`size_t`、`uintptr_t`、`VkDeviceSize` 等具有具体用途的类型保留原名。第三方和生成代码沿用原有约定；CUDA 互操作边界保留其类型契约。HLSL 与双语言共享头继续使用 `uint`，C++ 侧兼容别名由 `misc/Traits.h` 提供。新增纯 C++ 代码使用 `uint32`。

#### 命名规范 - 变量

**变量均为 全小写 & 下划线 风格**。

| 类别                   | 命名风格                               | 示例                                                |
| ---------------------- | -------------------------------------- | --------------------------------------------------- |
| **局部变量**           |                                        | `shadow_map_texture`, `pso_full_screen_info`        |
| **形参**               |                                        | `context`, `input_image`                            |
| **成员变量**           | `m_` 开头                              | `m_renderer`, `m_buffer_handle`                     |
| **常量**               | `k_` 开头                              | `k_fov_default`, `k_camera_speed_up_delta`          |
| **Bool**               | `is_` / `b_` / `has_` / `should_` 开头 | `is_shadow_enabled`, `is_dirty`, `m_is_initialized` |
| **Bindless Handle**    | `_hdl` 结尾                            | `albedo_map_hdl`, `output_tex_hdl`                  |
| **entt::Entity**       | `_entt` / `entity` 结尾                | `albedo_map_entt`, `material_entt`, `entity`        |
| **数组下标**           | `_id` 结尾                             |                                                     |
| **数量**               | `num_` / `_count` / `_cnt` 标记        | `mip_level_count`, `mip_level_cnt`, `num_of_images` |
| **用于输出的引用形参** | `out_` 开头                            | `out_param`                                         |

**成员变量** 补充：如果一个类型为POD，即没有任何方法，则 **其成员变量不应该以 `m_` 开头**！如 `RasterConfig`。

**数量** 补充：如果不加count等限定，会让其他开发者感到十分疑惑。好的例子：`mip_level_count`；坏的反例：`mips`。其他开发者看到 `mips`，完全不知道这里表示的是 ① Texture对应第`mips`层mip 还是 ② 总共有`mips`层mipmap层数。

#### 命名规范 - 其他

| 类别         | 命名风格                                                 | 示例                                          |
| ------------ | -------------------------------------------------------- | --------------------------------------------- |
| **类型**     | 驼峰                                                     | `RasterRenderer`, `Scene`                     |
| **函数**     | 驼峰                                                     | `EmplaceOrReplace()`, `ToString()`            |
| **枚举类型** | `E` 开头 & 驼峰                                          | `EShadowMapMode`                              |
| **枚举值**   | 驼峰（历史遗留导致目前大量 全大写&下划线，请尽量用驼峰） | `EAoMode::None`, `EAoMode::RtaoAoOnly`        |
| **宏定义**   | 全大写 & 下划线                                          | `WITH_CUDA`, `RENDER_API`                     |
| **命名空间** | 通常只需要Moer（尽量简化，避免复杂）                     | namespace Moer {}; namespace Moer::Render {}; |

枚举补充：强制使用 `enum class`，不应该使用 `enum`。

#### 开发过程中推荐使用的第三方库

* [EnTT](https://github.com/skypjack/entt)：MoerEngine ECS框架的核心，可以参考示例 `source/test/EnTTTest.cpp`
* [gtl](https://github.com/greg7mdp/gtl)：工具类模板库，提供了一系列线程安全的容器，例如 `gtl::parallel_flat_hash_map`

### HLSL命名规范

Shaders文件夹架构及相关规范见`/shaders/README.md`。

### CMake命名规范

| 更多操作类别  | 命名风格        | 示例                                     |
| ------------- | --------------- | ---------------------------------------- |
| 指令/函数     | 全小写          | add_executable(...), set(...)            |
| 关键字        | 全大写 & 下划线 | STATIC, GLOB_RECURSE, PRIVATE            |
| 局部变量      | 全小写 & 下划线 | set(my_source_files ...), libtorch_dir   |
| 全局/缓存变量 | 全大写 & 下划线 | option(WITH_CUDA), set(LIBTORCH_DIR ...) |
| 目标(Target)  | 命名空间 & 驼峰 | Moer::Render, CUDA::cudart               |

* 注：目标(Target)可以参考以下形式

  ```cmake
  # Moer::Render
  set(target_name moer_render)

  add_library(${target_name} SHARED ...)
  add_library(Moer::Render ALIAS ${target_name})

  # Moer::Cuda
  set(target_name moer_cuda)
  add_library(${target_name} SHARED ${moer_cuda_h} ${moer_cuda_cu})
  add_library(Moer::Cuda ALIAS ${target_name})
  ```

## 4. IDE & IntelliSence配置

*TODO*

### VSCode配置相关

- 设置中的 `C_Cpp.default.compilerPath` 字段不能使用msvc编译器，否则IntelliSense会出现假错。推荐使用clang
  - 注：和编译无关，只和 IntelliSense（IDE的智能代码高亮与补全）有关

## 5. 如何Code Review

这里贴一下Code Review时可以注意的内容：

1. 是否可以正常编译与运行，并且没有错误或警告
   * 如果修改涉及构建系统，或者改动幅度较大，需要完整测试一下多插件情况下的编译与运行（例如启用NRD、CUDA拓展）
2. 是否重复造轮子（例如重复实现一份代码里已有的工具）
   * 这通常是由于开发手册（即本文档）中没有详细介绍工具类导致的。出现这种情况的话，需要补充本文，并且告知开发者对应工具类的存在
3. 是否存在不规范的写法
   * 例如，函数形参忘加引用、子类的虚函数不添加virtual标记、类的特殊函数没有遵守零三五法则、没有禁用一些类的拷贝构造&拷贝赋值函数、没有考虑变量生命周期（滥用SharedPtr）
4. 是否有补充必要的注释
   * 有没有用什么设计模式？有没有什么容易出错的地方？有没有什么多线程相关内容？
   * etc...
5. 命名是否存在歧义或者不规范

## 6. RenderDoc GPU Marker

渲染 pass、GPU 计时区间和帧提交标签应遵循统一层级、颜色与命名约定。添加或修改 marker 前请阅读 [RenderDoc GPU Marker 指南](RENDERDOC.md)，尤其注意 timestamp key 的单次提交唯一性和 marker 对 RHI 命令重排边界的影响。

## 7. RHI 顶点格式

顶点声明和光追几何的 `vertex_format` 使用 `EVertexFormat`（`rhi/VertexFormat.h`），纹理和 texel buffer 使用 `EPixelFormat`。两者不能隐式转换，避免将深度、压缩或 sRGB 像素格式误用于顶点输入。

`Float1`～`Float4` 表示 32 位浮点分量，`UInt` 表示一个 32 位无符号整数，`UByte4Normalized` 表示四个 8 位无符号分量，读取时归一化到 `[0, 1]`。通过 `GetVertexFormatByteSize()` 计算顶点属性的字节数，由 Metal / Vulkan 各自映射到原生顶点格式；新增格式时同步补充这两处映射。

## 8. RDG 编译执行与资源边界

GPU 真正要求的是访问顺序、内存可见性、图像布局、队列所有权与使用期间的资源存活。
Vulkan 渲染路径由 RDG 根据显式声明生成这些依赖，RHI 按编译顺序执行命令和 barrier。
Raster、Raytracing、UI、场景上传、环境贴图处理和导出读回都采用 `Compiled`；编译或录制失败不会回退到 reorderer。
串行/并行录制只改变 CPU 调度，不改变这个契约。

一次访问由 `TextureAccessType` / `BufferAccessType`（用途）、`AccessMode`（读写）、
执行域和子资源范围共同描述。它们与创建资源时的 usage flags 不同：创建能力允许多种用途，
pass 声明选择本次用途。lowering 据此生成 stage、access mask 和图像布局。
`Sampled` 使用只读 shader 布局，`Storage` 使用 GENERAL；storage 的只读访问仍声明为
`Read/Storage`。颜色和深度附件分别使用 `ColorAttachment`、`DepthStencilAttachment`，
深度读写权限只由 `AccessMode` 决定。相同用途之间发生写后读，仍由编译器建立内存依赖。

普通 pass 不能传入边界枚举。`TextureBoundary::Undefined`、`BufferBoundary::Undefined`
只用于声明无须保留内容的初始状态；`TextureBoundary::PresentationSource` 表达呈现复制的
交接约定。初始化和导出接口通过类型安全的 variant 接受普通用途或对应资源的特殊边界，
内部 `ResourceState` 统一保存这些状态，不引入另一份状态账本。没有执行端点的 `Present`
已移除。`Unspecified` 仅保留给逻辑依赖声明；实际 lowering 必须得到明确的访问类型。

| 责任 | 保证者 |
| --- | --- |
| pass 访问哪些资源、读写方式、mip/layer 范围 | pass 声明；多阶段算法按依赖拆成节点 |
| pass 顺序、barrier、托管队列间等待与所有权转移 | RDG 编译与 lowering |
| bindless 实际可能访问的目标集合 | 场景/资源所有者提供候选集合；绑定整个 heap 不等于声明目标访问 |
| bindless 索引、descriptor 存储的更新与发布 | Vulkan 内部固定协议，不扫描应用目标资源 |
| BLAS 顶点/索引输入、BLAS/TLAS 公共输出 | RDG 声明 |
| AS scratch、TLAS instance 上传及其内部依赖 | Vulkan 构建命令的私有协议 |
| 资源保活、提交接受、GPU 完成与回收 | 沿用 RHI 提交/完成机制；三者不混为一项状态 |

跨图资源采用固定边界：普通图像为 `Sampled`，presentation source 为
`PresentationSource`，普通场景 buffer 为 `ShaderRead`，加速结构为
`AccelerationStructureRead`。所有者在新图像分配时建立布局；每张图导出约定状态。
这把跨图协调放到明确的边界，省去另一份后端 preferred-state 表和 TLAS 地址登记表，
代价是图之间可能增加状态转换。图内仍按实际依赖同步。

新图像只建立布局，不因此获得有效内容。TAA、ReSTIR、NRD 的历史有效性仍由
实际接受的帧推进；录制成功不代表 native submit 已接受，更不代表 GPU 已完成。

场景上传与后续 AS 构建合为一个 Graphics 命令流，避免额外的场景 Copy 提交与手工交接。
这放弃初始场景上传的独立 Copy 重叠。独立运行时贴图加载仍由一张 RDG 图执行
Copy → Graphics 交接，跨队列依赖由编译结果生成。

NRD 的公共纹理依赖也由图声明：RELAX 读取 motion，REBLUR 还可能写回 motion，因此后者
声明为 `ReadWrite/Storage`。NRD/NRI 负责 pass 内部临时纹理及多次 dispatch 的同步，
并依据实际结束状态将公共纹理恢复到声明状态，再交回 RDG；插件不再访问后端资源 tracker。
其串行 native 录制约束来自 NRI 的可变 descriptor 和 command-buffer 缓存，与资源依赖分析无关。

Vulkan 只接受 `Compiled` 命令流；任何非空的非 compiled 提交和推断式 barrier 都在 native
录制之前被拒绝。旧 reorderer、状态 tracker、preferred-state 恢复、StateSeed 和分层 native
并行录制器已经移除。后端仅将完整屏障翻译为 Vulkan 屏障，并校验范围与队列端点。
同状态只读访问无需屏障；buffer 字节区间和 texture aspect/mip/layer 范围直接保留。
新增入口应补充 RDG 资源声明或明确的私有同步协议。

并行化保留在两个已有边界：RDG 的独立 pass 录制，以及 RHI 的独立 source/native queue
翻译。旧 `RHI.CommandRecording.Parallel*` 配置已移除。资源保活仍随提交进入 Completion，
回读 staging 在 GPU 完成后读取并回收；没有新增一份资源生命周期。

Vulkan 提交统一由 `RHIExecutor` 调度：Translate 只录制并交接拥有资源的提交包，Submission
独占原生提交，Completion 在 GPU 完成后发布结果并回收。Queue 不再保存另一套 RHI 工作队列；
直接调用 Queue 的 `Execute/Present/Wait` 会被拒绝，依赖随提交包交给 Executor。
RHI 层的 `RHISubmissionRequest` 表示一份带逻辑队列选择的 `CmdSubmit`，
`RHISubmissionBatch` 将一组请求、拓扑和可选 Present 一起交给后端。
`Submit` 接管请求并排入 Executor 工作队列，`ProcessSubmissionRequests` 在该线程暂存或发布请求；
`ProcessFlushRequest` 和 `ProcessSyncRequest` 分别处理发布与同步边界。
Executor 模块集中在 `source/runtime/render/rhi/executor/`，调用方通过
`rhi/executor/RHIExecutor.h` 使用公共入口。实现按责任分开：
`RHIExecutor.cpp` 保留入口、批次发布和生命周期；
`RHIExecutorRecording.cpp` 负责将录制结果封装为提交请求并应用元数据；
`RHIExecutorRejection.cpp` 负责失败、取消时的结果通知和清理；
`RHIExecutorBackend.cpp` 负责后端工厂及非 Vulkan 队列适配。
`RHIExecutorWorkQueue` 持有 Executor 工作线程，所有操作统一通过 `Enqueue` 入队。
任务的 `prerequisites` 表示需要等待的 CPU 录制条件；即使为空也遵循 FIFO 顺序。
回调接收 `Execute`、`Reject` 或 `Cancel`，分别执行正常处理、录制失败收尾或关闭取消收尾。
`RHIExecutorInternal.h` 只声明实现文件间共享的辅助函数。
测试钩子放在模块内的 `executor/testing/`，不再由公共 `RHIExecutor.h` 暴露。
各图形 API 的实现统一放在 `rhi/backends/` 下的 `vulkan/`、`metal/`、`d3d12/`，
其中 `VulkanSubmissionExecutor` 属于 Vulkan 后端；公共 Executor、线程设施和提交拓扑保留在 RHI 层。
提交、Flush、Sync 和关闭共用批次发布过程；待发布批次始终保留未被后端接收的数据，
失败通知在锁外完成。Present 的接纳和发布仍在同一发布锁范围内，防止后续操作越过它。
Vulkan 的 `RunTranslateThread` 和 `RunSubmissionThread` 对应后端的两条工作线程。
其内部 `WorkerRequest` 包含批次提交、同步或停止操作，`CpuTaskCompletion` 通知 CPU 工作完成；
`VulkanSubmissionResult` 表示原生提交是否接受及可用的完成等待点，GPU 完成仍由队列 timeline 确认。
完成队列只接收拥有本次资源的完成包，不再逐条分派回调、Context 或 descriptor lease。
`ReserveSubmission` 从入口起以独立的 `VulkanRecordedSubmission` / `VulkanRecordedCopySubmission`
持有请求并预留逻辑队列序号，Executor 再调用 `VulkanCommandRecorder::Record`。录制失败也保留这个拥有资源的包，
继续由 Submission 发布批次结果，再交给 Completion 收尾，避免局部变量析构提前触发通知。
`submission_outcome` 为空表示尚未决定提交结果，有值表示已接受或已拒绝；它不表示 GPU 已完成。
原生提交成功后先保存结果，再发布其他状态，后续 CPU bookkeeping 异常不能把已接受的工作误当作未提交。

录制与提交由 Executor 显式串联：预留序号 → Recorder 录制 → Queue 提交 → Completion 收尾。
Device 为 Graphics、Compute、Copy 各持有一个 Recorder；原来的 Context 池、隔离区和录制统计迁入 Recorder，
没有增加第二份池或工作线程。录制中的 Context 随提交包转交，Completion 发布查询结果并执行 Context 回调后，
才调用 Recorder 的 `RecycleContexts` 复用或隔离。Device 先排空并销毁 Queue，再销毁 Recorder。
原来的 transferable lease 随提交包覆盖录制到提交决策，仍用于串行保护同一逻辑队列的可变录制状态。

`VulkanCommandRecording.cpp` 实现命令 visitor 与 Graphics/Compute 录制，`VulkanCopyCommandRecording.cpp`
处理 Copy 校验与录制；它们属于 Recorder，不再实现 Queue 成员。
`VulkanSubmission.h` 定义三阶段共享的拥有资源的交接包，Recorder 头文件不依赖 Queue。
`VulkanQueueCompletion.cpp` 确认 GPU 完成并收尾；`VulkanQueuePresentation.cpp` 保留交换链获取、呈现状态事务
及获取图像后才能录制的私有复制操作。`VulkanCommandProfiler.*` 由 Recorder 持有，Queue 仅转发查询接口。
`VulkanQueueInternal.*` 共享依赖等待、结果发布等实现函数，不另设状态所有者。

提交顺序由 Executor 保证，共享 `VkQueue`
的 host 锁仍负责原生队列互斥。同步边界先排空 Executor 的录制与提交，再等待 Queue 的完成
处理，不能只等待 GPU timeline 而漏掉 CPU 回收。`rhi_thread/rhi_bypass` 和旧
`parallel_record*` 配置已移除；Render Thread、帧延迟和 RDG 并行录制仍是独立的上层选择。

完成进度与关闭标志的更新必须和条件变量等待共用 `event_mutex`，防止通知落在条件检查与
真正睡眠之间；回调仍在锁外执行。Graphics/Compute 的工作等待与回收等待共用一个条件变量，
入队时使用 `notify_all`，避免只唤醒回收等待者而让 Completion 工作线程继续睡眠。

CUDA/TensorRT 使用两个 compiled 图之间的显式交接，不在 RDG 录制回调内提交队列。
前图先导出固定边界并提交；interop owner 把具体共享纹理从 `Sampled` 转为 `GENERAL`、
交给 external queue family，并通过导出的 RHI timeline 让 CUDA 等待。
CUDA stream 完成后，后续 RHI 提交等待 CUDA 的 timeline，收回纹理并恢复 `Sampled`，然后录制后图。
这些 Vulkan 提交也采用 `Compiled`；不存在 native queue 的旁路提交或资源推断。
这是 [Vulkan 外部资源所有权与同步](https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-queue-transfers)
所需的交接。当前实现选择在此边界等待 CUDA 和收回提交完成，以统一临时映射、推理 buffer 和 semaphore 的复用条件；
代价是启用 CUDA 时增加提交及 CPU 等待，暂不追求 Vulkan/CUDA 重叠。
外部交接在图之间完成，不作为普通 GPU pass 的一种执行分类。

GPU pass 统一通过 `AddRecordPass` 接收 RDG 独占分配的 CommandList；回调不负责封装或提交它。
`PassRecordPolicy` 只描述 CPU 录制约束，并由调用方在该接口中显式声明一次：
`ParallelAllowed` 允许同组回调并发录制；`Serial` 等待前组结束，在调用线程执行，再开始后组。
GPU 资源依赖、Token 与 `DependsOn` 仍约束命令执行顺序，不隐含 CPU 回调的先后关系。
串行录制不等待 GPU 完成，也不提供跨图或后端线程互斥。

UI frame packet 状态推进、Culling 共享读回槽和 bindless 待更新队列的消费仍保留串行约束。
不可变上传、已准备的 AS 命令及仅声明 GPU 边界的 pass 可以并行录制。CPU 准备使用 Setup pass，
接受/完成后的状态更新继续归提交回调所有；原来的 MainThread、CpuPrepare、ExternalControl
执行模型及 caller-owned CommandList 衔接已经移除。原生翻译策略保持独立，NRD 的前端录制
可以并行，而 NRI 可变缓存仍由后端 `SerialControl` 保护。
