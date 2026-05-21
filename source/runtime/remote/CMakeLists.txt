set(target_name moer_remote)

set(moer_runtime_remote_dir ${moer_runtime_dir}/remote)

file(GLOB_RECURSE remote_headers CONFIGURE_DEPENDS "*.h")
file(GLOB_RECURSE remote_sources CONFIGURE_DEPENDS "*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${remote_headers} ${remote_sources})

add_library(${target_name} SHARED ${remote_headers} ${remote_sources})
add_library(Moer::remote ALIAS ${target_name})

target_compile_definitions(${target_name} PRIVATE REMOTE_MODULE_SHARED_LIB)

set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_dir}>
)

target_link_libraries(${target_name}
    PUBLIC Moer::Core
    PUBLIC Moer::scripting
    PRIVATE hv
)