
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

#DXC


# add_library(ShaderConductorSDK SHARED IMPORTED)
# set_target_properties(ShaderConductorSDK PROPERTIES
#     IMPORTED_CONFIGURATIONS        "DEBUG;RELEASE"
#     IMPORTED_LOCATION_DEBUG        ${ShaderConductorDir_LOCAL}/${ShaderConductor_LIB_NAME}
#     IMPORTED_IMPLIB_DEBUG          ${ShaderConductorDir_LOCAL}/${ShaderConductor_LIB_NAME}
#     IMPORTED_LOCATION_RELEASE      ${ShaderConductorDir_LOCAL}/${ShaderConductor_LIB_NAME}
#     IMPORTED_IMPLIB_RELEASE        ${ShaderConductorDir_LOCAL}/${ShaderConductor_LIB_NAME}
#     INTERFACE_INCLUDE_DIRECTORIES  ${THIRD_PARTY_DIR}/ShaderConductor/Include
# )
IF(NOT TARGET dxc)
    add_library(dxc SHARED IMPORTED)
    set_target_properties(dxc PROPERTIES
         IMPORTED_CONFIGURATIONS        "RELEASE"
        PROPERTY IMPORTED_LOCATION_RELEASE      ${dxc_lib_dir_local}/${ShaderConductor_LIB_NAME}
        PROPERTY IMPORTED_IMPLIB_RELEASE        ${dxc_lib_local}
        PROPERTY INTERFACE_INCLUDE_DIRECTORIES  ${THIRD_PARTY_DIR}/DirectXShaderCompiler/include
    )
    add_custom_target(dxc_build)
    if(WIN32)
        set(dxc_lib "${THIRD_PARTY_DIR}/DirectXShaderCompiler/lib/Windows/dxcompiler.lib" PARENT_SCOPE)
        set(dxc_lib_local "${THIRD_PARTY_DIR}/DirectXShaderCompiler/lib/Windows/dxcompiler.lib")
        set(dxc_runtime "${THIRD_PARTY_DIR}/DirectXShaderCompiler/bin/Windows/dxcompiler.dll")
        set(dxc_bin_dir "${THIRD_PARTY_DIR}/DirectXShaderCompiler/bin/Windows" PARENT_SCOPE)
        set(dxc_bin "${THIRD_PARTY_DIR}/DirectXShaderCompiler/bin/Windows/dxc.exe" PARENT_SCOPE)
    
        if (${CMAKE_GENERATOR} MATCHES "Ninja")
            add_custom_command(
                    OUTPUT ${PROJECT_BINARY_DIR}/dxcompiler.dll
                    DEPENDS dxc_build
                    POST_BUILD
                    COMMAND
                    ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${dxc_runtime}> ${PROJECT_BINARY_DIR}
            )
        else ()
            add_custom_command(
                    TARGET
                    dxc
                    POST_BUILD
                    COMMAND
                    ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${dxc_runtime}> ${PROJECT_BINARY_DIR}/$<CONFIG>
            )
        endif ()
    elseif(LINUX)
    endif()

ENDIF()

add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER ${third_party_folder})

# option(BUILD_STATIC_LIBS ON)
# option(BUILD_TESTING OFF)
# option(GLM_TEST_ENABLE OFF)
# option(BUILD_SHARED_LIBS OFF)
# ADD_SUBDIRECTORY(glm)

