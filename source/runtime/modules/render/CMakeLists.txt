set(MODULE_NAME render)
set(MODULE_DIR ${moer_runtime_module_root_dir}/${MODULE_NAME})
file(GLOB_RECURSE RENDER_MODULE_HEADERS "include/*.h")
file(GLOB_RECURSE RENDER_MODULE_SOURCES "source/*.cpp")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

add_library(${MODULE_NAME} ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

set_target_properties(${MODULE_NAME} PROPERTIES FOLDER "runtime")
target_link_directories(${MODULE_NAME}
	PUBLIC ${dxc_bin_dir}
target_include_directories(${MODULE_NAME}



	PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include/rhi>
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/boost>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glm>
	PRIVATE $<BUILD_INTERFACE:${MODULE_DIR}/include/rhi/vulkan>
	PRIVATE $<BUILD_INTERFACE:${moer_third_party_dir}/VulkanSDK/include/vulkan>
	PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/DirectXShaderCompiler/include>
)

target_link_libraries(${MODULE_NAME}
    PRIVATE ${vulkan_lib}
	PRIVATE ${dxc_lib}
    PUBLIC MoerRuntimeCore
)
