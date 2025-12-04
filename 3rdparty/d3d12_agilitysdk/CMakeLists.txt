
cmake_minimum_required(VERSION 3.15)
project(d3d12_agilitysdk)

include(FetchContent)

# 1. 定义版本号 (保持你的原逻辑)
set(MOER_AGILITYSDK_VERSION_MAJOR "715" CACHE INTERNAL "")
set(MOER_AGILITYSDK_VERSION_MINOR "1"   CACHE INTERNAL "")
set(MOER_AGILITYSDK_VERSION_IS_PREVIEW TRUE CACHE INTERNAL TRUE)
set(MOER_AGILITYSDK_VERSION_ID "1.${MOER_AGILITYSDK_VERSION_MAJOR}.${MOER_AGILITYSDK_VERSION_MINOR}" CACHE INTERNAL "")

if(MOER_AGILITYSDK_VERSION_IS_PREVIEW)
    set(MOER_AGILITYSDK_VERSION_ID "${MOER_AGILITYSDK_VERSION_ID}-preview" CACHE INTERNAL "")
endif()

# 2. [关键修改] 从 NuGet 自动下载 SDK，而不是使用本地路径
#    NuGet 下载链接格式通常是固定的
message(STATUS "Preparing to fetch D3D12 Agility SDK version: ${MOER_AGILITYSDK_VERSION_ID}")

FetchContent_Declare(
    AgilitySDK_Nuget
    URL "https://www.nuget.org/api/v2/package/Microsoft.Direct3D.D3D12/${MOER_AGILITYSDK_VERSION_ID}"
    # NuGet 包本质是 ZIP，CMake 会自动解压
)
FetchContent_MakeAvailable(AgilitySDK_Nuget)

# 3. [关键修改] 重定向目录变量
#    原本指向 CMAKE_CURRENT_SOURCE_DIR，现在指向下载解压后的目录
set(MOER_AGILITYSDK_DIR "${agilitysdk_nuget_SOURCE_DIR}" CACHE INTERNAL "")
set(MOER_AGILITYSDK_DLL_DIR "${MOER_AGILITYSDK_DIR}/build/native/bin/x64" CACHE INTERNAL "")

# 4. 保持你的原有函数逻辑不变
#    注意：这里依赖外部变量 'real_out_put_dir'，请确保在调用前已定义
function(deploy_agilitysdk target_name)
    if(NOT TARGET copy_dll_${target_name})
        add_custom_target(copy_dll_${target_name})
        set_target_properties(copy_dll_${target_name} PROPERTIES FOLDER "utils")
    endif()

    add_library(${target_name} INTERFACE)
    target_include_directories(${target_name} INTERFACE "${MOER_AGILITYSDK_DIR}/build/native/include")
 
    set(dll
        "${MOER_AGILITYSDK_DLL_DIR}/D3D12Core.dll"
        "${MOER_AGILITYSDK_DLL_DIR}/D3D12Core.pdb"
        "${MOER_AGILITYSDK_DLL_DIR}/d3d12SDKLayers.dll"
        "${MOER_AGILITYSDK_DLL_DIR}/d3d12SDKLayers.pdb"
    )

    # 检查 real_out_put_dir 是否定义，防止报错
    if(NOT DEFINED real_out_put_dir)
        message(WARNING "Variable 'real_out_put_dir' is not defined! DLLs might be copied to a wrong location.")
    endif()

    message(STATUS "Copying Agility SDK DLLs to: ${real_out_put_dir}/D3D12")
    
    add_custom_command(
        TARGET copy_dll_${target_name}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${real_out_put_dir}/D3D12"
        COMMAND 
            ${CMAKE_COMMAND} -E copy_if_different
            ${dll} # 这里可以直接展开 list
            "${real_out_put_dir}/D3D12"
        COMMAND_EXPAND_LISTS
    )

    set_target_properties(${target_name} 
        PROPERTIES IMPORTED_LOCATION "${MOER_AGILITYSDK_DLL_DIR}/D3D12Core.dll") # 修正：指向具体文件更规范

    add_dependencies(${target_name} copy_dll_${target_name})

endfunction()

# 5. 调用函数
deploy_agilitysdk(d3d12_agilitysdk)

