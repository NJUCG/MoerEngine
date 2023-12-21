set(target_name MoerEngineRuntime)
set(moer_runtime_dir ${moer_source_dir}/runtime)

set(moer_runtime_core_dir ${moer_runtime_dir}/core)
set(moer_runtime_module_root_dir ${moer_runtime_dir}/modules)

add_subdirectory(core)
add_subdirectory(modules/render)
add_subdirectory(modules/resource)


# add_subdirectory(functions)
file(GLOB  ENGINE_HEADERS   "*.h")
file(GLOB  ENGINE_SOURCES   "*.cpp")
add_library(${target_name} STATIC ${ENGINE_HEADERS} ${ENGINE_SOURCES})

set_target_properties(${target_name} PROPERTIES 
    FOLDER "engine"  )

target_include_directories(${target_name} 
    PRIVATE $<BUILD_INTERFACE:${RUNTIME_CORE_DIR}/include>
    PRIVATE $<BUILD_INTERFACE:${RUNTIME_MODULE_ROOT_DIR}/render/include>
)

target_link_libraries(${target_name}
    PUBLIC MoerRuntimeCore
    PUBLIC render
)
