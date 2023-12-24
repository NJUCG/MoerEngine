set(TINYEXR_BUILD_SAMPLE OFF CACHE BOOL " " FORCE)

set(TINYEXR_USE_MINIZ ON CACHE BOOL " " FORCE)
add_subdirectory(tinyexr)

# add_library(tinyexr SHARED tinyexr/tinyexr.h tinyexr/tinyexr.cc)
target_include_directories(tinyexr INTERFACE tinyexr/)
set_target_properties(tinyexr PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(WIN32)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" ) # clang-cl
    target_compile_options(tinyexr PRIVATE 
    -Wno-unsafe-buffer-usage 
    -Wno-tautological-type-limit-compare 
    -Wno-old-style-cast
    -Wno-sign-conversion
    -Wno-nonportable-system-include-path
    -Wno-unused-but-set-variable)
else() # msvc
endif()
endif()