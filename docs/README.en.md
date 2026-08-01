# MoerEngine

Engine for Realtime Rendering

**| [简体中文](README.md) | English |**

**This README is outdated. If you need English version, please raise an issue. We will update it as soon as possible.**

## Dependencies

* OS
  * Windows 10 or 11
* Compiler
  * MSVC == 19.44.*
  * or clang（待测试）
* CMake >= 3.26.0 且 < 4.0.0 ([download link](https://github.com/Kitware/CMake/releases/tag/v3.31.9))
* Git ([download link](https://git-scm.com/downloads))

## How to Build & Run

### MoerEngine

- Method 1: Command Line

  ```bash
  # Clone the repo
  # Tip: If you don't have configured SSH, replace `git@xxx` with `https://github.com/NJUCG/MoerEngine.git`
  git clone --branch dev-rhi-remake git@github.com:NJUCG/MoerEngine.git
  cd MoerEngine

  # Ignore some specific commits (formatting changes)
  git config --local blame.ignoreRevsFile .git-blame-ignore-revs
  
  # Download Sponza scene to `./asset/scenes/`
  git clone --branch scene/sponza --depth 1 git@github.com:NJUCG/MoerEngine.git ./asset/scenes/sponza
  
  # Configure the engine (use default settings)
  cp template.MoerEngine.toml MoerEngine.toml
  
  # Build
  cmake -B build
  cmake --build build -j16 # change 16 to your cpu core count
  
  # Run
  ./target/bin/Debug/MoerEditor.exe
  ```

  - If you have installed [just](https://github.com/casey/just), you can run the following commands to build and run the engine.
    ```bash
    just setup
    just gbr # generate build run
    ```

- Method 2: Visual Studio 2022 (GUI)

  Visual Studio 2022 can open a folder whose root contains `CMakeLists.txt` directly; no generated `.sln` is required. See [CMake projects in Visual Studio](https://learn.microsoft.com/en-us/cpp/build/cmake-projects-in-visual-studio?view=msvc-170).

  1. In Visual Studio Installer, install the **Desktop development with C++** workload and the **C++ CMake tools for Windows** component.
  2. In File Explorer, copy `template.MoerEngine.toml` to `MoerEngine.toml` in the repository root. This file must exist before the first configure.
  3. Select `File -> Open -> Folder` in Visual Studio and open the repository root. Because the root contains `CMakeLists.txt`, Visual Studio enables CMake integration and automatically generates the cache for its default configuration.
  4. Select `x64-Debug` in the Configuration dropdown. If automatic configuration did not run, or a setting changed, select `Project -> Configure Cache`. CMake diagnostics are available in the CMake category of the Output window.
  5. After configuration succeeds, select `MoerEditor.exe` in the Startup Item dropdown. Choose `Build -> Build All`, or open CMake Targets View in Solution Explorer, right-click `MoerEditor`, and choose Build.
  6. Press `F5` to build and debug. To run without the debugger, press `Ctrl+F5` or select `Debug -> Start Without Debugging`.

  The Debug executable is written to `target/bin/Debug/MoerEditor.exe`. Select `x64-Release` in the Configuration dropdown for a Release build, which writes `target/bin/Release/MoerEditor.exe`.

  Rider 2026.1 introduced Beta support for CMake C++ projects, but current Rider releases do not yet expose the same CMake toolchain/profile configuration UI as CLion. This guide therefore does not present an unverified Rider menu path as a supported build method. See [Rider 2026.1 CMake support](https://www.jetbrains.com/rider/whatsnew/2026-1/#cmake-support-for-c-gaming-projects-beta).

### CUDA, LibTorch, and TensorRT

> Needs to be updated from Chinese Version

* CUDA
  * If you installed CUDA Toolkit in your system, Moer Engine will build with cuda **automatically.**
  * [CUDA Toolkit 12.8 Downloads](https://developer.nvidia.com/cuda-12-8-0-download-archive)
* LibTorch and TensorRT

  * If you want to enable them, you need to install them manually, and tell Moer Engine where they are.
  * First, install LibTorch and TensorRT.

    * [PyTorch Downloads](https://pytorch.org/get-started/locally/)
    * [TensorRT 10.x Downloads](https://developer.nvidia.com/tensorrt/download/10x)
    * **Please download the version that matches your CUDA Toolkit!**
    * Recommended: `CUDA Toolkit 12.8`, `libtorch-2.8.0-cu128`, `TensorRT-10.12.0.36`
  * Then, run `cp source/cuda/template.LibTorch_TensorRT.cmake source/cuda/LibTorch_TensorRT.cmake`
  * Modify `LibTorch_TensorRT.cmake`
  * Add `/path/to/torch/lib` and `/path/to/tensorRT/lib` to your PATH
    * ![image-20250920204538099](README/image-20250920204538099.png)
  * Finally, recompile, and it should work.
* IntelliSense
  * You can set Environment Variables `CUDA_PATH`, `LIBTORCH_PATH`, `TENSORRT_PATH`. Then, vscode will load corresponding include path to IntelliSense.
  * Detailed in `.vscode/c_cpp_properties.json`
  * Example
    * ![image-20250920201137320](README/image-20250920201137320.png)
    * ![image-20250920201248463](README/image-20250920201248463.png)

## How to use

### How to render other scenes

- Method 1: GUI
  - Select `Open Scene` in GUI

- Method 2: Config
  - If you succeed to run Moer Engine, open the configuration file `./target/bin/Debug/config/MoerEngine.toml`.
  - Then, change `scene_path` to any other scene files in your machine.
    - Such as `scene_path = E:\Data\Models\BlueArchive\aris\CH0200.fbx.`
  - Run `./target/bin/Debug/MoerEditor.exe` again, you will see the new scene.
  - Tip: `./target/bin/Debug/config/*` will be replaced with `./source/configs/*`, every time you compile.

### Camera Controlling

- The camera control method of **Moer Engine** is very similar to Unreal Engine's.
  - When dragging the mouse, press `W/A/S/D/Q/E` to **move the camera**.
  - Drag `right mouse button` to **rotate the camera**.
  - Drag `left mouse button` to rotate the camera and move forward/backward.
  - Drag `both mouse buttons` or `middle button` to move the camera.
- Press `F` to control camera without dragging.
- Press `↑` or `↓` to change the **speed** of camera.
- Scroll the mouse wheel to change **FOV**.
