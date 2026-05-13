# Export Moer::Runtime target

set(target_name moer_runtime)

set(moer_runtime_dir ${moer_source_dir}/runtime)

set(moer_runtime_core_dir ${moer_runtime_dir}/core)
set(moer_runtime_render_dir ${moer_runtime_dir}/render)
set(moer_runtime_scripting_dir ${moer_runtime_dir}/scripting)

add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(scripting)

# CONFIGURE_DEPENDS，指如果文件变化了，则自动重新GLOB
# 这里不能递归GLOB，因为子文件夹内是其他模块
file(GLOB engine_headers CONFIGURE_DEPENDS "*.h")
file(GLOB engine_sources CONFIGURE_DEPENDS "*.cpp")
add_library(${target_name} STATIC ${engine_headers} ${engine_sources})
add_library(Moer::Runtime ALIAS ${target_name})

set_target_properties(${target_name} PROPERTIES 
    FOLDER "runtime"  )

target_include_directories(${target_name} 
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_dir}>
)

target_link_libraries(${target_name}
    PUBLIC Moer::Core
    PUBLIC Moer::Render
    PUBLIC Moer::scripting
)