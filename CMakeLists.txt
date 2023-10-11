##################################################
## CMake
##################################################
cmake_minimum_required(VERSION 3.26.0)


##################################################
## Project
##################################################
project(MoerEngine)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(MOER_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(ENGINE_ROOT_DIR ${MOER_ROOT_DIR}/source)

set(CMAKE_INSTALL_PREFIX "${MOER_ROOT_DIR}/target")
set(BINARY_ROOT_DIR "${CMAKE_INSTALL_PREFIX}")

add_definitions(-DASSET_DIR="${MOER_ROOT_DIR}/asset")

if(${MSVC})
    add_compile_options("/Zc:preprocessor")
endif()

add_subdirectory("source")