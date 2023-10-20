set(TINYEXR_BUILD_SAMPLE OFF CACHE BOOL " " FORCE)

set(TINYEXR_USE_MINIZ ON CACHE BOOL " " FORCE)
add_subdirectory(tinyexr)

# add_library(tinyexr SHARED tinyexr/tinyexr.h tinyexr/tinyexr.cc)
target_include_directories(tinyexr INTERFACE tinyexr/)
set_target_properties(tinyexr PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)