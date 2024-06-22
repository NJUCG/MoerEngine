
set(third_party_folder "3rdParty")

include(vulkan.cmake)
include(${moer_root_dir}/cmake/functions.cmake)

copy_dll(dxc dxcompiler SHARED ${moer_third_party_dir}/DirectXShaderCompiler )
set_target_folder(dxc ${third_party_folder})

set(SPDLOG_USE_STD_FORMAT ON)
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_SANITIZE_ADDRESS ON)
add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER ${third_party_folder}/spdlog)
    
option(GLFW_BUILD_DOCS OFF)
option(GLFW_BUILD_EXAMPLES OFF)
option(GLFW_BUILD_TESTS OFF)
option(GLFW_INSTALL ON)
set(GLFW_LIBRARY_TYPE "STATIC")
add_subdirectory(glfw)
set_target_folder(glfw ${third_party_folder}/GLFW3)
set_target_folder(update_mappings ${third_party_folder}/GLFW3)

include(${CMAKE_CURRENT_SOURCE_DIR}/imgui.cmake)
set_target_folder(imgui ${third_party_folder}/imgui)

set(SPIRV_REFLECT_EXECUTABLE OFF)
set(SPIRV_REFLECT_STATIC_LIB ON)
add_subdirectory(SPIRV-Reflect)
set_target_folder(spirv-reflect-static ${third_party_folder}/spirv-reflect)

add_subdirectory(DirectX-Headers)
set_target_folder(DirectX-Headers ${third_party_folder}/dxheaders)
set_target_folder(DirectX-Guids ${third_party_folder}/dxheaders)

set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(json)
add_subdirectory(AssetLoad)

set(MI_USE_CXX OFF)
set(MI_BUILD_STATIC      ON)
set(MI_BUILD_OBJECT      OFF)
set(MI_BUILD_TESTS       OFF)
add_subdirectory(mimalloc)
target_compile_options(mimalloc-static PRIVATE "$<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-strict-prototypes>")
set_target_folder(mimalloc-static ${third_party_folder}/mimalloc)
set_target_folder(mimalloc ${third_party_folder}/mimalloc)

add_subdirectory(MeshProcess)
set_target_folder(MeshProcess ${third_party_folder})


