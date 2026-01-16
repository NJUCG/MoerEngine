set(target_name moer_resource)

file(GLOB_RECURSE RESOURCE_MODULE_HEADERS "*.h")
file(GLOB_RECURSE RESOURCE_MODULE_SOURCES "*.cpp" "*.h")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RESOURCE_MODULE_HEADERS} ${RESOURCE_MODULE_SOURCES})

add_library(${target_name} SHARED ${RESOURCE_MODULE_HEADERS} ${RESOURCE_MODULE_SOURCES})
add_library(Moer::Resource ALIAS ${target_name})

target_compile_definitions(${target_name} PRIVATE RESOURCE_MODULE_SHARED_LIB)

target_compile_options(${target_name} PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>")

set_target_properties(${target_name} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}>
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_resource_dir}>
)
target_link_libraries(${target_name}
    PUBLIC AssetLoad
    PUBLIC Moer::Core
    PUBLIC MeshProcess
    PUBLIC nlohmann_json::nlohmann_json
)