# #Vulkan SDK
if(WIN32)
    add_compile_definitions("VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/Windows")
    add_compile_definitions("VK_ICD_FILENAMES=${moer_third_party_dir}/VulkanSDK/binary/Windows/vulkan_icd.json")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions("VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/Linux")
    #https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader/+/HEAD/loader/LoaderAndLayerInterface.md#icd-discovery
    add_compile_definitions("VK_ICD_FILENAMES=${moer_third_party_dir}/VulkanSDK/binary/Linux/vulkan_icd.json")
elseif(APPLE)
    add_compile_definitions("VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/MacOS")
    #https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader/+/HEAD/loader/LoaderAndLayerInterface.md#icd-discovery
    add_compile_definitions("VK_ICD_FILENAMES=${moer_third_party_dir}/VulkanSDK/binary/MacOS/MoltenVK_icd.json")
    
else()
endif()