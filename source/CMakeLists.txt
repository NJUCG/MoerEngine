
add_subdirectory(cuda)
add_subdirectory(runtime)
add_subdirectory(editor)

if(WITH_PROFILE)
	add_subdirectory(profile)
endif()

if(${moer_build_test})
	add_subdirectory(test)
endif()
