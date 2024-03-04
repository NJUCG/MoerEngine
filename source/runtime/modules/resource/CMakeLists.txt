set(MODULE_NAME MoerResource)
set(MODULE_DIR ${moer_runtime_module_root_dir}/resource)

file(GLOB_RECURSE RESOURCE_MODULE_HEADERS "include/*.h")
file(GLOB_RECURSE RESOURCE_MODULE_SOURCES "source/*.cpp" "source/*.h")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RESOURCE_MODULE_HEADERS} ${RESOURCE_MODULE_SOURCES})

add_library(${MODULE_NAME} SHARED ${RESOURCE_MODULE_HEADERS} ${RESOURCE_MODULE_SOURCES})
target_compile_definitions(${MODULE_NAME} PRIVATE RESOURCE_MODULE_SHARED_LIB)

target_compile_options(${MODULE_NAME} PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>")

set_target_properties(${MODULE_NAME} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

set_target_properties(${MODULE_NAME} PROPERTIES FOLDER "runtime")

target_include_directories(${MODULE_NAME}
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
    PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>
)
target_link_libraries(${MODULE_NAME}
    PUBLIC AssetLoad
    PUBLIC MoerRuntimeCore
    PUBLIC render
    PUBLIC MeshProcess
)