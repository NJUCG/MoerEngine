# 请修改下面两个变量
# 请务必对齐 LibTorch和TensorRT 与 CUDAToolkit 的版本
set(LIBTORCH_DIR "/path/to/libtorch"  CACHE PATH "LIBTORCH_DIR" FORCE)
set(TENSORRT_DIR "/path/to/tensor_rt" CACHE PATH "TENSORRT_DIR" FORCE)

# WITH_CUDA 只支持 Windows11 + Vulkan
# CMake的Cache变量会持续存在，直到删除build目录；FORCE用于覆盖之前的值
# 启用：
#set(WITH_CUDA ON CACHE BOOL "WITH_CUDA" FORCE)
# 关闭：
set(WITH_CUDA OFF CACHE BOOL "WITH_CUDA" FORCE)