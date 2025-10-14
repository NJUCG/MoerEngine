cmake_minimum_required(VERSION 3.26.0)

include("${CMAKE_SOURCE_DIR}/cmake/load_config.cmake")

if (WITH_CUDA)
    project(MoerEngine LANGUAGES CXX C CUDA)
else()
    project(MoerEngine LANGUAGES CXX C)
endif()

set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 1)
set(PROJECT_VERSION_PATCH 0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(moer_root_dir ${CMAKE_CURRENT_SOURCE_DIR})
set(moer_source_dir ${moer_root_dir}/source) 
set(moer_third_party_dir "${moer_root_dir}/3rdparty")
set(moer_shader_dir "${moer_root_dir}/shaders")

# set(CMAKE_INSTALL_PREFIX "${moer_root_dir}/target/$<$<CXX_COMPILER_ID:MSVC>:>$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:$<CMAKE_BUILD_TYPE>>")
if(MSVC)
    set(CMAKE_INSTALL_PREFIX "${moer_root_dir}/target")
else()
    set(CMAKE_INSTALL_PREFIX "${moer_root_dir}/target/${CMAKE_BUILD_TYPE}")
    endif()

set(BINARY_ROOT_DIR "${CMAKE_INSTALL_PREFIX}")

# set ouput folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BINARY_ROOT_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${BINARY_ROOT_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${BINARY_ROOT_DIR}/lib)
set(CMAKE_PDB_OUTPUT_DIRECTORY ${BINARY_ROOT_DIR}/bin)

add_compile_definitions(EDITOR_MODE_ON=1)

option(moer_build_test "build test unit" ON)

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

function(add_subdirectory_silent dir)
    # 先全局静音
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL "")
    set(CMAKE_WARN_DEPRECATED OFF CACHE INTERNAL "")
    # 添加第三方库
    add_subdirectory(${dir})
    # 恢复默认设置，只对你自己源码目录有效
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 0 CACHE INTERNAL "")
    set(CMAKE_WARN_DEPRECATED ON CACHE INTERNAL "")
endfunction()

add_subdirectory_silent("3rdparty")
add_subdirectory("source")

