
set(third_party_folder "3rdParty")

#Vulkan SDK
if(WIN32)
    set(vulkan_lib "${moer_third_party_dir}/VulkanSDK/lib/Windows/vulkan-1.lib" PARENT_SCOPE)
    set(glslangValidator "${moer_third_party_dir}/VulkanSDK/binary/Windows/glslangValidator.exe" PARENT_SCOPE)
    # 程序内部生成系统路径，使用自己的VK_LAYER_PATH
    add_compile_definitions("${CMAKE_PROJECT_NAME}_VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/Windows")

elseif(APPLE)
    set(vulkan_lib "${moer_third_party_dir}/VulkanSDK/lib/MacOS/libvulkan.1.dylib" PARENT_SCOPE)
    set(glslangValidator "${moer_third_party_dir}/VulkanSDK/binary/MacOS/glslangValidator" PARENT_SCOPE)
    add_compile_definitions("${CMAKE_PROJECT_NAME}_VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/MacOS")
    #https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader/+/HEAD/loader/LoaderAndLayerInterface.md#icd-discovery
    add_compile_definitions("${CMAKE_PROJECT_NAME}_VK_ICD_FILENAMES=${moer_third_party_dir}/VulkanSDK/binary/MacOS/MoltenVK_icd.json")

else()
endif()

add_library(dxc SHARED IMPORTED GLOBAL)
add_custom_target(DXC_COPY ALL)
#DXC
if(WIN32)
    set(dxc_lib "${moer_third_party_dir}/DirectXShaderCompiler/lib/Windows/dxcompiler.lib" PARENT_SCOPE)
    set(dxc_lib_local "${moer_third_party_dir}/DirectXShaderCompiler/lib/Windows/dxcompiler.lib")
    set(dxc_runtime_dir "${moer_third_party_dir}/DirectXShaderCompiler/bin/Windows")
    set(dxc_bin_dir "${moer_third_party_dir}/DirectXShaderCompiler/bin/Windows" PARENT_SCOPE)
    set(dxc_bin "${moer_third_party_dir}/DirectXShaderCompiler/bin/Windows/dxc.exe" PARENT_SCOPE)

    if(CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
    set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
else() 
    set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") 
endif()
    add_custom_command(
        TARGET DXC_COPY
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${dxc_runtime_dir}/dxcompiler.dll"
        ${real_out_put_dir}
        # COMMAND ${CMAKE_COMMAND} -E copy_if_different
        # "${dxc_runtime_dir}/dxcompiler.dll"
        # ${real_out_put_dir}  #executable

        # COMMAND ${CMAKE_COMMAND} -E copy_if_different
        # "${dxc_runtime_dir}/dxcompiler.dll"
        # ${real_out_put_dir}  #test
    )

    set_target_properties(dxc PROPERTIES 
     IMPORTED_IMPLIB  ${dxc_lib_local}
     IMPORTED_LOCATION  ${dxc_runtime_dir}/dxcompiler.dll)

elseif(LINUX)
endif()

add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER ${third_party_folder})


