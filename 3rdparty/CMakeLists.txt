
set(third_party_folder "3rdParty")

#Vulkan SDK
if(WIN32)
    set(vulkan_lib "${moer_third_party_dir}/VulkanSDK/lib/Windows/vulkan-1.lib" PARENT_SCOPE)
    set(glslangValidator "${moer_third_party_dir}/VulkanSDK/binary/Windows/glslangValidator.exe")

elseif(APPLE)
    set(vulkan_lib "${moer_third_party_dir}/VulkanSDK/lib/MacOS/libvulkan.1.dylib" PARENT_SCOPE)
    set(glslangValidator "${moer_third_party_dir}/VulkanSDK/binary/MacOS/glslangValidator" )
    add_compile_definitions("VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/MacOS")
    #https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader/+/HEAD/loader/LoaderAndLayerInterface.md#icd-discovery
    add_compile_definitions("VK_ICD_FILENAMES=${moer_third_party_dir}/VulkanSDK/binary/MacOS/MoltenVK_icd.json")
    

else()
endif()

add_library(dxc SHARED IMPORTED GLOBAL)

add_custom_target(DXC_COPY)
#DXC
if(WIN32)
    set(dxc_lib "${moer_third_party_dir}/DirectXShaderCompiler/lib/Windows/dxcompiler.lib" )
    set(dxc_runtime_dir "${moer_third_party_dir}/DirectXShaderCompiler/bin/Windows")

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
    )

    set_target_properties(dxc PROPERTIES 
     IMPORTED_IMPLIB  ${dxc_lib}
     IMPORTED_LOCATION  ${dxc_runtime_dir}/dxcompiler.dll)

elseif(LINUX)
endif()

add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER ${third_party_folder})

# option(BUILD_STATIC_LIBS ON)
# option(BUILD_TESTING OFF)
# option(GLM_TEST_ENABLE OFF)
# option(BUILD_SHARED_LIBS OFF)
# ADD_SUBDIRECTORY(glm)
    
option(GLFW_BUILD_DOCS OFF)
option(GLFW_BUILD_EXAMPLES OFF)
option(GLFW_BUILD_TESTS OFF)
option(GLFW_INSTALL ON)
add_subdirectory(glfw)

add_subdirectory(SPIRV-Cross)
set_target_properties(spirv-cross-core PROPERTIES FOLDER ${third_party_folder})

