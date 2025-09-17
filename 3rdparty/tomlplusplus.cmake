
# export "tomlplusplus" target, which is an header-only library (precomputed textures)

set(TOMLPLUSPLUS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tomlplusplus-3.4.0/include)
add_library(tomlplusplus INTERFACE)
target_include_directories(tomlplusplus INTERFACE "${TOMLPLUSPLUS_DIR}")