set(MODULE_NAME render)
set(MODULE_DIR ${moer_runtime_module_root_dir}/${MODULE_NAME})

configure_file(${MODULE_DIR}/include/config.h.in ${MODULE_DIR}/include/config.h)

file(GLOB_RECURSE RENDER_MODULE_HEADERS "include/*.h")
file(GLOB_RECURSE RENDER_MODULE_SOURCES "source/*.cpp" "source/*.h")


source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES} )

add_library(${MODULE_NAME} SHARED ${RENDER_MODULE_HEADERS} ${RENDER_MODULE_SOURCES})

target_compile_definitions(${MODULE_NAME} PRIVATE RENDER_MODULE_SHARED_LIB)

if (MSVC)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${MODULE_NAME} PRIVATE "-Wno-nullability-completeness" "-Wno-unknown-attributes")
endif()

set_target_properties(${MODULE_NAME} PROPERTIES FOLDER "runtime")

target_include_directories(${MODULE_NAME}
	PUBLIC $<BUILD_INTERFACE:${MODULE_DIR}/include>
    PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/SPIRV-Reflect>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/DirectX-Headers/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
	PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/VulkanSDK/include>
	PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/DirectXShaderCompiler/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/VulkanMemoryAllocator>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/imgui>
)
target_link_libraries(${MODULE_NAME}
    PUBLIC ${vulkan_lib}
    PRIVATE spirv-reflect-static
	PUBLIC dxc
    PUBLIC MoerRuntimeCore
    PUBLIC Microsoft::DirectX-Guids
    PUBLIC Microsoft::DirectX-Headers
    PUBLIC glfw
    PUBLIC ImGui::imgui
)

if(WINDOWS)
target_compile_definitions(${MODULE_NAME}
    PUBLIC "VK_LAYER_PATH=${moer_third_party_dir}/VulkanSDK/binary/Windows")
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






