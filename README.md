# MoerEngine

实时渲染引擎

**| 简体中文 | [English](README.en.md) |**

## 依赖

* 操作系统
  * Windows 10 or 11
* 编译器（二选一）
  * MSVC == 19.44.*
    * 注：MSVC安装时，语言包请选择英文，否则编译时有概率出现乱码错误
  * clang（待测试）

* CMake >= 3.26.0 且 < 4.0.0 ([download link](https://github.com/Kitware/CMake/releases/tag/v3.31.9))
* Git ([download link](https://git-scm.com/downloads))


## 如何构建&运行？

### MoerEngine

- 方法一：命令行

  ```bash
  # Clone仓库
  # 如果没有配置SSH的话，请把 `git@xxx` 替换为 `https://github.com/NJUCG/MoerEngine.git`
  git clone git@github.com:NJUCG/MoerEngine.git
  cd MoerEngine
  
  # 忽略一些特定commit对commit历史的影响
  git config --local blame.ignoreRevsFile .git-blame-ignore-revs
  
  # 下载Sponza场景文件，到此目录：`./asset/scenes/`
  git clone --branch sponza-scene-files --depth 1 git@github.com:NJUCG/MoerEngine.git ./asset/scenes/sponza
  
  # 根据模板创建一份MoerEngine的配置文件
  # 配置文件中，可以设置默认的渲染器（光栅化、光追）、默认分辨率等选项
  cp template.MoerEngine.toml MoerEngine.toml
  
  # 构建
  cmake -B build
  cmake --build build -j16 # change 16 to your cpu core count
  
  # 运行
  ./target/bin/Debug/MoerEditor.exe
  ```

  - 如果安装了 [just](https://github.com/casey/just)，你可以通过如下这两条命令部署、构建、启动引擎
    ```bash
    just setup
    just gbr # generate build run
    ```

- 方法二：Rider
  - TODO

### CUDA等AI组件支持

* 如果你希望在MoerEngine中启用CUDA、LibTorch、TensorRT，那么你需要手动在系统中安装这三个依赖，再在MoerEngine中配置他们。接下来为启用AI组件的具体操作手册：
1. 下载依赖
   * [CUDA Toolkit 12.8 Downloads](https://developer.nvidia.com/cuda-12-8-0-download-archive)
   * [PyTorch Downloads](https://pytorch.org/get-started/locally/)
   * [TensorRT 10.x Downloads](https://developer.nvidia.com/tensorrt/download/10x)
   * 推荐版本
     * CUDA Toolkit 12.8
     * libtorch-2.8.0-cu128
     * TensorRT-10.12.0.36
     * **请注意，LibTorch和TensorRT版本需要与CUDAToolkit版本相对应**

2. 根据模板创建配置文件（启用AI组件）
   * 根据 `template.EnableCuda.cmake` 创建 `EnableCuda.cmake`
   * 修改 `EnableCuda.cmake` 的内容
   * 注：MoerEngine的构建系统会自动检测 `EnableCuda.cmake` 文件。**如果该文件存在，则会启用AI组件**

3. 将动态库添加进PATH

   * 将你安装的LibTorch和TensorRT的 `lib` 目录添加进 `PATH` 环境变量。例子如下图：
   * ![image-20250920204538099](README/image-20250920204538099.png)
   * 注：如果动态库缺失，则引擎会在没有任何错误提示的情况下崩溃

4. 重新编译MoerEngine

   ```bash
   cmake -B build
   cmake --build build -j16
   # 或者使用just
   just gb # generate build
   ```

   * 观察日志，若generate的日志出现了 `WITH_CUDA=1`，则成功启用AI相关组件；若日志中为 `WITH_CUDA=0`，则没有启用AI相关组件

5. 配置VSCode的IntelliSense**（可选）**
   * 设置环境变量 `CUDA_PATH`、`LIBTORCH_PATH`、`TENSORRT_PATH`。例子如下图：
   * ![image-20250920201137320](README/image-20250920201137320.png)
   * ![image-20250920201248463](README/image-20250920201248463.png)

#### 注意事项

* 启用CUDA后，光栅化渲染器就不支持Stencil模板测试了。原因是CUDA不支持 `PF_D32_SFLOAT_S8_UINT`

### NRD降噪器支持

* MoerEngine支持NVIDIA NRD拓展。由于NVIDIA的封闭协议，MoerEngine无法直接引入NRD，需要用户自行下载NRD源码并进行配置。以下为具体操作步骤

1. Clone NRD源码
   ```bash
   # 推荐在非MoerEngine目录下Clone NRD源码，避免不小心提交NRD源码
   # 如果无法访问，请联系项目维护者
   git clone git@github.com:NJUCG/NRD.git
   ```

2. 根据模板创建配置文件
   * 根据 `template.EnableNrd.cmake` 创建 `EnableNrd.cmake`
   * 修改 `EnableNrd.cmake` 的内容，将 `NRD_DIR` 设置为你Clone的NRD源码路径

3. 重新编译MoerEngine

   ```bash
   cmake -B build
   cmake --build build -j16
   # 或者使用just
   just gb # generate build
   ```

   * 观察日志，若generate的日志出现了 `WITH_NRD=1`，则成功启用NRD；若日志中为 `WITH_NRD=0`，则没有启用NRD

## 使用方法

### 如何渲染场景？

- 方法一：GUI
  - 打开MoerEditor后，点击 `OpenScene` 打开文件选择器，并根据右下角支持格式来选择不同格式的场景

- 方法二：配置文件
  - `MoerEditor.exe` 会使用同目录下的 `MoerEngine.toml` 作为配置文件。这个配置文件包含了启动时的默认场景
  - 注：在每次编译MoerEngine时，`./target/bin/Debug/MoerEngine.toml` 会被替换为 `./MoerEngine.toml`。所以如果你是开发者，请修改 `./MoerEngine.toml`

### 如何移动摄像机

- MoerEngine的摄像机控制逻辑与UnrealEngine类似
  - 长按右键时，通过 `W/A/S/D/Q/E` 来移动摄像机
  - 长按右键时，拖动鼠标 来 旋转摄像机
  - 长按左键时，拖动鼠标 来 在水平面内操作摄像机
  - 长按中键时，拖动鼠标 来 在竖直面内操作摄像机
- 按下 `F` 键，进入漫游模式
- 通过引擎UI右上角的 `Camera` 选项页来设置摄像机的移动速度和FOV

## IDE & IntelliSence配置

*TODO*

### VSCode配置相关

- 设置中的 `C_Cpp.default.compilerPath` 字段不能使用msvc编译器，否则IntelliSense会出现假错。推荐使用clang
  - 注：和编译无关，只和 IntelliSense（IDE的智能代码高亮与补全）有关

## 如何贡献

本项目欢迎PR。你可以从Github Issues中选择一个Issue解决，或者提出一个新的Issue。

如果你想要开发的功能或者修复的问题没有在Issues中出现，请先提出一个新的Issue，以告知维护者和其他开发者。

如果你想要开发的功能较为复杂，请提前在Issue中和维护者进行沟通，以确保PR可以被合并。在开发复杂功能的过程中，推荐使用Draft PR，以便让维护者和其他开发者随时查看你的进度并进行交流。

main分支为开发分支，release分支为稳定分支，所以PR都应提交到main分支。

以下命名规范均为推荐规范，非强制要求。但请尽量遵循，以保持代码风格的一致性和可读性。

### Git规范

如果你是NJU META的成员（即 拥有仓库读写权限），你可以直接在MoerEngine仓库中创建对应分支进行开发。推荐的分支命名格式为 `feature/xxx` 或 `bugfix/xxx`。同时，也请创建分支对应的Draft PR，以告知其他开发者你正在开发的内容。

Commit信息，请遵循 [Conventional Commits](https://www.conventionalcommits.org/zh-hans/v1.0.0/) 规范。

### 第三方库与依赖项

引入第三方库时，请检查对应的协议。如果是MIT、Apache等宽松协议，可以直接引入。否则，请先和维护者沟通。

此外，**请务必检查头文件是否有额外要求！**例如，`volk` 要求include头文件前定义平台相关宏、`NRD` 要求特定的头文件include顺序。这种情况下，请 **务必创建一个单独的头文件来封装该依赖库的include逻辑**，否则会导致其他开发者错误的include行为。

### C++命名规范

*TODO*

Shaders文件夹架构及相关规范见`/shaders/README.md`。

### CMake命名规范

|               | 命名风格        | 示例                                     |
| ------------- | --------------- | ---------------------------------------- |
| 指令/函数     | 小写            | add_executable(...), set(...)            |
| 关键字        | 大写 & 下划线   | STATIC, GLOB_RECURSE, PRIVATE            |
| 局部变量      | 小写 & 下划线   | set(my_source_files ...), libtorch_dir   |
| 全局/缓存变量 | 大写 & 下划线   | option(WITH_CUDA), set(LIBTORCH_DIR ...) |
| 目标(Target) | 命名空间 & 驼峰 | Moer::Render, CUDA::cudart               |

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

## 开源协议

MoerEngine源代码采用Apache-2.0 License授权。

本项目开发过程中使用了以下项目：

* [assimp](https://github.com/assimp/assimp)： BSD-3-Clause License
* [astc-encoder](https://github.com/ARM-software/astc-encoder): Apache 2.0 License
* [dds_image](https://github.com/spnda/dds_image): MIT License
* [tinyexr](https://github.com/syoyo/tinyexr): BSD-3-Clause License
* ktx: Apache-2.0 License
* [pugixml](https://github.com/zeux/pugixml): MIT License
* [stb](https://github.com/nothings/stb): MIT License
* D3D12MemoryAllocator: MIT License
* DirectXShaderCompiler: University of Illinois/NCSA Open Source License
* [glfw](https://github.com/glfw/glfw): Zlib License
* [imgui](https://github.com/ocornut/imgui): MIT License
* [JSON for Modern C++](https://github.com/nlohmann/json): MIT License
* [meshoptimizer](https://github.com/zeux/meshoptimizer): MIT License
* [metis](https://github.com/KarypisLab/METIS/): Apache-2.0 License
* [mimalloc](https://github.com/microsoft/mimalloc): MIT License
* [nativefiledialog-extended](https://github.com/btzy/nativefiledialog-extended): Zlib License
* [NRI](https://github.com/NVIDIA-RTX/NRI): MIT License
* [smaa](https://github.com/iryoku/smaa): MIT License
* [spdlog](https://github.com/gabime/spdlog): MIT License
* [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross): Apache-2.0 License
* [tomlplusplus](https://github.com/marzer/tomlplusplus): MIT License
* [volk](https://github.com/zeux/volk): MIT License
* [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator): MIT License
* [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers): Apache-2.0 License
* WinPixEventRuntime: MIT License

外部构建依赖（以下组件不包含在源码仓库中，需在构建时手动或自动下载）：

* [NVIDIA NRD](https://github.com/NVIDIA-RTX/NRD): 通过git下载（可选）
* [D3D12 Agility SDK](https://devblogs.microsoft.com/directx/directx12agility/)：通过CMake自动下载
* [CUDA Toolkit](https://developer.nvidia.com/cuda-toolkit)：官网下载（可选）
* [LibTorch](https://pytorch.org/get-started/locally/)：官网下载（可选）
* [TensorRT](https://developer.nvidia.com/tensorrt/download/10x)：官网下载（可选）
