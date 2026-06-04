
# This file will be compiled if and only if WITH_NRD is ON

if (NOT WITH_NRD)
	message(FATAL_ERROR "NRD is disabled. NRD.cmake should not be included.")
endif()

# 检查${NRD_ROOT}路径是否存在
if (NOT EXISTS "${NRD_ROOT}")
	message(FATAL_ERROR "NRD_ROOT path does not exist: ${NRD_ROOT}. Please set the correct path to NRD library.")
endif()

# ShaderMake for NRD
if (WIN32)
	get_target_property(REDIST_DXC dxc DXC_TARGET_EXECUTABLE)
	if (NOT REDIST_DXC)
		message(FATAL_ERROR "Target property DXC_TARGET_EXECUTABLE is missing on target 'dxc'.")
	endif()
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

# NRD
set(NRD_DXC_PATH ${DXC_PATH})
set(NRD_DXC_SPIRV_PATH ${DXC_SPIRV_PATH})
set(NRD_SHADERS_PATH ${NRD_ROOT}/Shaders/Binary CACHE STRING "")
set(NRD_NORMAL_ENCODING "0" CACHE STRING "")
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "")
message(STATUS NRD_DXC_PATH=${NRD_DXC_PATH})

# NRD's CMakeLists.txt adds -fPIC for Clang when MSVC is false,
# but clang on Windows targeting MSVC doesn't support -fPIC.
if (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(NRD_MSVC_OVERRIDE ${MSVC})
    set(MSVC TRUE)
endif()

add_subdirectory(${NRD_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/NRD)

if (WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(MSVC ${NRD_MSVC_OVERRIDE})
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