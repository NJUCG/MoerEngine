
set(moer_third_party_dir "${moer_source_dir}/3rdparty")
set(moer_asset_dir "asset")
set(moer_config_dir "configs")

add_compile_definitions("__ENGINE_NAME__=${CMAKE_PROJECT_NAME}")

# Render Backend Options
set(moer_supported_moer_rhi_api Vulkan DX12)
set(moer_rhi_api Vulkan CACHE STRING "Chosen Render Hardware API")
set_property(CACHE moer_rhi_api PROPERTY STRINGS ${moer_supported_moer_rhi_api})

if(NOT moer_rhi_api IN_LIST moer_supported_moer_rhi_api)
	message(FATAL_ERROR "moer_rhi_api must be one of ${moer_supported_moer_rhi_api}")
endif()

if(moer_rhi_api STREQUAL Vulkan)
	add_compile_definitions("VULKAN_RHI=1")
elseif(moer_rhi_api STREQUAL DX12)
	add_compile_definitions("DX12_RHI=1")
endif()

message(STATUS "backend graphics api: ${moer_rhi_api}")

add_subdirectory(3rdParty)
add_subdirectory(runtime)
add_subdirectory(editor)

if(${moer_build_test})
	add_subdirectory(test)
endif()