# 6. 编译辅助静态库 (源文件现在来自下载目录)
#    注意：NuGet包里确实包含 build/native/src/d3dx12 源码
add_library(d3d12_agilitysdk_d3dx12  
    STATIC 
    "${MOER_AGILITYSDK_DIR}/build/native/src/d3dx12/d3dx12_property_format_table.cpp"
)

target_include_directories(d3d12_agilitysdk_d3dx12 
    PUBLIC
    "${MOER_AGILITYSDK_DIR}/build/native/include/d3dx12")
    
target_link_libraries(d3d12_agilitysdk_d3dx12 PRIVATE d3d12_agilitysdk)

# ==========================================
# = Deprecated
# ==========================================

# project(d3d12_agilitysdk)
 
# set(MOER_AGILITYSDK_VERSION_MAJOR "715" CACHE INTERNAL "")
# set(MOER_AGILITYSDK_VERSION_MINOR "1"   CACHE INTERNAL "")
# set(MOER_AGILITYSDK_VERSION_IS_PREVIEW TRUE CACHE INTERNAL TRUE)
# set(MOER_AGILITYSDK_VERSION_ID "1.${MOER_AGILITYSDK_VERSION_MAJOR}.${MOER_AGILITYSDK_VERSION_MINOR}" CACHE INTERNAL "")
# if(MOER_AGILITYSDK_VERSION_IS_PREVIEW)
#     set(MOER_AGILITYSDK_VERSION_ID "${MOER_AGILITYSDK_VERSION_ID}-preview" CACHE INTERNAL "")
# endif()

# set(MOER_AGILITYSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/microsoft.direct3d.d3d12.${MOER_AGILITYSDK_VERSION_ID}" CACHE INTERNAL "")
# set(MOER_AGILITYSDK_DLL_DIR "${MOER_AGILITYSDK_DIR}/build/native/bin/x64" CACHE INTERNAL "")



# # modified 'copy_dll' function from 'cmake/functions.cmake'
# function(deploy_agilitysdk target_name )
#     if(NOT TARGET copy_dll_${target_name})
#         add_custom_target(copy_dll_${target_name})
#         set_target_properties(copy_dll_${target_name} PROPERTIES FOLDER "utils")
#     endif()
#     add_library(${target_name} INTERFACE)
#     target_include_directories(${target_name} INTERFACE "${MOER_AGILITYSDK_DIR}/build/native/include")
 
#     set(dll
#         "${MOER_AGILITYSDK_DLL_DIR}/D3D12Core.dll"
#         "${MOER_AGILITYSDK_DLL_DIR}/D3D12Core.pdb"
#         "${MOER_AGILITYSDK_DLL_DIR}/d3d12SDKLayers.dll"
#         "${MOER_AGILITYSDK_DLL_DIR}/d3d12SDKLayers.pdb"
#     )
#     message(STATUS "copy ${dll} to ${real_out_put_dir}/D3D12")
#     add_custom_command(
#         TARGET copy_dll_${target_name}
#         POST_BUILD
#         COMMAND
#             ${CMAKE_COMMAND} -E make_directory "${real_out_put_dir}/D3D12"
#         COMMAND 
#             ${CMAKE_COMMAND} -E copy_if_different
#             "${dll}"
#             "${real_out_put_dir}/D3D12"
#         COMMAND_EXPAND_LISTS
#     )

#     set_target_properties(${target_name} 
#         PROPERTIES IMPORTED_LOCATION  "${dll}")

#     add_dependencies(${target_name} copy_dll_${target_name})

# endfunction()

# deploy_agilitysdk(d3d12_agilitysdk)


# add_library(d3d12_agilitysdk_d3dx12  
#     STATIC 
#     "${MOER_AGILITYSDK_DIR}/build/native/src/d3dx12/d3dx12_property_format_table.cpp"
# )
# target_include_directories(d3d12_agilitysdk_d3dx12 
#     PUBLIC
#     "${MOER_AGILITYSDK_DIR}/build/native/include/d3dx12")
# target_link_libraries(d3d12_agilitysdk_d3dx12 PRIVATE d3d12_agilitysdk)
