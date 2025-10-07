# 请修改下面两个变量
# 请务必对齐 LibTorch和TensorRT 与 CUDAToolkit 的版本
set(LIBTORCH_DIR "/path/to/libtorch")
set(TENSORRT_DIR "/path/to/libtorch")

# CUDA_PASS_IN_RASTER 只支持 Windows11 + Vulkan
# FORCE覆盖之前的值
set(CUDA_PASS_IN_RASTER false CACHE STRING "CUDA_PASS_IN_RASTER" FORCE)