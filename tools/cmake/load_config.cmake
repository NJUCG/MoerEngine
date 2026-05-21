
# ============================================================
# Feature Defaults (all OFF)
# ============================================================

# CUDA
set(WITH_CUDA OFF CACHE BOOL "WITH_CUDA" FORCE)
set(LIBTORCH_DIR "/path/to/libtorch" CACHE PATH "LIBTORCH_DIR" FORCE)
set(TENSORRT_DIR "/path/to/tensorrt" CACHE PATH "TENSORRT_DIR" FORCE)

# NRD
set(WITH_NRD OFF CACHE BOOL "WITH_NRD" FORCE)
set(NRD_ROOT "/path/to/nrd"  CACHE PATH "NRD_ROOT" FORCE)

# RenderDoc
set(WITH_RENDERDOC OFF CACHE BOOL "WITH_RENDERDOC" FORCE)
set(RENDERDOC_ROOT "/path/to/renderdoc"  CACHE PATH "RENDERDOC_ROOT" FORCE)

# Profile
set(WITH_PROFILE OFF CACHE BOOL "WITH_PROFILE" FORCE)

# ============================================================
# Load user overrides from EnableFeatures.cmake
# template: template.EnableFeatures.cmake
# ============================================================

if (EXISTS "${CMAKE_SOURCE_DIR}/EnableFeatures.cmake")
    include("${CMAKE_SOURCE_DIR}/EnableFeatures.cmake")
    message(STATUS "EnableFeatures.cmake found")
else()
    message(STATUS "EnableFeatures.cmake not found, all optional features disabled")
endif()

# ============================================================
# Print feature configuration
# ============================================================

message(STATUS "WITH_CUDA      = ${WITH_CUDA}")
message(STATUS "WITH_NRD       = ${WITH_NRD}")
message(STATUS "WITH_RENDERDOC = ${WITH_RENDERDOC}")
message(STATUS "WITH_PROFILE   = ${WITH_PROFILE}")