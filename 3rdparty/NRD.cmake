
# This file will be compiled if and only if WITH_NRD is ON

if (NOT WITH_NRD)
	message(FATAL_ERROR "NRD is disabled. NRD.cmake should not be included.")
endif()

if (NOT NRD_ROOT OR NRD_ROOT STREQUAL "/path/to/nrd")
	set(NRD_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/NRD" CACHE PATH "NRD root directory" FORCE)
endif()

if (NOT EXISTS "${NRD_ROOT}/CMakeLists.txt")
	message(FATAL_ERROR "NRD source tree not found at ${NRD_ROOT}. Initialize the submodule with: git submodule update --init --recursive 3rdparty/NRD")
endif()

# ShaderMake for NRD
if (WIN32)
	set(REDIST_DXC "${MOER_DXC_EXECUTABLE}")
    message(STATUS "REDIST_DXC=${REDIST_DXC}")
else()
	message(FATAL_ERROR "The vendored DXC package currently only supports Windows x64.")
endif()
if (EXISTS "${REDIST_DXC}")
	if (WIN32 AND NOT DXC_PATH)
		set(DXC_PATH "${REDIST_DXC}" CACHE STRING "Path to DirectX Shader Compiler for DXIL output")
	endif()
	if (NOT DXC_SPIRV_PATH)
		set(DXC_SPIRV_PATH "${REDIST_DXC}" CACHE STRING "Path to DirectX Shader Compiler for SPIR-V output")
	endif()
endif()
# Have ShaderMake use included DXC
option(SHADERMAKE_FIND_DXC "" OFF)
option(SHADERMAKE_FIND_DXC_SPIRV "" OFF)
# But do have ShaderMake find FXC for compiling NRD shaders
option(SHADERMAKE_FIND_FXC "" ON)

set(MOER_SHADERMAKE_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/shadermake-src")
if (EXISTS "${MOER_SHADERMAKE_CACHE_DIR}/CMakeLists.txt")
	set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${MOER_SHADERMAKE_CACHE_DIR}" CACHE PATH "" FORCE)
endif()

set(MOER_MATHLIB_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/mathlib-src")
if (EXISTS "${MOER_MATHLIB_CACHE_DIR}/CMakeLists.txt")
	set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${MOER_MATHLIB_CACHE_DIR}" CACHE PATH "" FORCE)
endif()

# NRD
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "Disable legacy DXBC shaders" FORCE)
set(NRD_DXC_PATH ${DXC_PATH})
set(NRD_DXC_SPIRV_PATH ${DXC_SPIRV_PATH})
# Keep the NRD submodule pristine: generate shader blobs in the parent build directory instead of the source tree.
set(NRD_SHADERS_PATH ${CMAKE_CURRENT_BINARY_DIR}/NRD/Shaders/Binary CACHE STRING "" FORCE)
set(NRD_NORMAL_ENCODING "0" CACHE STRING "")
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "")
message(STATUS NRD_DXC_PATH=${NRD_DXC_PATH})
message(STATUS NRD_ROOT=${NRD_ROOT})

add_subdirectory(${NRD_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/NRD)

if (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	set(NRD_CLANG_COMPILE_OPTIONS
		-Wextra
		-Wno-missing-field-initializers
		-Werror
		-fvisibility=hidden
	)
	if (NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
		list(APPEND NRD_CLANG_COMPILE_OPTIONS -mssse3)
	endif()
	set_property(TARGET NRD PROPERTY COMPILE_OPTIONS "${NRD_CLANG_COMPILE_OPTIONS}")
endif()

target_compile_options(
    NRD
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
)
add_dependencies(NRD NRI copy_dll_dxc)

# post build
set(NRD_ENCODING_FILE "${NRD_ROOT}/Shaders/NRDConfig.hlsli")
set(MOER_SHADER_NRD_DIR "${moer_shader_dir}/external/nrd")
add_custom_command(
    OUTPUT "${MOER_SHADER_NRD_DIR}/NRDEncoding.hlsli"
    COMMAND ${CMAKE_COMMAND} -E copy "${NRD_ENCODING_FILE}" "${MOER_SHADER_NRD_DIR}/NRDEncoding.hlsli"
    DEPENDS "${NRD_ENCODING_FILE}"
    COMMENT "Copying NRDEncoding.hlsli to ${MOER_SHADER_NRD_DIR}"
)

add_custom_target(CopyNRDEncodingFile
    DEPENDS "${MOER_SHADER_NRD_DIR}/NRDEncoding.hlsli"
)