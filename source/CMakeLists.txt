add_subdirectory(profile)
add_subdirectory(cuda)
add_subdirectory(runtime)
add_subdirectory(editor)
add_subdirectory(tool)

if(${moer_build_test})
	add_subdirectory(test)
endif()
