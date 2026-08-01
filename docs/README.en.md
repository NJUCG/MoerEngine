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

- Method 2: Rider (GUI)

  > Direct CMake C++ project support is available in Rider 2026.1 and newer and is currently marked Beta. Older Rider releases cannot use this workflow. See [CMake support for C++ gaming projects](https://www.jetbrains.com/rider/whatsnew/2026-1/#cmake-support-for-c-gaming-projects-beta).

  1. In File Explorer, copy `template.MoerEngine.toml` to `MoerEngine.toml` in the repository root. This file must exist before the first CMake configure.
  2. Select `File -> Open` in Rider and open the repository root. Rider loads the project from the root `CMakeLists.txt`.
  3. Open `Settings -> Build, Execution, Deployment -> Toolchains`. Select the Visual Studio 2022 toolchain to provide the Windows SDK, `rc.exe`, and the linker, then make sure Rider detects CMake, Ninja, Clang, and Clang++.
  4. Open `Settings -> Build, Execution, Deployment -> CMake` and create a Debug profile with:
     - Generator: `Ninja`;
     - Build type: `Debug`;
     - Build directory: `build` under the repository root;
     - CMake options: `-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`.
  5. Reload the CMake project. After configuration succeeds, select the `MoerEditor` target in the Rider toolbar and build it.
  6. Open `Run -> Edit Configurations` and create or select a CMake Application. Set the target to `MoerEditor` and the working directory to the repository root, then choose Run or Debug.

  The Debug executable is written to `target/bin/Debug/MoerEditor.exe`. Create a separate Release CMake profile with Build type `Release`; changing only the run configuration does not reconfigure a Debug build as Release.

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
