if (MOER_CORE_ONLY)
    set(moer_runtime_dir ${moer_source_dir}/runtime)
    set(moer_runtime_core_dir ${moer_runtime_dir}/core)

    add_subdirectory(runtime/core)

    if(${moer_build_test})
    	add_subdirectory(test)
    endif()

    return()
endif()

add_subdirectory(profile_deprecated)
add_subdirectory(cuda)
add_subdirectory(runtime)
add_subdirectory(editor)
add_subdirectory(profiler)

if(${moer_build_test})
	add_subdirectory(test)
endif()
