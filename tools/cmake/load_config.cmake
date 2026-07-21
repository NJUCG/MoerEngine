
# ============================================================
# Feature Defaults (all OFF)
# ============================================================

# CUDA
# Do not FORCE cache defaults here. Command-line -D values are the build API used by
# presets and CI jobs; a local EnableFeatures.cmake may still opt in to FORCE when the
# developer intentionally wants the file to own the configuration.
option(WITH_CUDA "Build CUDA/LibTorch/TensorRT integration" OFF)
set(LIBTORCH_DIR "" CACHE PATH "LibTorch root directory")
set(TENSORRT_DIR "" CACHE PATH "TensorRT root directory")

# NRD
option(WITH_NRD "Build NVIDIA NRD integration" OFF)
set(NRD_ROOT "" CACHE PATH "NRD source root directory")

# RenderDoc
option(WITH_RENDERDOC "Build RenderDoc integration" OFF)
set(RENDERDOC_ROOT "" CACHE PATH "RenderDoc SDK root directory")

# Profile
option(WITH_PROFILE "Build profiling integration" OFF)

# Local matrix validation needs command-line feature values to be authoritative
# while leaving a developer's normal EnableFeatures.cmake untouched.
option(MOER_IGNORE_ENABLE_FEATURES "Ignore local EnableFeatures.cmake overrides" OFF)

# ============================================================
# Load user overrides from EnableFeatures.cmake
# template: template.EnableFeatures.cmake
# ============================================================

if (MOER_IGNORE_ENABLE_FEATURES)
    message(STATUS "EnableFeatures.cmake ignored for explicit feature validation")
elseif (EXISTS "${CMAKE_SOURCE_DIR}/EnableFeatures.cmake")
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
