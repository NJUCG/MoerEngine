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
  
* Rider
  * TODO

## How to render other scenes

* How to render other scenes
  * If you succeed to run Moer Engine, open the configuration file `./target/bin/Debug/config/MoerEngine.toml`.
  * Then, change `scene_path` to any other scene files in your machine.
    * Such as `scene_path = E:\Data\Models\BlueArchive\aris\CH0200.fbx.`
  * Run `./target/bin/Debug/MoerEditor.exe` again, you will see the new scene.
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

## Project Structure Notes

* 写一些关于项目结构的注释（后面应该要移到 `doc/` 里面去）

### CMakeLists

* 所有library（通过add_library添加的东西）
  * MoerRuntime、MoerCore、MoerIO、MoerRender、MoerResource
  * 其中，MoerRuntime被其他四个库public链接了，同时，每个库都public include了很多头文件目录
    * 所以，MoerEditor只链接了MoerRuntime，并且不需要添加runtime相关的头文件目录
* 所有executable（通过add_executable添加的东西）
  * MoerEditor、source/test文件夹下的一大团东西

### MoerEditor

* 2025.3.13左右，重构了一下MoerEditor

* **为什么新Editor和Runtime耦合在一起？**

  * 不独立开写的原因是：① 目前需要UI来在Raster和Raytracing两种渲染模式之间切换，而这个切换信息是需要UI来选择的；② 目前想不到什么不需要UI独立运行的场景
  * 独立开写，在技术上也是可以实现的。目前耦合性其实不高

* **如何实现的切换Raster和Raytracing两种渲染方法**？

  * 切换时，只保留特定对象，其他数据（包括场景）直接全部重新初始化
    * 注：场景会直接重新读取SceneCache
  * 具体内容参见 `source/editor/Editor.cpp`，代码很短

* **为什么用namespace独立开 Raster和Raytracing代码？**

  * 注：Moer::Render::Raster、Moer::Render::Raytracing
  * 原因 ①：这两个部分都是接近程序入口的代码，十分末端，不应该被其他模块再调用。所以独立成namespace是不影响后续开发的（希望如此）
  * 原因 ②：LightingPass和一些Pass会重名，导致编译错误

* **为什么把原来代码直接废弃了？**

  * ~~因为改不动，太难了~~
  * 因为重构时，能跑的两个程序入口RHITest和RaytracingTest，和原有的MoerEditor结构差距太大。并且Engine.cpp/h也没用。秉承着化繁为简的思路，我就直接把旧的、不可用的程序入口废弃了。现在代码入口也更清晰明了。

* **为什么把 .h 和 .cpp 放在同一个目录下？**

  * ~~我懒了~~
  * 我其实不太了解为什么.h和.cpp要分开存放。我的理解里，只有做基础库的时候，才需要这么做？为了用户调用方便。.h和.cpp塞在同一个目录下，开发起来会舒服一些。

* **目前MoerEditor的目录结构（只包含目录）**

  ```
  editor
  - common
  - raster
  - raytracing
  - ui
    - raster_ui
    - raytracing_ui
  ```

  * 其中，`common`、`raster`、`raytracing` 分别是两个渲染方法的入口和公共代码
  * 其中，`ui` 储存了所有ui相关的代码
    * 每个渲染方法单独的UI（以下称为SubUI），以 *组合* 的形式嵌入主UI
    * 每个SubUI只需要实现
      * struct Config —— 数据
      * void ShowConfig() —— 操作数据的UI
    * 在主UI中，会public抛出SubUI的config，并调用SubUI的ShowConfig()

* TODO

  * editor模块中，残留了非常多的TODO和FIXME，需要解决

  * 两个渲染方法，仍有许多代码可以合并，需要合并一下

  * 在切换渲染方法时，保留摄像机数据

    * 实现方法一：在主UI（EditorUI）中加对应数据段
    * 实现方法二：直接修改场景数据（缓存文件）；切换渲染方法时，会直接重新从缓存中加载场景数据

  * RHITest的normal，看了下，貌似解压有问题，色调不对

  * Raytracing的NRD貌似有一点小问题

    * 第一个是 `NRD_INTEGRATION_ASSERT(isNormalRoughnessFormatValid, "IN_NORMAL_ROUGHNESS format doesn't match NRD normal encoding");` 这个assert会被触发

    * 第二个是，重置屏幕大小后，再切换渲染方法，m_FrameIndex的数值貌似不对。可能是调用问题，我现在硬编码直接修改了

        