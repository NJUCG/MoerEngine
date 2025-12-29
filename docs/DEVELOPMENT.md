# 开发手册

## 规范

以下命名规范均为推荐规范，非强制要求。但请尽量遵循，以保持代码风格的一致性和可读性。

### Git规范

如果你是NJU META的成员（即 拥有仓库读写权限），你可以直接在MoerEngine仓库中创建对应分支进行开发。推荐的分支命名格式为 `feature/xxx` 或 `bugfix/xxx`。同时，也请创建分支对应的Draft PR，以告知其他开发者你正在开发的内容。

Commit信息，请遵循 [Conventional Commits](https://www.conventionalcommits.org/zh-hans/v1.0.0/) 规范。

### 第三方库与依赖项

引入第三方库时，请检查对应的协议。如果是MIT、Apache等宽松协议，可以直接引入。否则，请先和维护者沟通。

此外，**请务必检查头文件是否有额外要求！**例如，`volk` 要求include头文件前定义平台相关宏、`NRD` 要求特定的头文件include顺序。这种情况下，请 **务必创建一个单独的头文件来封装该依赖库的include逻辑**，否则会导致其他开发者错误的include行为。

### C++命名规范

*TODO*

### HLSL命名规范

Shaders文件夹架构及相关规范见`/shaders/README.md`。

### CMake命名规范

|               | 命名风格        | 示例                                     |
| ------------- | --------------- | ---------------------------------------- |
| 指令/函数     | 小写            | add_executable(...), set(...)            |
| 关键字        | 大写 & 下划线   | STATIC, GLOB_RECURSE, PRIVATE            |
| 局部变量      | 小写 & 下划线   | set(my_source_files ...), libtorch_dir   |
| 全局/缓存变量 | 大写 & 下划线   | option(WITH_CUDA), set(LIBTORCH_DIR ...) |
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

## IDE & IntelliSence配置

*TODO*

### VSCode配置相关

- 设置中的 `C_Cpp.default.compilerPath` 字段不能使用msvc编译器，否则IntelliSense会出现假错。推荐使用clang
  - 注：和编译无关，只和 IntelliSense（IDE的智能代码高亮与补全）有关