set(target_name moer_scripting)

set(moer_runtime_scripting_dir ${moer_runtime_dir}/scripting)

file(GLOB_RECURSE scripting_headers CONFIGURE_DEPENDS "*.h")
file(GLOB_RECURSE scripting_sources CONFIGURE_DEPENDS "*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${scripting_headers} ${scripting_sources})

add_library(${target_name} SHARED ${scripting_headers} ${scripting_sources})
add_library(Moer::scripting ALIAS ${target_name})

target_compile_definitions(${target_name} PRIVATE SCRIPTING_MODULE_SHARED_LIB)

if (MSVC)
    target_compile_options(${target_name} PRIVATE /bigobj)
endif()

set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_dir}>
)

target_link_libraries(${target_name}
    PUBLIC Moer::Core
    PRIVATE Moer::Render
    PRIVATE EnTT::EnTT
    PUBLIC pybind11::headers
    PUBLIC python312_embed
)

if (NOT WIN32)
    get_target_property(python312_home python312_embed PYTHON312_TARGET_ROOT)
    get_target_property(python312_stdlib python312_embed PYTHON312_TARGET_STDLIB_DIR)
    get_target_property(python312_dynload python312_embed PYTHON312_TARGET_DYNLOAD_DIR)
    target_compile_definitions(${target_name} PRIVATE
        "MOER_PYTHON_HOME=\"${python312_home}\""
        "MOER_PYTHON_STDLIB_DIR=\"${python312_stdlib}\""
        "MOER_PYTHON_DYNLOAD_DIR=\"${python312_dynload}\""
    )
endif()
