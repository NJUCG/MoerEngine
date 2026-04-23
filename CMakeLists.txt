cmake_minimum_required(VERSION 3.26.0)

# 加载用户配置
include("${CMAKE_SOURCE_DIR}/cmake/load_config.cmake")

# 项目相关
if (WITH_CUDA)
    project(MoerEngine LANGUAGES CXX C CUDA)
else()
    project(MoerEngine LANGUAGES CXX C)
endif()

set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 1)
set(PROJECT_VERSION_PATCH 0)

# 路径
set(moer_root_dir ${CMAKE_CURRENT_SOURCE_DIR})
set(moer_source_dir ${moer_root_dir}/source) 
set(moer_third_party_dir "${moer_root_dir}/3rdparty")
set(moer_shader_dir "${moer_root_dir}/shaders")

# 构建相关
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# 统一输出路径：MSVC 与 Clang(Ninja) 均使用 target/bin/<Config>/ 与 target/lib/<Config>/
# $<CONFIG> 在 multi-config(MSVC) 下为构建时选择的配置，在 single-config(Ninja) 下为 CMAKE_BUILD_TYPE
set(CMAKE_INSTALL_PREFIX "${moer_root_dir}/target")
set(BINARY_ROOT_DIR "${moer_root_dir}/target")

# set output folder (使用 generator expression 保证两种生成器输出目录一致)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}/bin/$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}/lib/$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}/lib/$<CONFIG>")
set(CMAKE_PDB_OUTPUT_DIRECTORY "${BINARY_ROOT_DIR}/bin/$<CONFIG>")

add_compile_definitions(EDITOR_MODE_ON=1)

option(moer_build_test "build test unit" ON)
option(moer_enable_trace "Enable profiler trace instrumentation" ON)

if(moer_enable_trace)
    add_compile_definitions(MOER_TRACE_ENABLED=$<IF:$<CONFIG:Release>,0,1>)
else()
    add_compile_definitions(MOER_TRACE_ENABLED=0)
endif()
add_compile_definitions(MOER_TRACE_GPU_ENABLED=1)
add_compile_definitions(MOER_TRACE_CSV_ENABLED=1)

if(${MSVC})
    add_compile_options("/Zc:preprocessor")
    add_compile_options("/wd5105")
    add_compile_options("/wd5104")
    add_compile_options("/wd4267") # 禁用MSVC C4267警告 (size_t -> int)
    add_compile_options("/FS")
    add_compile_options("/MP")
    # Support Chinese Comments
    add_compile_options("$<$<C_COMPILER_ID:MSVC>:/utf-8>")
    add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")
endif()

# 子目录：3rdparty

include(${CMAKE_SOURCE_DIR}/cmake/functions.cmake)
add_subdirectory_silent("3rdparty")

# 子目录：source

add_subdirectory("source")
