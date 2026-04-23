# 如果需要启用某个Feature，请将 `OFF` 改为 `ON`，并且设置对应路径变量

# ============================================================
# CUDA Support
# 请务必对齐 LibTorch和TensorRT 与 CUDAToolkit 的版本
# WITH_CUDA 只支持 Windows11 + Vulkan
# CMake的Cache变量会持续存在，直到删除build目录；FORCE用于覆盖之前的值
# ============================================================
set(WITH_CUDA OFF CACHE BOOL "WITH_CUDA" FORCE)
set(LIBTORCH_DIR "/path/to/libtorch"  CACHE PATH "LIBTORCH_DIR" FORCE)
set(TENSORRT_DIR "/path/to/tensor_rt" CACHE PATH "TENSORRT_DIR" FORCE)

# ============================================================
# NRD Denoiser
# ============================================================
set(WITH_NRD OFF CACHE BOOL "WITH_NRD" FORCE)
set(NRD_ROOT "${CMAKE_SOURCE_DIR}/3rdparty/NRD"  CACHE PATH "NRD_ROOT" FORCE)

# ============================================================
# Perfetto Profiling
# ============================================================
set(WITH_PROFILE OFF CACHE BOOL "WITH_PROFILE" FORCE)

# ============================================================
# RenderDoc
# ============================================================
set(WITH_RENDERDOC OFF CACHE BOOL "WITH_RENDERDOC" FORCE)
set(RENDERDOC_ROOT "/path/to/RenderDoc"  CACHE PATH "RENDERDOC_ROOT" FORCE)
