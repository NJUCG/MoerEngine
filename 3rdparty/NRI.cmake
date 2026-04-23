set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_NONE_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_VK_SUPPORT ON CACHE BOOL "" FORCE)
set(NRI_ENABLE_VALIDATION_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D11_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D12_SUPPORT OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_D3D_EXTENSIONS OFF CACHE BOOL "" FORCE)
set(NRI_ENABLE_AGILITY_SDK_SUPPORT OFF CACHE BOOL "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_VULKAN_HEADERS "${CMAKE_SOURCE_DIR}/3rdparty/VulkanSDK" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_VMA "${CMAKE_SOURCE_DIR}/3rdparty/VulkanMemoryAllocator" CACHE PATH "" FORCE)

add_subdirectory(NRI)

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
