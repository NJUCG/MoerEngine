
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

set(NRD_SOURCE_ROOT "${NRD_ROOT}")
set(NRD_OVERLAY_ROOT "${CMAKE_CURRENT_BINARY_DIR}/_overlay/NRD")

# Keep the submodule pristine: copy upstream NRD into a build-local overlay and patch only the overlay.
file(REMOVE_RECURSE "${NRD_OVERLAY_ROOT}")
file(MAKE_DIRECTORY "${NRD_OVERLAY_ROOT}")

file(GLOB NRD_OVERLAY_CONTENT RELATIVE "${NRD_SOURCE_ROOT}" "${NRD_SOURCE_ROOT}/*")
foreach(nrd_entry IN LISTS NRD_OVERLAY_CONTENT)
	file(COPY "${NRD_SOURCE_ROOT}/${nrd_entry}" DESTINATION "${NRD_OVERLAY_ROOT}")
endforeach()

set(NRD_CMAKE_FILE "${NRD_OVERLAY_ROOT}/CMakeLists.txt")
file(READ "${NRD_CMAKE_FILE}" NRD_CMAKE_FILE_CONTENT)
# Our bundled ShaderMake path works with the current flatten/strip flags, but the legacy --useAPI flag from upstream breaks this toolchain.
string(REPLACE "--useAPI --flatten --stripReflection --WX" "--flatten --stripReflection --WX" NRD_CMAKE_FILE_CONTENT "${NRD_CMAKE_FILE_CONTENT}")
file(WRITE "${NRD_CMAKE_FILE}" "${NRD_CMAKE_FILE_CONTENT}")

set(NRD_INTEGRATION_FILE "${NRD_OVERLAY_ROOT}/Integration/NRDIntegration.hpp")
file(READ "${NRD_INTEGRATION_FILE}" NRD_INTEGRATION_FILE_CONTENT)
# NRD v4.14.3 still assumes the pre-v170 NRI major/minor macro split; NRI v170 exposes only NRI_VERSION.
string(REPLACE
	"#endif\n\nstatic_assert(NRD_VERSION_MAJOR >= 4 && NRD_VERSION_MINOR >= 14, \"Unsupported NRD version!\");"
	"#endif\n\n#ifndef NRI_VERSION_MAJOR\n    #define NRI_VERSION_MAJOR 1\n#endif\n\n#ifndef NRI_VERSION_MINOR\n    #define NRI_VERSION_MINOR NRI_VERSION\n#endif\n\nstatic_assert(NRD_VERSION_MAJOR >= 4 && NRD_VERSION_MINOR >= 14, \"Unsupported NRD version!\");"
	NRD_INTEGRATION_FILE_CONTENT
	"${NRD_INTEGRATION_FILE_CONTENT}"
)
# NRI v170 collapsed DeviceDesc version reporting into nriVersion.
string(REPLACE
	"if (deviceDesc.nriVersionMajor != NRI_VERSION_MAJOR || deviceDesc.nriVersionMinor != NRI_VERSION_MINOR)"
	"if (deviceDesc.nriVersion != NRI_VERSION_MINOR)"
	NRD_INTEGRATION_FILE_CONTENT
	"${NRD_INTEGRATION_FILE_CONTENT}"
)
# NRI v170 moved the global SPIR-V offset toggle into PipelineLayoutBits flags.
string(REPLACE
	"        pipelineLayoutDesc.ignoreGlobalSPIRVOffsets = true;"
	"        pipelineLayoutDesc.flags = nri::PipelineLayoutBits::IGNORE_GLOBAL_SPIRV_OFFSETS;"
	NRD_INTEGRATION_FILE_CONTENT
	"${NRD_INTEGRATION_FILE_CONTENT}"
)
# NRI v170 nested alignment limits under memoryAlignment.
string(REPLACE
	"deviceDesc.constantBufferOffsetAlignment"
	"deviceDesc.memoryAlignment.constantBufferOffset"
	NRD_INTEGRATION_FILE_CONTENT
	"${NRD_INTEGRATION_FILE_CONTENT}"
)
file(WRITE "${NRD_INTEGRATION_FILE}" "${NRD_INTEGRATION_FILE_CONTENT}")

set(NRD_RESOURCE_FILE "${NRD_OVERLAY_ROOT}/Resources/NRD.rc")
file(READ "${NRD_RESOURCE_FILE}" NRD_RESOURCE_FILE_CONTENT)
# llvm-rc on the clang+ninja path rejects the upstream copyright symbol; rewrite it to ASCII in the overlay only.
string(REPLACE "Copyright © 2023" "Copyright (C) 2023" NRD_RESOURCE_FILE_CONTENT "${NRD_RESOURCE_FILE_CONTENT}")
file(WRITE "${NRD_RESOURCE_FILE}" "${NRD_RESOURCE_FILE_CONTENT}")

set(NRD_ROOT "${NRD_OVERLAY_ROOT}" CACHE PATH "NRD root directory" FORCE)

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

# NRD
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "Disable legacy DXBC shaders" FORCE)
set(NRD_DXC_PATH ${DXC_PATH})
set(NRD_DXC_SPIRV_PATH ${DXC_SPIRV_PATH})
set(NRD_SHADERS_PATH ${NRD_ROOT}/Shaders/Binary CACHE STRING "")
set(NRD_NORMAL_ENCODING "0" CACHE STRING "")
set(NRD_ROUGHNESS_ENCODING "1" CACHE STRING "")
message(STATUS NRD_DXC_PATH=${NRD_DXC_PATH})
message(STATUS NRD_ROOT=${NRD_ROOT})

add_subdirectory(${NRD_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/NRD)

target_compile_options(
    NRD
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-return-type-c-linkage>"
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-switch>"
)
add_dependencies(NRD NRI copy_dll_dxc)

# post build
set(NRD_ENCODING_FILE "${NRD_ROOT}/Shaders/Include/NRDEncoding.hlsli")
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