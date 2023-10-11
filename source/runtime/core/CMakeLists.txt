set(target_name MoerRuntimeCore)
file(GLOB_RECURSE CORE_HEADERS "include/*.h" "include/*.inl")
file(GLOB_RECURSE CORE_SOURCES "source/*.cpp")
file(GLOB_RECURSE CORE_PRIVATE_HEADERS "source/*.h")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_HEADERS} ${CORE_SOURCES} ${CORE_PRIVATE_HEADERS})

add_library(${target_name} ${CORE_HEADERS} ${CORE_SOURCES} ${CORE_PRIVATE_HEADERS})
set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
	PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/boost>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glm>
)
target_link_libraries(${target_name}
    PUBLIC glm::glm
    PUBLIC spdlog::spdlog_header_only
)