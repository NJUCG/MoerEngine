
add_subdirectory(cuda)
add_subdirectory(runtime)
add_subdirectory(editor)

if(${moer_build_test})
	add_subdirectory(test)
endif()
