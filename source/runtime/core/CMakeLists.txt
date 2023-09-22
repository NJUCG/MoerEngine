set(target_name MoerRuntimeCore)
file(GLOB_RECURSE CORE_HEADERS "include/*\(.h|.inl\)")
file(GLOB_RECURSE CORE_SOURCES "source/*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_HEADERS} ${CORE_SOURCES})

add_library(${target_name} ${CORE_HEADERS} ${CORE_SOURCES})
set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
	PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/boost>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glm>
)
target_link_libraries(${target_name}
    PUBLIC glm::glm
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
    PUBLIC spdlog::spdlog_header_only
)