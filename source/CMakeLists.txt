
set(THIRD_PARTY_DIR "${ENGINE_ROOT_DIR}/3rdparty")
set(ENGINE_ASSET_DIR "asset")
set(ENGINE_CONFIG_DIR "configs")

add_compile_definitions("__ENGINE_NAME__=${CMAKE_PROJECT_NAME}")

##################################################
## Build Options
##################################################

#########################
## Render Backend Options
#########################

set(SUPPORTED_RHI_APIS Vulkan DX12)
set(RHI_API Vulkan CACHE STRING "Chosen Render Hardware API")
set_property(CACHE RHI_API PROPERTY STRINGS ${SUPPORTED_RHI_APIS})

if(NOT RHI_API IN_LIST SUPPORTED_RHI_APIS)
	message(FATAL_ERROR "RHI_API must be one of ${SUPPORTED_RHI_APIS}")
endif()

if(RHI_API STREQUAL  Vulkan)
	add_compile_definitions("VULKAN_RHI=1")
elseif(RHI_API STREQUAL DX12)
	add_compile_definitions("DX12_RHI=1")
endif()



##################################################
## Sub Directories
##################################################

ADD_SUBDIRECTORY(${THIRD_PARTY_DIR})

message(${RHI_API})

set(ENGINE_RUNTIME runtime)

ADD_SUBDIRECTORY(runtime)
ADD_SUBDIRECTORY(editor)