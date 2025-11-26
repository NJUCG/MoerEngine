# Export Moer::Render target

set(target_name moer_render)

configure_file(${moer_runtime_render_dir}/config.h.in ${moer_runtime_render_dir}/config.h)

file(GLOB_RECURSE render_module_headers "*.h" ".hpp" "*.hlsli")
file(GLOB_RECURSE render_module_sources "*.cpp")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${render_module_headers} ${render_module_sources} )

add_library(${target_name} SHARED ${render_module_headers} ${render_module_sources})
add_library(Moer::Render ALIAS ${target_name})

target_compile_definitions(${target_name} PRIVATE RENDER_MODULE_SHARED_LIB)

if (MSVC)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target_name} PRIVATE "-Wno-nullability-completeness" "-Wno-unknown-attributes")
endif()

set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}>
	PUBLIC $<BUILD_INTERFACE:${moer_runtime_render_dir}>
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_resource_dir}>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/volk>
	PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/DirectXShaderCompiler/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/VulkanMemoryAllocator>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/imgui>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/smaa>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/NRI/Include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/NRD/Include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/NRD/Integration>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/SPIRV-Cross>
)
target_link_libraries(${target_name}
    # PUBLIC ${vulkan_lib}
    PUBLIC volk
	PUBLIC dxc
    PUBLIC glfw
    PUBLIC ImGui::imgui
    PUBLIC smaa
    PUBLIC Moer::Core
    PUBLIC NRI
    PUBLIC NRD
    PRIVATE spirv-cross-cpp
    # dx, maybe add a option based on platform
    PUBLIC d3d12_agilitysdk
    PUBLIC d3d12_agilitysdk_d3dx12
    PUBLIC d3d12 
    PUBLIC dxgi
    PUBLIC dxguid
    PUBLIC D3D12MemoryAllocator
    PUBLIC winpixeventruntime
    PUBLIC nfd
    PUBLIC AssetLoad
)

# Link CUDA
if (TARGET Moer::Cuda)
    target_link_libraries(${target_name}
        PUBLIC Moer::Cuda # Link CUDA and add includes
    )
    message(STATUS "Moer::Render linked Moer::Cuda")
endif()

# 设置WITH_CUDA宏
# 注意，这里必须为PUBLIC，需要扩散到其他模块
if (WITH_CUDA)
    target_compile_definitions(${target_name} PUBLIC WITH_CUDA=1)
    message(STATUS "WITH_CUDA=1 in Moer::Render")
else()
# 不启用
    target_compile_definitions(${target_name} PUBLIC WITH_CUDA=0)
    message(STATUS "WITH_CUDA=0 in Moer::Render")
endif()

target_compile_definitions(${target_name} PUBLIC -DMOER_AGILITY_SDK_VERSION=${MOER_AGILITYSDK_VERSION_MAJOR})
# agilitysdk need to export symbols from exe, however current render module is dll, so not easy for exe to import this then export again...
# so here use PUBLIC not PRIVATE
# maybe ref https://github.com/TheRealMJP/SHforHLSL/blob/main/SHTest/SampleFramework12/v1.04/Graphics/DX12.cpp#L141-L178


if(WIN32)
target_compile_definitions(${target_name}
    PUBLIC "NOMINMAX"
    )
elseif(LINUX)
endif()

if(MSVC)
    if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        MultiThreaded$<$<CONFIG:Debug>:Debug>DLL)
    endif()
    set_property(TARGET ${target_name} PROPERTY
             MSVC_RUNTIME_LIBRARY  "${CMAKE_MSVC_RUNTIME_LIBRARY}")

endif() 

add_dependencies(${target_name} CopyNRDEncodingFile)

# MARK: Copy Resources

# 定义拷贝资源的输出目录
if(WIN32)
    target_compile_definitions(${target_name} # 注意，这里是MoerCore，因为ConfigManager在Core模块中
        PUBLIC "SHADER_PATH_RELATIVE_TO_ASSET=shaders"
        PUBLIC "SHADER_SHARED_PATH_RELATIVE_TO_ASSET=shaderheaders"
    )
endif()

if(CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
else()
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") # 这是什么？
endif()

# 检查MoerEngine.toml是否存在
set(MOER_ENGINE_CONFIG_FILE "${moer_root_dir}/MoerEngine.toml")
if(NOT EXISTS "${MOER_ENGINE_CONFIG_FILE}")
    # 如果不存在，直接停止，并打印路径
    message(FATAL_ERROR "\nConfig file not found at path: ${MOER_ENGINE_CONFIG_FILE}.\nPlease generate MoerEngine.toml based on the template.MoerEngine.toml. 请根据模板文件template.MoerEngine.toml生成MoerEngine.toml配置文件。")
endif()

# 设置输出目录
add_custom_target(
	CopyResources ALL
    COMMAND ${CMAKE_COMMAND} -E copy "${moer_root_dir}/MoerEngine.toml"                    "${output_dir}/MoerEngine.toml"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}/asset"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_root_dir}/asset"                    "${output_dir}/asset"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_root_dir}/shaders"                  "${output_dir}/asset/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shaderheaders" "${output_dir}/asset/shaderheaders"
    COMMENT "Copying resources to ${output_dir}/asset"
)

# add_dependencies(${target_name} CopyResources)


