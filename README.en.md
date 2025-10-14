# MoerEngine

Engine for Realtime Rendering

**| [简体中文](README.md) | English |**

## Dependencies

* OS
  * Windows 10, 11
* TODO

## How to Build & Run

- Command Line

  - Tip: If you don't have configured SSH, replace `git@xxx` with `https://github.com/NJUCG/MoerEngine.git`

  ```bash
  # Clone the repo
  git clone --branch dev-rhi-remake git@github.com:NJUCG/MoerEngine.git
  cd MoerEngine

  # Ignore some specific commits (formatting changes)
  git config --local blame.ignoreRevsFile .git-blame-ignore-revs
  
  # Download Sponza scene to `./asset/scenes/`
  git clone --branch sponza-scene-files --depth 1 git@github.com:NJUCG/MoerEngine.git ./asset/scenes/sponza
  
  # Configure the engine (use default settings)
  cp source/configs/template.MoerEngine.toml source/configs/MoerEngine.toml
  
  # Build
  cmake -B build
  cmake --build build -j16 # change 16 to your cpu core count
  
  # Run
  # - MoerEditor.exe needs to be modified according to your platform
  # - You can switch from Raster to RayTracing by GUI or Config.
  ./target/bin/Debug/MoerEditor.exe
  ```

  - If you have installed `just`, you can run `just` to build and run the engine directly.
    - [just](https://github.com/casey/just)

- Rider
  - TODO

### CUDA, LibTorch, and TensorRT

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


## How to render other scenes

- How to render other scenes
  - If you succeed to run Moer Engine, open the configuration file `./target/bin/Debug/config/MoerEngine.toml`.
  - Then, change `scene_path` to any other scene files in your machine.
    - Such as `scene_path = E:\Data\Models\BlueArchive\aris\CH0200.fbx.`
  - Run `./target/bin/Debug/MoerEditor.exe` again, you will see the new scene.
- Tip
  - `./target/bin/Debug/config/*` will be replaced with `./source/configs/*`, every time you compile.

## Camera Controlling

- The camera control method of **Moer Engine** is very similar to Unreal Engine's.
  - When dragging the mouse, press `W/A/S/D/Q/E` to **move the camera**.
  - Drag `right mouse button` to **rotate the camera**.
  - Drag `left mouse button` to rotate the camera and move forward/backward.
  - Drag `both mouse buttons` or `middle button` to move the camera.
- Press `F` to control camera without dragging.
- Press `↑` or `↓` to change the **speed** of camera.
- Scroll the mouse wheel to change **FOV**.