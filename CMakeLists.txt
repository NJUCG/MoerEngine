##################################################
## CMake
##################################################
cmake_minimum_required(VERSION 3.26.0)


##################################################
## Project
##################################################
project(Moer2)

set(CMAKE_CXX_STANDARD 20)
set(MOER2_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(ENGINE_ROOT_DIR ${MOER2_ROOT_DIR}/source)

set(CMAKE_INSTALL_PREFIX "${MOER2_ROOT_DIR}/target")
set(BINARY_ROOT_DIR "${CMAKE_INSTALL_PREFIX}")

add_definitions(-DASSET_DIR="${MOER2_ROOT_DIR}/asset")

if(${CMAKE_BUILD_TYPE} MATCHES "Debug")
    #set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
    add_definitions(-DDEBUG)
endif()

add_subdirectory("source")