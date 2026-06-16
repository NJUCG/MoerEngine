set(GKLIB_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(SHARED OFF CACHE BOOL "" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/gklib)

if (WIN32)
	target_include_directories(GKlib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/gklib/include/win32)
	target_compile_definitions(GKlib PUBLIC WIN32 __MSC__ _CRT_SECURE_NO_DEPRECATE read=_read write=_write)
	target_compile_options(GKlib PRIVATE -include io.h)
	set_property(TARGET GKlib PROPERTY LINK_LIBRARIES "")
	set_property(TARGET GKlib PROPERTY INTERFACE_LINK_LIBRARIES "")
endif()

unset(GKLIB_BUILD_APPS CACHE)
unset(SHARED CACHE)