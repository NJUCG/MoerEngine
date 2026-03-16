# 开发手册 与 规范

## 目录

- [1. 推荐使用的开发工具](#1-推荐使用的开发工具)
  - [just](#just)
- [2. 设计思路](#2-设计思路)
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

### C++规范

MoerEngine开发早期使用 `.clang-tidy` 对命名风格进行规范，但强制性较弱，导致风格不统一，历史遗留问题大。

因此，**这里的规范从 2026.1.8 启用**。此前代码暂不做更新，等到以新命名规范的代码数量超过旧代码，再考虑统一修改。

此处规范和先前规范的主要区别在于：形参不应该以 `_` 开头；非POD成员变量应该以 `m_` 开头。此文档优先级高于 `.clang-tidy`

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