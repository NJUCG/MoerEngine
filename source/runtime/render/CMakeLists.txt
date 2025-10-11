set(MODULE_NAME MoerRender)
set(MODULE_DIR ${moer_runtime_dir}/render)

configure_file(${MODULE_DIR}/config.h.in ${MODULE_DIR}/config.h)

file(GLOB_RECURSE RENDER_MODULE_HEADERS "*.h" ".hpp" "*.hlsli")
file(GLOB_RECURSE RENDER_MODULE_SOURCES "*.cpp")


source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES} )

add_library(${MODULE_NAME} SHARED ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

target_compile_definitions(${MODULE_NAME} PRIVATE RENDER_MODULE_SHARED_LIB)

# 注意，这里必须为PUBLIC，否则会发生奇妙的溢出错误。并且editor模块中也需要使用这个宏
if (CUDA_PASS_IN_RASTER)
    target_compile_definitions(${MODULE_NAME} PUBLIC CUDA_PASS_IN_RASTER=1)
    message(STATUS "CUDA_PASS_IN_RASTER=1 in Moer::Render")
else()
    target_compile_definitions(${MODULE_NAME} PUBLIC CUDA_PASS_IN_RASTER=0)
    message(STATUS "CUDA_PASS_IN_RASTER=0 in Moer::Render")
endif()

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

target_compile_definitions(${MODULE_NAME} PUBLIC -DMOER_AGILITY_SDK_VERSION=${MOER_AGILITYSDK_VERSION_MAJOR})
# agilitysdk need to export symbols from exe, however current render module is dll, so not easy for exe to import this then export again...
# so here use PUBLIC not PRIVATE
# maybe ref https://github.com/TheRealMJP/SHforHLSL/blob/main/SHTest/SampleFramework12/v1.04/Graphics/DX12.cpp#L141-L178


if(WIN32)
target_compile_definitions(${MODULE_NAME}
    PUBLIC "MOER_VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/Windows"
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

if(WIN32)
target_compile_definitions(MoerCore
    PUBLIC "DEVELOP_SHADER_SHARED_PATH=${CMAKE_CURRENT_SOURCE_DIR}/shaderheaders"
    )
endif()

add_dependencies(${MODULE_NAME} CopyNRDEncodingFile)






