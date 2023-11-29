

set(moer_asset_dir "asset")
set(moer_config_dir "configs")

add_subdirectory(runtime)
add_subdirectory(editor)

if(${moer_build_test})
	add_subdirectory(test)
endif()
