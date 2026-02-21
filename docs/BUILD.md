# 构建手册

## 目录

- [构建手册](#构建手册)
  - [目录](#目录)
  - [1. 依赖](#1-依赖)
  - [2. MoerEngine](#2-moerengine)
  - [3. CUDA等AI组件支持](#3-cuda等ai组件支持)
    - [注意事项](#注意事项)
  - [4. NRD降噪器支持](#4-nrd降噪器支持)

## 1. 依赖

* 操作系统
  * Windows 10 or 11
* 编译器（二选一）
  * MSVC == 19.44.*
  * clang + ninja
* Vulkan SDK 1.3 ([download link](https://vulkan.lunarg.com/sdk/home))
  * 推荐使用Vulkan SDK 1.3
  * Vulkan SDK 1.4版本会导致无法使用DebugPrintfEXT（待解决）

* CMake >= 3.26.0 且 < 4.0.0 ([download link](https://github.com/Kitware/CMake/releases/tag/v3.31.9))
* Git ([download link](https://git-scm.com/downloads))

## 2. MoerEngine

- 方法一：命令行

  ```bash
  # Clone仓库
  # 如果没有配置SSH的话，请把 `git@xxx` 替换为 `https://github.com/NJUCG/MoerEngine.git`
  # MoerEngine新的依赖项均使用submodule形式引入，所以请添加--recursive来clone所有依赖项
  # - 如果在clone时忘记添加 --recursive，请执行git submodule update --init --recursive
  git clone git@github.com:NJUCG/MoerEngine.git --recursive
  cd MoerEngine
  
  # 忽略一些特定commit对commit历史的影响（只影响开发过程，不影响编译和使用）
  git config --local blame.ignoreRevsFile .git-blame-ignore-revs
  
  # 下载Sponza场景文件，到此目录：`./asset/scenes/`
  git clone --branch scene/sponza --depth 1 git@github.com:NJUCG/MoerEngine.git ./asset/scenes/sponza
  
  # 根据模板创建一份MoerEngine的配置文件
  # 配置文件中，可以设置默认的渲染器（光栅化、光追）、默认分辨率等选项
  cp template.MoerEngine.toml MoerEngine.toml
  
  # 构建
  cmake -B build
  # 如果想要使用clang+ninja，可以替换为：cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
  cmake --build build -j16 # change 16 to your cpu core count
  
  # 运行
  ./target/bin/Debug/MoerEditor.exe
  ```

  - 如果安装了 [just](https://github.com/casey/just)，你可以参考根目录下的 `template.justfile` 文件来编写你自己的脚本
  
- 方法二：Rider
  
  - TODO

## 3. CUDA等AI组件支持

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
   * ![image-20250920204538099](BUILD/image-20250920204538099.png)
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
   * ![image-20250920201137320](BUILD/image-20250920201137320.png)
   * ![image-20250920201248463](BUILD/image-20250920201248463.png)

### 注意事项

* 启用CUDA后，光栅化渲染器就不支持Stencil模板测试了。原因是CUDA不支持 `PF_D32_SFLOAT_S8_UINT`

## 4. NRD降噪器支持

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