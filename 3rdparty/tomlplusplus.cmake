
# export "smaa" target, which is an header-only library (precomputed textures)

set(TOMLPLUSPLUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tomlplusplus/include)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE "${TOMLPLUSPLUS_DIR}")