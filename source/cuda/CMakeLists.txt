

find_package(CUDAToolkit QUIET)

if (CUDAToolkit_FOUND)
    message(STATUS "CUDA found: ${CUDAToolkit_VERSION}")

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

else()
    message(STATUS "CUDA not found - building without CUDA support")
endif()
