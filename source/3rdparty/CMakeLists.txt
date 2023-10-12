#Vulkan SDK
set(third_party_folder "3rdParty")

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

add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER ${third_party_folder})
    
option(GLFW_BUILD_DOCS OFF)
option(GLFW_BUILD_EXAMPLES OFF)
option(GLFW_BUILD_TESTS OFF)
option(GLFW_INSTALL ON)
add_subdirectory(glfw)

