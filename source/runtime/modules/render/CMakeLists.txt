set(MODULE_NAME render)
set(MODULE_DIR ${RUNTIME_MODULE_ROOT_DIR}/${MODULE_NAME})
file(GLOB_RECURSE RENDER_MODULE_HEADERS "include/*.h")
file(GLOB_RECURSE RENDER_MODULE_SOURCES "source/*.cpp")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

add_library(${MODULE_NAME} ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

set_target_properties(${MODULE_NAME} PROPERTIES FOLDER "runtime")

target_include_directories(${MODULE_NAME}
	PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>
    PUBLIC $<BUILD_INTERFACE:${RUNTIME_CORE_DIR}/include>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/boost>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/glm>
	PRIVATE $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/VulkanSDK/include/vulkan>
)
target_link_libraries(${MODULE_NAME}
    PRIVATE ${vulkan_lib}
    PUBLIC MoerRuntimeCore
)

set(RenderModuleTest ON)

if(${RenderModuleTest})

add_executable(ShaderReflectionTest test/ShaderReflectionTest.cpp)
target_include_directories(ShaderReflectionTest
	PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/glm>
	PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/boost>

	PUBLIC $<BUILD_INTERFACE:${RUNTIME_CORE_DIR}/include>
	PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>

)
target_link_libraries(ShaderReflectionTest
	PRIVATE ${MODULE_NAME}
)

#add_test(NAME ShaderReflectionTest_ COMMAND ShaderReflectionTest)
endif()