# 如果CUDA成功导入，则cmake表达式 (TARGET moer_cuda) 为真；同时，链接moer_cuda后，cpp端会被设置宏 HAS_CUDA=1
# 
# 只有CUDA成功导入时，才考虑LibTorch和TensorRT（版本需要自己手动校验）
# - 如果LibTorch和TensorRT成功导入，则.......

find_package(CUDAToolkit QUIET)

if (CUDAToolkit_FOUND)
    message(STATUS "CUDA found: ${CUDAToolkit_VERSION}")

    if (NOT WIN32)
        message(WARNING "CUDA/LibTorch/TensorRT have not been tested on non-Windows platforms!")
    endif()

    # MARK: Import CUDA

    # 获取所有cu文件
    # CONFIGURE_DEPENDS，指如果文件变化了，则自动重新GLOB
    file(GLOB_RECURSE moer_cuda_cu CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cu")
    # file(GLOB_RECURSE moer_cuda_cuh CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cuh")
    # file(GLOB_RECURSE moer_cuda_h CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")

    # 编译一个CUDA的静态库，把上面这些cu文件都编译了
    add_library(moer_cuda SHARED ${moer_cuda_cu})

    # 设置目标属性
    set_target_properties(moer_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION OFF # 需要分离编译时打开；如果打开的话，编译无法通过，貌似是因为兼容性bug
        POSITION_INDEPENDENT_CODE ON # 编译动态库的时候需要打开
        LINKER_LANGUAGE CUDA # 不然cmake会提示：CMake can not determine linker language for target: moer_cuda
    )

    # 宏, link include, link sources
    target_compile_definitions(moer_cuda PUBLIC HAS_CUDA=1)
    target_compile_definitions(moer_cuda PRIVATE MOER_CUDA_EXPORTS) # 只在编译moer_cuda时，启用这个宏，保证导出符号正确性
    # 直接把当前文件夹作为include，这样开发的时候就不用把header和cu分开写了
    target_include_directories(moer_cuda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(moer_cuda PUBLIC ${CUDAToolkit_INCLUDE_DIRS})
    target_link_libraries(moer_cuda PUBLIC CUDA::cudart)

    # Reference: https://github.com/microsoft/vcpkg/issues/30272#issuecomment-1817929528
    # Hack for the extra \EHsc that gets added on Windows to the nvcc compile line, which causes the error:
    # "nvcc fatal: A single input file is required for a non-link phase when an outputfile is specified..."
    if(${MSVC})
        set_target_properties(moer_cuda PROPERTIES COMPILE_OPTIONS -Xcompiler)
    endif()
    # 这个语句会覆盖target_compile_options的设置，所以必须在target_compile_options之前执行

    # 其他编译选项
    # $<CONFIG:Debug>:..          # 如果是Debug模式，才添加下面的options
    # $<COMPILE_LANGUAGE:CUDA>:.. # 如果语言是CUDA，才添加下面的options
    target_compile_options(moer_cuda PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler "/utf-8">) # 支持utf-8
    target_compile_options(moer_cuda PRIVATE $<$<CONFIG:Debug>:$<$<COMPILE_LANGUAGE:CUDA>:-G;-lineinfo>>) # -G和-lineinfo，分别启动device端的调试信息和行号，便于debug

    # 【重要】其他library编译时，使用下面这个语句进行link
    # target_link_libraries(another_library PRIVATE moer_cuda)

    # 检查是否存在 LibTorch_TensorRT.cmake 文件

    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/LibTorch_TensorRT.cmake")
        message(STATUS "LibTorch_TensorRT.cmake found")
        include("${CMAKE_CURRENT_SOURCE_DIR}/LibTorch_TensorRT.cmake")

        message(STATUS "LibTorch_Dir = ${LIBTORCH_DIR}")
        message(STATUS "TensorRT_Dir = ${TENSORRT_DIR}")

        # import libtorch
        find_package(Torch REQUIRED PATHS "${LIBTORCH_DIR}/share/cmake/Torch")

        # import tensorRT
        find_path(TensorRT_include_dir NvInfer.h HINTS "${TENSORRT_DIR}/include")
        find_library(TensorRT_library nvinfer_10 HINTS "${TENSORRT_DIR}/lib")
        find_library(TensorRT_plugin_library nvinfer_plugin_10 HINTS "${TENSORRT_DIR}/lib")

        if (TensorRT_include_dir AND TensorRT_library AND TensorRT_plugin_library)
            add_library(TensorRT INTERFACE) # TODO: 整理cmake命名规范
            target_include_directories(TensorRT INTERFACE "${TensorRT_include_dir}")
            target_link_libraries(TensorRT INTERFACE
                "${TensorRT_library}"
                "${TensorRT_plugin_library}"
            )
        else()
            message(WARNING "TensorRT not found at ${TENSORRT_DIR}. TensorRT_include_dir=${TensorRT_include_dir}; TensorRT_library=${TensorRT_library}")
        endif()

        if (DEFINED TORCH_LIBRARIES) # linux下可能不是这么判断，可能是Torch::Torch
            message(STATUS "LibTorch has imported successfully. TORCH_LIBRARIES = ${TORCH_LIBRARIES}")

            # Use Torch to instead of ${TORCH_LIBRARIES}
            add_library(LibTorch INTERFACE)
            target_link_libraries(LibTorch INTERFACE ${TORCH_LIBRARIES})
        else()
            message(STATUS "LibTorch has not been imported.")
        endif()

        if (TARGET TensorRT)
            message(STATUS "TensorRT has imported successfully.")
        else()
            message(STATUS "TensorRT has not been imported.")
        endif()

        if ((TARGET LibTorch) AND (TARGET TensorRT))
            add_library(LibTorch_TensorRT INTERFACE)
            target_link_libraries(LibTorch_TensorRT INTERFACE
                ${TORCH_LIBRARIES}
                TensorRT
            )
            message(STATUS "CUDA, LibTorch, TensorRT all imported successfully.")
        endif()

    else()
        message(STATUS "LibTorch_TensorRT.cmake not found, building without LibTorch & TensorRT support.")
    endif()

else()
    message(STATUS "CUDA not found - building without CUDA & LibTorch & TensorRT support.")
endif()
