
set(third_party_folder "3rdParty")

include(vulkan.cmake)

if(CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
    set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
else() 
    set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") 
endif()

add_library(dxc SHARED IMPORTED GLOBAL)
add_custom_target(DXC_COPY)
#DXC
if(WIN32)
    set(dxc_lib "${moer_third_party_dir}/DirectXShaderCompiler/lib/Windows/dxcompiler.lib" )
    set(dxc_runtime_dir "${moer_third_party_dir}/DirectXShaderCompiler/bin/Windows")

    add_custom_command(
        TARGET DXC_COPY
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${dxc_runtime_dir}/dxcompiler.dll"
        ${real_out_put_dir}
    )

    set_target_properties(dxc PROPERTIES 
     IMPORTED_IMPLIB  ${dxc_lib}
     IMPORTED_LOCATION  ${dxc_runtime_dir}/dxcompiler.dll)

     add_dependencies(dxc DXC_COPY)

elseif(LINUX)
endif()

set(SPDLOG_USE_STD_FORMAT ON)
set(SPDLOG_BUILD_SHARED ON)
set(SPDLOG_SANITIZE_ADDRESS ON)
add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER ${third_party_folder})
    
option(GLFW_BUILD_DOCS OFF)
option(GLFW_BUILD_EXAMPLES OFF)
option(GLFW_BUILD_TESTS OFF)
option(GLFW_INSTALL ON)
set(GLFW_LIBRARY_TYPE "STATIC")
add_subdirectory(glfw)

include(${CMAKE_CURRENT_SOURCE_DIR}/imgui.cmake)

set(SPIRV_REFLECT_EXECUTABLE OFF)
set(SPIRV_REFLECT_STATIC_LIB ON)
add_subdirectory(SPIRV-Reflect)

add_subdirectory(DirectX-Headers)

add_subdirectory(json)
add_subdirectory(AssetLoad)

set(MI_USE_CXX OFF)
set(MI_BUILD_STATIC      ON)
set(MI_BUILD_OBJECT      OFF)
set(MI_BUILD_TESTS       OFF)
add_subdirectory(mimalloc)
target_compile_options(mimalloc-static PRIVATE "$<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-strict-prototypes>")

add_subdirectory(MeshProcess)


