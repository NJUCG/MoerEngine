
# CUDA Support

# template.EnableCuda.cmake
set(WITH_CUDA OFF CACHE BOOL "WITH_CUDA" FORCE)
set(LIBTORCH_DIR "/path/to/libtorch" CACHE PATH "LIBTORCH_DIR" FORCE)
set(TENSORRT_DIR "/path/to/tensorrt" CACHE PATH "TENSORRT_DIR" FORCE)

if (EXISTS "${CMAKE_SOURCE_DIR}/EnableCuda.cmake")
    include("${CMAKE_SOURCE_DIR}/EnableCuda.cmake")
    
    message(STATUS "EnableCuda.cmake found")
else()
    message(STATUS "EnableCuda.cmake not found")
endif()

# template.EnableNrd.cmake
set(WITH_NRD OFF CACHE BOOL "WITH_NRD" FORCE)
set(NRD_ROOT "/path/to/nrd"  CACHE PATH "NRD_ROOT" FORCE)

if (EXISTS "${CMAKE_SOURCE_DIR}/EnableNrd.cmake")
    include("${CMAKE_SOURCE_DIR}/EnableNrd.cmake")
    
    message(STATUS "EnableNrd.cmake found")
else()
    message(STATUS "EnableNrd.cmake not found")
endif()