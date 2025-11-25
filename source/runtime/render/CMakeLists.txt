set(MODULE_NAME MoerRender)
set(MODULE_DIR ${moer_runtime_dir}/render)

configure_file(${MODULE_DIR}/config.h.in ${MODULE_DIR}/config.h)

file(GLOB_RECURSE RENDER_MODULE_HEADERS "*.h" ".hpp" "*.hlsli")
file(GLOB_RECURSE RENDER_MODULE_SOURCES "*.cpp")


source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES} )

add_library(${MODULE_NAME} SHARED ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

target_compile_definitions(${MODULE_NAME} PRIVATE RENDER_MODULE_SHARED_LIB)

if (MSVC)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${MODULE_NAME} PRIVATE "-Wno-nullability-completeness" "-Wno-unknown-attributes")
endif()

set_target_properties(${MODULE_NAME} PROPERTIES FOLDER "runtime")

target_include_directories(${MODULE_NAME}
	PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}>
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}>
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
target_link_libraries(${MODULE_NAME}
    # PUBLIC ${vulkan_lib}
    PUBLIC volk
	PUBLIC dxc
    PUBLIC glfw
    PUBLIC ImGui::imgui
    PUBLIC smaa
    PUBLIC MoerCore
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
if (TARGET moer_cuda)
    target_link_libraries(${MODULE_NAME}
        PUBLIC moer_cuda # Link CUDA and add includes
    )
    message(STATUS "Moer::Render linked Moer::Cuda")
endif()

# 设置WITH_CUDA宏
# 注意，这里必须为PUBLIC，需要扩散到其他模块
if (WITH_CUDA)
    target_compile_definitions(${MODULE_NAME} PUBLIC WITH_CUDA=1)
    message(STATUS "WITH_CUDA=1 in Moer::Render")
else()
# 不启用
    target_compile_definitions(${MODULE_NAME} PUBLIC WITH_CUDA=0)
    message(STATUS "WITH_CUDA=0 in Moer::Render")
endif()

target_compile_definitions(${MODULE_NAME} PUBLIC -DMOER_AGILITY_SDK_VERSION=${MOER_AGILITYSDK_VERSION_MAJOR})
# agilitysdk need to export symbols from exe, however current render module is dll, so not easy for exe to import this then export again...
# so here use PUBLIC not PRIVATE
# maybe ref https://github.com/TheRealMJP/SHforHLSL/blob/main/SHTest/SampleFramework12/v1.04/Graphics/DX12.cpp#L141-L178


if(WIN32)
target_compile_definitions(${MODULE_NAME}
    PUBLIC "NOMINMAX"
    )
elseif(LINUX)
endif()

if(MSVC)
    if (NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        MultiThreaded$<$<CONFIG:Debug>:Debug>DLL)
    endif()
    set_property(TARGET ${MODULE_NAME} PROPERTY
             MSVC_RUNTIME_LIBRARY  "${CMAKE_MSVC_RUNTIME_LIBRARY}")

endif() 

add_dependencies(${MODULE_NAME} CopyNRDEncodingFile)

# MARK: Copy Resources

# 定义拷贝资源的输出目录
if(WIN32)
    target_compile_definitions(MoerCore # 注意，这里是MoerCore，因为ConfigManager在Core模块中
        PUBLIC "SHADER_PATH_RELATIVE_TO_RESOURCE=shaders"
        PUBLIC "SHADER_SHARED_PATH_RELATIVE_TO_RESOURCE=shaderheaders"
    )
endif()

if(CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
else()
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") # 这是什么？
endif()

# 检查MoerEngine.toml是否存在
set(MOER_ENGINE_CONFIG_FILE "${moer_root_dir}/MoerEngine.toml")

# 2. 检查文件是否存在
if(NOT EXISTS "${MOER_ENGINE_CONFIG_FILE}")
    # 3. 如果不存在，直接报错停止，并打印清晰的路径
    message(FATAL_ERROR "\nConfig file not found at path: ${MOER_ENGINE_CONFIG_FILE}.\nPlease generate MoerEngine.toml based on the template.MoerEngine.toml. 请根据模板文件template.MoerEngine.toml生成MoerEngine.toml配置文件。")
endif()

# 设置输出目录
add_custom_target(
	CopyResources
    COMMAND ${CMAKE_COMMAND} -E copy "${moer_root_dir}/MoerEngine.toml"                    "${output_dir}/MoerEngine.toml"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}/resource"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_root_dir}/asset"                    "${output_dir}/resource"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_root_dir}/shaders"                  "${output_dir}/resource/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/shaderheaders" "${output_dir}/resource/shaderheaders"
    COMMENT "Copying resources to ${output_dir}/resource"
)

add_dependencies(${MODULE_NAME} CopyResources)


