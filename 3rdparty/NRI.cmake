set(NRI_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/NRI")
set(NRI_OVERLAY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_overlay/NRI")
set(NRI_VMA_OVERLAY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_overlay/VulkanMemoryAllocator")

# Keep the submodule pristine: copy upstream NRI into a build-local overlay and patch only the overlay.
file(REMOVE_RECURSE "${NRI_OVERLAY_ROOT}" "${NRI_VMA_OVERLAY_ROOT}")
file(MAKE_DIRECTORY "${NRI_OVERLAY_ROOT}" "${NRI_VMA_OVERLAY_ROOT}/include")

file(GLOB NRI_OVERLAY_CONTENT RELATIVE "${NRI_SOURCE_ROOT}" "${NRI_SOURCE_ROOT}/*")
foreach(nri_entry IN LISTS NRI_OVERLAY_CONTENT)
    file(COPY "${NRI_SOURCE_ROOT}/${nri_entry}" DESTINATION "${NRI_OVERLAY_ROOT}")
endforeach()

# Upstream NRI expects a VMA source root that contains include/vk_mem_alloc.h, while this repo vendors only the header.
configure_file(
    "${CMAKE_SOURCE_DIR}/3rdparty/VulkanMemoryAllocator/vk_mem_alloc.h"
    "${NRI_VMA_OVERLAY_ROOT}/include/vk_mem_alloc.h"
    COPYONLY
)

set(NRI_SHARED_CPP "${NRI_OVERLAY_ROOT}/Source/Shared/Shared.cpp")
file(READ "${NRI_SHARED_CPP}" NRI_SHARED_CPP_CONTENT)
# clang on Windows needs Windows.h visible before the shared source reaches Win32 declarations via upstream includes.
string(REPLACE
    "#ifndef _WIN32\n#    include <csignal> // raise\n#    include <cstdarg> // va_start, va_end\n#endif"
    "#ifdef _WIN32\n#    include <Windows.h>\n#    include <cstdarg> // va_start, va_end\n#else\n#    include <csignal> // raise\n#    include <cstdarg> // va_start, va_end\n#endif"
    NRI_SHARED_CPP_CONTENT
    "${NRI_SHARED_CPP_CONTENT}"
)
file(WRITE "${NRI_SHARED_CPP}" "${NRI_SHARED_CPP_CONTENT}")

set(NRI_RESOURCE_FILE "${NRI_OVERLAY_ROOT}/Resources/NRI.rc")
file(READ "${NRI_RESOURCE_FILE}" NRI_RESOURCE_FILE_CONTENT)
# llvm-rc on the clang+ninja path rejects the upstream copyright symbol; rewrite it to ASCII in the overlay only.
string(REPLACE
    "Copyright © 2021"
    "Copyright (C) 2021"
    NRI_RESOURCE_FILE_CONTENT
    "${NRI_RESOURCE_FILE_CONTENT}"
)
file(WRITE "${NRI_RESOURCE_FILE}" "${NRI_RESOURCE_FILE_CONTENT}")

set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NONE_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_VK_SUPPORT ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_VALIDATION_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_AGILITY_SDK_SUPPORT OFF CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS "${CMAKE_SOURCE_DIR}/3rdparty/VulkanSDK" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_VMA "${NRI_VMA_OVERLAY_ROOT}" CACHE PATH "" FORCE)

add_subdirectory("${NRI_OVERLAY_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/NRI")

# 修复NRI使用D3D12AgilitySDK，而不是系统WindowsSDK的问题
if(TARGET NRI_D3D12)
    target_include_directories(NRI_D3D12 BEFORE PRIVATE 
        "${MOER_AGILITYSDK_DIR}/build/native/include"
    )
endif()

target_compile_options(NRI
PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
INTERFACE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"

)
if(TARGET NRI_Shared)
    target_compile_options(NRI_Shared
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    )
endif()

if(TARGET NRI_NONE)
    target_compile_options(NRI_NONE
    PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    )
endif()

if(TARGET NRI_D3D11)
    target_compile_options(
        NRI_D3D11
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-microsoft-extra-qualification>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-ignored-attributes>"
    )
endif()

if(TARGET NRI_D3D12)
    target_compile_options(
        NRI_D3D12
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-nullability-completeness>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-ignored-attributes>"
    )
endif()

if(TARGET NRI_VK)
    target_compile_options(
        NRI_VK
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-deprecated-declarations>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-function>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-tautological-undefined-compare>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-nullability-completeness>"
    )
endif()

if(TARGET NRI_Validation)
    target_compile_options(
        NRI_Validation
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-undefined-bool-conversion>"
        PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
    )
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-msse4.1)
endif()
