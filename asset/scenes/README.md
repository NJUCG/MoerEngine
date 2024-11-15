# Scenes

## Get Started

* You can download scenes from [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) and place them in this directory.
* Below is an example of how to use the Sponza scene.
  1. Clone [glTF-Sample-Models](https://github.com/KhronosGroup/glTF-Sample-Models) to somewhere on your machine (It's not recommended to clone it inside this project, as git may track it as a submodule).
  2. Copy or move the contents of `glTF-Sample-Models/2.0/Sponza/glTF` into `./asset/scenes/sponza`. 
  3. Let `scene_path=./asset/scenes/sponza/Sponza.gltf` in `./source/configs/MoerEngine.ini`.
    * If you don't have `MoerEngine.ini` in that directory, you need to copy `MoerEngine.ini.template` to `MoerEngine.ini` and modify it.
  4. Run MoerEngine.

## About .gitignore

* Git will ignore all subdirectories in this directory.
* So, you can place the scenes in this directory without worrying about accidentally committing them to the repository.