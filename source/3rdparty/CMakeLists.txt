#Vulkan SDK
set(third_party_folder "3rdParty")

if(WIN32)
    set(vulkan_lib "${THIRD_PARTY_DIR}/VulkanSDK/lib/Windows/vulkan-1.lib" PARENT_SCOPE)
    set(glslangValidator "${THIRD_PARTY_DIR}/VulkanSDK/binary/Windows/glslangValidator.exe" PARENT_SCOPE)
    # 程序内部生成系统路径，使用自己的VK_LAYER_PATH
    add_compile_definitions("${CMAKE_PROJECT_NAME}_VK_LAYER_PATH=${THIRD_PARTY_DIR}/VulkanSDK/binary/Windows")

elseif(APPLE)
    set(vulkan_lib "${THIRD_PARTY_DIR}/VulkanSDK/lib/MacOS/libvulkan.1.dylib" PARENT_SCOPE)
    set(glslangValidator "${THIRD_PARTY_DIR}/VulkanSDK/binary/MacOS/glslangValidator" PARENT_SCOPE)
    add_compile_definitions("${CMAKE_PROJECT_NAME}_VK_LAYER_PATH=${THIRD_PARTY_DIR}/VulkanSDK/binary/MacOS")
    #https://chromium.googlesource.com/external/github.com/KhronosGroup/Vulkan-Loader/+/HEAD/loader/LoaderAndLayerInterface.md#icd-discovery
    add_compile_definitions("${CMAKE_PROJECT_NAME}_VK_ICD_FILENAMES=${THIRD_PARTY_DIR}/VulkanSDK/binary/MacOS/MoltenVK_icd.json")

else()
endif()

IF(NOT TARGET spdlog)
    ADD_SUBDIRECTORY(spdlog)
ENDIF()

    option(BUILD_STATIC_LIBS ON)
    option(BUILD_TESTING OFF)
    option(GLM_TEST_ENABLE OFF)
    option(BUILD_SHARED_LIBS OFF)
    ADD_SUBDIRECTORY(glm)

