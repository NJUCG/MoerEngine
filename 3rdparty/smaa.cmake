
# export "smaa" target, which is an header-only library (precomputed textures)

set(SMAA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/smaa)
add_library(smaa INTERFACE)
target_include_directories(smaa INTERFACE "${SMAA_DIR}")