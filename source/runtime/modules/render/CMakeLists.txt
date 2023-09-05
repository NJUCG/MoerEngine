set(MODULE_NAME render)
set(MODULE_DIR ${RUNTIME_MODULE_ROOT_DIR}/${MODULE_NAME})
file(GLOB_RECURSE RENDER_MODULE_HEADERS "include/*.h")
file(GLOB_RECURSE RENDER_MODULE_SOURCES "*source/*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

add_library(${MODULE_NAME} ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

set_target_properties(${MODULE_NAME} PROPERTIES FOLDER "engine/runtime/render")

target_include_directories(${MODULE_NAME}
	PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>
    PUBLIC $<BUILD_INTERFACE:${RUNTIME_CORE_DIR}/include>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/boost>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/spdlog/include>
)

target_link_libraries(${MODULE_NAME}
    #PUBLIC glm::glm
    #PUBLIC glfw
    #PUBLIC assimp
    #PUBLIC imgui
    #glslang
    #PUBLIC pugixml-shared
    #nlohmann_json
    ## HLSL
    #OSDependent
    #OGLCompiler
    #SPIRV
    #spirv-cross-core
    #spirv-cross-cpp
    #spirv-cross-glsl
    #spirv-cross-reflect
    PRIVATE ${vulkan_lib}
)