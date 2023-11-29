# #Vulkan SDK
if(WIN32)
    set(vulkan_lib "${moer_third_party_dir}/VulkanSDK/lib/Windows/vulkan-1.lib" PARENT_SCOPE)
elseif(LINUX)
    set(vulkan_lib "${moer_third_party_dir}/VulkanSDK/lib/Linux/libvulkan.so.1" PARENT_SCOPE)
    add_compile_definitions("VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/Linux")
    #https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader/+/HEAD/loader/LoaderAndLayerInterface.md#icd-discovery
    add_compile_definitions("VK_ICD_FILENAMES=${moer_third_party_dir}/VulkanSDK/binary/Linux/vulkan_icd.json")
elseif(APPLE)
    set(vulkan_lib "${moer_third_party_dir}/VulkanSDK/lib/MacOS/libvulkan.1.dylib" PARENT_SCOPE)
    add_compile_definitions("VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/MacOS")
    #https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader/+/HEAD/loader/LoaderAndLayerInterface.md#icd-discovery
    add_compile_definitions("VK_ICD_FILENAMES=${moer_third_party_dir}/VulkanSDK/binary/MacOS/MoltenVK_icd.json")
    
else()
endif()