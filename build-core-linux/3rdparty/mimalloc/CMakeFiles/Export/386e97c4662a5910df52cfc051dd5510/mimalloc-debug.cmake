#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "mimalloc" for configuration "Debug"
set_property(TARGET mimalloc APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(mimalloc PROPERTIES
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/libmimalloc-debug.so.2.1"
  IMPORTED_SONAME_DEBUG "libmimalloc-debug.so.2"
  )

list(APPEND _cmake_import_check_targets mimalloc )
list(APPEND _cmake_import_check_files_for_mimalloc "${_IMPORT_PREFIX}/lib/libmimalloc-debug.so.2.1" )

# Import target "mimalloc-static" for configuration "Debug"
set_property(TARGET mimalloc-static APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(mimalloc-static PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/lib/mimalloc-2.1/libmimalloc-debug.a"
  )

list(APPEND _cmake_import_check_targets mimalloc-static )
list(APPEND _cmake_import_check_files_for_mimalloc-static "${_IMPORT_PREFIX}/lib/mimalloc-2.1/libmimalloc-debug.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
