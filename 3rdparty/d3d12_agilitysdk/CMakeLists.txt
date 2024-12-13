project(d3d12_agilitysdk)
 
set(MOER_AGILITYSDK_VERSION_MAJOR "715" CACHE INTERNAL "")
set(MOER_AGILITYSDK_VERSION_MINOR "1"   CACHE INTERNAL "")
set(MOER_AGILITYSDK_VERSION_IS_PREVIEW TRUE CACHE INTERNAL TRUE)
set(MOER_AGILITYSDK_VERSION_ID "1.${MOER_AGILITYSDK_VERSION_MAJOR}.${MOER_AGILITYSDK_VERSION_MINOR}" CACHE INTERNAL "")
if(MOER_AGILITYSDK_VERSION_IS_PREVIEW)
    set(MOER_AGILITYSDK_VERSION_ID "${MOER_AGILITYSDK_VERSION_ID}-preview" CACHE INTERNAL "")
endif()

set(MOER_AGILITYSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/microsoft.direct3d.d3d12.${MOER_AGILITYSDK_VERSION_ID}" CACHE INTERNAL "")
set(MOER_AGILITYSDK_DLL_DIR "${MOER_AGILITYSDK_DIR}/build/native/bin/x64" CACHE INTERNAL "")



# modified 'copy_dll' function from 'cmake/functions.cmake'
function(deploy_agilitysdk target_name )
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
    message(STATUS "copy ${dll} to ${real_out_put_dir}/D3D12")
    add_custom_command(
        TARGET copy_dll_${target_name}
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E make_directory "${real_out_put_dir}/D3D12"
        COMMAND 
            ${CMAKE_COMMAND} -E copy_if_different
            "${dll}"
            "${real_out_put_dir}/D3D12"
        COMMAND_EXPAND_LISTS
    )

    set_target_properties(${target_name} 
        PROPERTIES IMPORTED_LOCATION  "${dll}")

    add_dependencies(${target_name} copy_dll_${target_name})

endfunction()

deploy_agilitysdk(d3d12_agilitysdk)


add_library(d3d12_agilitysdk_d3dx12  
    STATIC 
    "${MOER_AGILITYSDK_DIR}/build/native/src/d3dx12/d3dx12_property_format_table.cpp"
)
target_include_directories(d3d12_agilitysdk_d3dx12 
    PUBLIC
    "${MOER_AGILITYSDK_DIR}/build/native/include/d3dx12")
target_link_libraries(d3d12_agilitysdk_d3dx12 PRIVATE d3d12_agilitysdk)
