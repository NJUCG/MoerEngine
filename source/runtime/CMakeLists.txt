set(target_name MoerRuntime)
set(moer_runtime_dir ${moer_source_dir}/runtime)


set(moer_runtime_core_dir ${moer_runtime_dir}/core)
set(moer_runtime_module_root_dir ${moer_runtime_dir}/modules)
add_subdirectory(core)

add_subdirectory(modules/render)

add_library(${target_name} INTERFACE)

set_target_properties(${target_name} PROPERTIES FOLDER "engine")