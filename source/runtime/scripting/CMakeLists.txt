set(target_name moer_scripting)

set(moer_runtime_scripting_dir ${moer_runtime_dir}/scripting)

file(GLOB_RECURSE scripting_headers CONFIGURE_DEPENDS "*.h")
file(GLOB_RECURSE scripting_sources CONFIGURE_DEPENDS "*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${scripting_headers} ${scripting_sources})

add_library(${target_name} SHARED ${scripting_headers} ${scripting_sources})
add_library(Moer::scripting ALIAS ${target_name})

target_compile_definitions(${target_name} PRIVATE SCRIPTING_MODULE_SHARED_LIB)

set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_dir}>
)

target_link_libraries(${target_name}
    PUBLIC Moer::Core
    PUBLIC pybind11::headers
    PUBLIC python312_embed
)
