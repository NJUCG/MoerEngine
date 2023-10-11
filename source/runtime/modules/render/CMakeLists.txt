set(MODULE_NAME render)
set(MODULE_DIR ${moer_runtime_module_root_dir}/${MODULE_NAME})

configure_file(${MODULE_DIR}/include/config.h.in ${MODULE_DIR}/include/config.h)

file(GLOB_RECURSE RENDER_MODULE_HEADERS "include/*.h")
file(GLOB_RECURSE RENDER_MODULE_SOURCES "source/*.cpp" "source/*.h")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

add_library(${MODULE_NAME} ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

set_target_properties(${MODULE_NAME} PROPERTIES FOLDER "runtime")

target_include_directories(${MODULE_NAME}
        PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>
    	PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include/rhi>
        PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/boost>
        PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
        PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glm>
        PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
        PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include/rhi/vulkan>
	PRIVATE $<BUILD_INTERFACE:${moer_third_party_dir}/VulkanSDK/include/vulkan>
        PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/VulkanMemoryAllocator>
)

target_link_libraries(${MODULE_NAME}
        PRIVATE ${vulkan_lib}
        PUBLIC MoerRuntimeCore
)

set(RenderModuleTest ON)
set(VulkanRHITest ON)

if (${RenderModuleTest})

    add_executable(ShaderReflectionTest test/ShaderReflectionTest.cpp)
    target_include_directories(ShaderReflectionTest
            PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glm>
            PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/boost>

            PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
            PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>

    )
    target_link_libraries(ShaderReflectionTest
            PRIVATE ${MODULE_NAME}
    )

    add_test(NAME ShaderReflectionTest_ COMMAND ShaderReflectionTest)
endif ()

if (${VulkanRHITest})

    add_executable(VulkanRHITest test/VulkanRHITest.cpp)
    target_include_directories(VulkanRHITest
            PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
            PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glm>
            PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/boost>
            PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/include>
            PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>
    )
    target_link_libraries(VulkanRHITest
            PRIVATE ${MODULE_NAME}
            PRIVATE glfw
    )

    add_test(NAME VulkanRHITest_ COMMAND VulkanRHITest)

endif ()