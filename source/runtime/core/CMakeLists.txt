set(TARGET_NAME MoerRuntimeCore)
file(GLOB_RECURSE CORE_HEADERS "include/*.h")
file(GLOB_RECURSE CORE_SOURCES "source/*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_HEADERS} ${CORE_SOURCES})

add_library(${TARGET_NAME} ${CORE_HEADERS} ${CORE_SOURCES})
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "runtime")

target_include_directories(${TARGET_NAME}
	PUBLIC $<BUILD_INTERFACE:${RUNTIME_CORE_DIR}/include>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/boost>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/glm>
)
target_link_libraries(${TARGET_NAME}
    PUBLIC glm::glm
    PUBLIC spdlog::spdlog_header_only
)