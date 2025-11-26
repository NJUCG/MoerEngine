# 如果CUDA成功导入，则cmake表达式 (TARGET Moer::Cuda) 为真；同时，链接moer_cuda后，cpp端会被设置宏 WITH_CUDA=1
# 
# 只有CUDA成功导入时，才考虑LibTorch和TensorRT（版本需要自己手动校验）
# - 如果LibTorch和TensorRT成功导入，则.......

if (WITH_CUDA)
    message(STATUS "CMake WITH_CUDA == ON")

    find_package(CUDAToolkit QUIET)
    message(STATUS "CUDA found: ${CUDAToolkit_VERSION}. Use `CUDA::cudart` to link.")

    if (NOT WIN32)
        message(WARNING "CUDA/LibTorch/TensorRT have not been tested on non-Windows platforms!")
    endif()

    message(STATUS "LibTorch_Dir = ${LIBTORCH_DIR}")
    message(STATUS "TensorRT_Dir = ${TENSORRT_DIR}")

    # import libtorch
    find_package(Torch REQUIRED PATHS "${LIBTORCH_DIR}/share/cmake/Torch" QUIET)

    # import tensorRT
    find_path(TensorRT_include_dir NvInfer.h HINTS "${TENSORRT_DIR}/include" QUIET)
    find_library(TensorRT_library nvinfer_10 HINTS "${TENSORRT_DIR}/lib" QUIET)
    find_library(TensorRT_plugin_library nvinfer_plugin_10 HINTS "${TENSORRT_DIR}/lib" QUIET)
    find_library(TensorRT_onnx_parser nvonnxparser_10 HINTS "${TENSORRT_DIR}/lib" QUIET)

    if (TensorRT_include_dir AND TensorRT_library AND TensorRT_plugin_library)
        add_library(TensorRT INTERFACE) # TODO: 整理cmake命名规范
        target_include_directories(TensorRT INTERFACE "${TensorRT_include_dir}")
        target_link_libraries(TensorRT INTERFACE
            "${TensorRT_library}"
            "${TensorRT_plugin_library}"
            "${TensorRT_onnx_parser}"
        )
    else()
        message(WARNING "TensorRT not found at ${TENSORRT_DIR}. TensorRT_include_dir=${TensorRT_include_dir}; TensorRT_library=${TensorRT_library}")
    endif()

    if (DEFINED TORCH_LIBRARIES) # linux下可能不是这么判断，可能是Torch::Torch
        # Use LibTorch to instead of ${TORCH_LIBRARIES}
        add_library(LibTorch INTERFACE)
        target_link_libraries(LibTorch INTERFACE ${TORCH_LIBRARIES})

        message(STATUS "LibTorch has imported successfully. Use `LibTorch` to link. TORCH_LIBRARIES = ${TORCH_LIBRARIES}")
    else()
        message(STATUS "LibTorch has not been imported.")
    endif()

    if (TARGET TensorRT)
        message(STATUS "TensorRT has imported successfully. Use `TensorRT` to link.")
    else()
        message(STATUS "TensorRT has not been imported.")
    endif()

else()
    message(STATUS "CMake WITH_CUDA == OFF")
endif()

