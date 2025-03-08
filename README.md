# MoerEngine
Engine for Realtime Rendering

## Current Capability
- Render Hardware Interface(vulkan implemented)
- Multi-Thread TaskSystem
- Math
- RHI implemented ui backend
- Deferred gui rendering
- Shader pipeline(compiling, reflection and binding)

## TODO

### Critical

- GPUScene management
- Camera Manager
- Material System design and implementation
- Bindless support
  
### Pending

- Scene Management
- Offline mesh clusterize
- Default pipeline
- DX12 support
- dxc and metis on linux

## How to Build & Run

* Command Line

  * Tip: If you don't have configured SSH, replace `git@xxx` with `https://github.com/NJUCG/MoerEngine.git`

  ```bash
  # Clone the repo
  git clone --branch dev-rhi-remake git@github.com:NJUCG/MoerEngine.git
  cd MoerEngine
  
  # Download Sponza scene to `./asset/scenes/`
  git clone --branch sponza-scene-files --depth 1 git@github.com:NJUCG/MoerEngine.git ./asset/scenes/
  
  # Configure the engine (use default settings)
  cp source/configs/MoerEngine.ini.template source/configs/MoerEngine.ini
  
  # Build
  cmake -B build
  cmake --build build -j16 # change 16 to your cpu core count
  
  # Run `Raster`
  # - `RHITest.exe` needs to be modified according to your platform
  ./target/bin/Debug/RHITest.exe
  
  # Run `Ray Tracing`
  # - `RTTest.exe` needs to be modified according to your platform
  ./target/bin/Debug/RTTest.exe
  ```

* Rider
  * TODO

## How to render other scenes

* How to render other scenes
  * If you succeed to run Moer Engine, open the configuration file `./target/bin/Debug/config/MoerEngine.ini`.
  * Then, change `scene_path` to any other scene files in your machine.
    * Such as `scene_path = E:\Data\Models\BlueArchive\aris\CH0200.fbx.`
  * Run `./target/bin/Debug/RHITest.exe` again, you will see the new scene.
* Tip
  * `./target/bin/Debug/config/*` will be replaced with `./source/configs/*`, every time you compile.

## Camera Controlling

* The camera control method of **Moer Engine** is very similar to Unreal Engine's.
  * When dragging the mouse, press `W/A/S/D/Q/E` to **move the camera**.
  * Drag `right mouse button` to **rotate the camera**.
  * Drag `left mouse button` to rotate the camera and move forward/backward.
  * Drag `both mouse buttons` or `middle button` to move the camera.
* Press `F` to control camera without dragging.
* Press `↑` or `↓` to change the **speed** of camera.
* Scroll the mouse wheel to change **FOV**.