# MoerEngine

实时渲染引擎

**| 简体中文 | [English](README.en.md) |**

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/NJUCG/MoerEngine)

> 注：MoerEngine目前仍处于早期开发阶段，功能和性能均不完善，并正在进行小范围重构。

## 目录

- [1. 如何构建](#1-如何构建)
- [2. 如何使用](#2-如何使用)
- [3. 效果图](#3-效果图)
- [4. 如何贡献](#4-如何贡献)
- [开源协议](#开源协议)

## 1. 如何构建

* 详细的 *构建步骤*、*依赖项*、*CUDA等AI插件*、*NRD插件* 请参考【[构建手册](./docs/BUILD.md)】

* 通过命令行构建与运行MoerEngine

    ```bash
    # Clone仓库
    git clone --recurse-submodules --shallow-submodules git@github.com:NJUCG/MoerEngine.git
    cd MoerEngine
    
    # 根据模板创建一份MoerEngine的配置文件
    cp template.MoerEngine.toml MoerEngine.toml
    
    # 构建
    cmake -B build
    cmake --build build -j16 # change 16 to your cpu core count
    
    # 运行
    ./target/bin/Debug/MoerEditor.exe
    ```

## 2. 如何使用

### 2.1 如何渲染场景

- 方法一：GUI
  - 打开MoerEditor后，点击 `OpenScene` 打开文件选择器，并根据右下角支持格式来选择不同格式的场景
- 方法二：配置文件
  - `MoerEditor.exe` 会使用同目录下的 `MoerEngine.toml` 作为配置文件。这个配置文件包含了启动时的默认场景

### 2.2 如何移动摄像机

- MoerEngine的摄像机控制逻辑与UnrealEngine类似
  - 长按右键时，通过 `W/A/S/D/Q/E` 来移动摄像机
  - 长按右键时，拖动鼠标 来 旋转摄像机
  - 长按左键时，拖动鼠标 来 在水平面内操作摄像机
  - 长按中键时，拖动鼠标 来 在竖直面内操作摄像机
- 按下 `F` 键，进入漫游模式
- 通过引擎UI右上角的 `Camera` 选项页来设置摄像机的移动速度和FOV

## 3. 效果图

### RayTracing Renderer

![image-20251229200054104](README/image-20251229200054104.png)

![image-20251229125336195](README/image-20251229125336195.png)

![image-20251229124932154](README/image-20251229124932154.png)

![image-20251229125512698](README/image-20251229125512698.png)

![image-20251229154103450](README/image-20251229154103450.png)

![image-20251229194034456](README/image-20251229194034456.png)

![image-20251229195717156](README/image-20251229195717156.png)

### Raster Renderer

![image-20251229140647045](README/image-20251229140647045.png)

![image-20251229125126217](README/image-20251229125126217.png)

## 4. 如何贡献

本项目欢迎PR。你可以从Github Issues中选择一个Issue解决，或者提出一个新的Issue。

如果你想要开发的功能或者修复的问题没有在Issues中出现，请先提出一个新的Issue，以告知维护者和其他开发者。

如果你想要开发的功能较为复杂，请提前在Issue中和维护者进行沟通，以确保PR可以被合并。在开发复杂功能的过程中，推荐使用Draft PR，以便让维护者和其他开发者随时查看你的进度并进行交流。

main分支为开发分支，release分支为稳定分支，所以PR都应提交到main分支。

更详细的信息，请参考【[开发手册](./docs/DEVELOPMENT.md)】

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