# 是否找到全部三个依赖
if (TARGET CUDA::cudart AND TARGET LibTorch AND TARGET TensorRT)
    # MARK: Compile Moer CUDA

    # 获取所有cu文件
    # CONFIGURE_DEPENDS，指如果文件变化了，则自动重新GLOB
    file(GLOB_RECURSE moer_cuda_cu CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cu")
    # file(GLOB_RECURSE moer_cuda_cuh CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cuh")
    file(GLOB_RECURSE moer_cuda_h CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

    # 编译一个CUDA的静态库，把上面这些cu文件都编译了（.h文件会被自动忽略）
    set(target_name moer_cuda)
    add_library(${target_name} SHARED ${moer_cuda_h} ${moer_cuda_cu})
    add_library(Moer::Cuda ALIAS ${target_name})

    # 设置目标属性
    set_target_properties(${target_name} PROPERTIES
        CUDA_SEPARABLE_COMPILATION OFF # 需要分离编译时打开；如果打开的话，编译无法通过，貌似是因为兼容性bug
        POSITION_INDEPENDENT_CODE ON   # 编译动态库的时候需要打开
        LINKER_LANGUAGE CUDA           # 不然cmake会提示：CMake can not determine linker language for target: ${target_name}
        CUDA_ARCHITECTURES "61;86;120" # 支持非本机的计算架构（例如3060和3080的Compute Capability都是8.6）
                                       # NV计算能力表格：https://developer.nvidia.cn/cuda-gpus
    )

    # 宏, link include, link sources
    target_compile_definitions(${target_name} PRIVATE MOER_CUDA_EXPORTS) # 只在编译moer_cuda时，启用这个宏，保证导出符号正确性
    # 直接把当前文件夹作为include，这样开发的时候就不用把header和cu分开写了
    target_include_directories(${target_name} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(${target_name} PUBLIC ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(${target_name} PUBLIC CUDA::cudart CUDA::cublas CUDA::cusolver LibTorch TensorRT)
    # Reference: https://github.com/microsoft/vcpkg/issues/30272#issuecomment-1817929528
    # Hack for the extra \EHsc that gets added on Windows to the nvcc compile line, which causes the error:
    # "nvcc fatal: A single input file is required for a non-link phase when an outputfile is specified..."
    if(${MSVC})
        set_target_properties(${target_name} PROPERTIES COMPILE_OPTIONS -Xcompiler)
    endif()
    # 这个语句会覆盖target_compile_options的设置，所以必须在target_compile_options之前执行

    # 其他编译选项
    # $<CONFIG:Debug>:..          # 如果是Debug模式，才添加下面的options
    # $<COMPILE_LANGUAGE:CUDA>:.. # 如果语言是CUDA，才添加下面的options
    target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler;"/utf-8"> # 支持utf-8
        $<$<COMPILE_LANGUAGE:CUDA>:-diag-suppress=221> # 禁用float溢出警告，例如：((float)(1e+300))
        $<$<CONFIG:Debug>:$<$<COMPILE_LANGUAGE:CUDA>:-G>> # -G启动device端的调试信息
    )

    # 【重要】其他library编译时，使用下面这个语句进行link
    # target_link_libraries(another_library PRIVATE ${target_name})

    message(STATUS "Moer::Cuda has been configured successfully.")

    # 检查PATH中是否包含了LibTorch和TensorRT的lib路径，避免程序直接闪退
    if (WIN32)
        set(LIBTORCH_LIB_DIR "${LIBTORCH_DIR}\\lib")
        set(TENSORRT_LIB_DIR "${TENSORRT_DIR}\\lib")

        file(TO_NATIVE_PATH "${LIBTORCH_LIB_DIR}" LIBTORCH_LIB_DIR)
        file(TO_NATIVE_PATH "${TENSORRT_LIB_DIR}" TENSORRT_LIB_DIR)
        file(TO_NATIVE_PATH "$ENV{PATH}" ENV_PATH)

        # 在 PATH 中查找
        string(FIND "${ENV_PATH}" "${LIBTORCH_LIB_DIR}" FOUND_LIBTORCH_LIB_DIR)
        string(FIND "${ENV_PATH}" "${TENSORRT_LIB_DIR}" FOUND_TENSORRT_LIB_DIR)

        if (FOUND_LIBTORCH_LIB_DIR EQUAL -1)
            message(WARNING
                "LibTorch DLL path not found in PATH: ${LIBTORCH_LIB_DIR}\n"
                "Your program may crash at runtime unless you add this to PATH.")
        endif()
        if (FOUND_TENSORRT_LIB_DIR EQUAL -1)
            message(WARNING
                "TensorRT DLL path not found in PATH: ${TENSORRT_LIB_DIR}\n"
                "Your program may crash at runtime unless you add this to PATH.")
        endif()
    endif()

else()
    message(STATUS "Moer::Cuda has been skipped because lacking of dependencies.")
endif()

if (WITH_CUDA AND NOT (TARGET CUDA::cudart AND TARGET LibTorch AND TARGET TensorRT))
    message(FATAL_ERROR "Enabled CUDA pass in raster, but doesn't have dependencies!")
    # FATAL_ERROR: Stop cmake generation process
endif()
# 从此以后，只要WITH_CUDA开启，那么依赖就一定准备完毕