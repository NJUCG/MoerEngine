cmake_minimum_required(VERSION 3.26.0)

project(MoerEngine LANGUAGES CXX CUDA)
set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 1)
set(PROJECT_VERSION_PATCH 0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(moer_root_dir ${CMAKE_CURRENT_SOURCE_DIR})
set(moer_source_dir ${moer_root_dir}/source)

set(CMAKE_INSTALL_PREFIX "${moer_root_dir}/target")
set(BINARY_ROOT_DIR "${CMAKE_INSTALL_PREFIX}")

# set ouput folder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${BINARY_ROOT_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${BINARY_ROOT_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${BINARY_ROOT_DIR}/lib)

add_definitions(-DASSET_DIR="${moer_root_dir}/asset")

option(moer_build_test "build test unit" ON)

if(${MSVC})
    add_compile_options("/Zc:preprocessor")
endif()

add_subdirectory("source")