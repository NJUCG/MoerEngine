
set(third_party_folder "3rdParty")

include(vulkan.cmake)
include(${moer_root_dir}/cmake/functions.cmake)

copy_dll(dxc dxcompiler SHARED ${moer_third_party_dir}/DirectXShaderCompiler )
set_target_folder(dxc ${third_party_folder})

# spdlog
set(SPDLOG_USE_STD_FORMAT ON)
set(SPDLOG_BUILD_SHARED OFF)
set(SPDLOG_SANITIZE_ADDRESS ON)
add_subdirectory(spdlog)
set_target_properties(spdlog PROPERTIES FOLDER ${third_party_folder}/spdlog)

# glfw
option(GLFW_BUILD_DOCS OFF)
option(GLFW_BUILD_EXAMPLES OFF)
option(GLFW_BUILD_TESTS OFF)
option(GLFW_INSTALL ON)
set(GLFW_LIBRARY_TYPE "SHARED")
add_subdirectory(glfw)
set_target_folder(glfw ${third_party_folder}/GLFW3)
set_target_folder(update_mappings ${third_party_folder}/GLFW3)

# imgui
include(${CMAKE_CURRENT_SOURCE_DIR}/imgui.cmake)
set_target_folder(imgui ${third_party_folder}/imgui)

# spirv-reflect
set(SPIRV_REFLECT_EXECUTABLE OFF)
set(SPIRV_REFLECT_STATIC_LIB ON)
add_subdirectory(SPIRV-Reflect)
set_target_folder(spirv-reflect-static ${third_party_folder}/spirv-reflect)

# directx-headers
add_subdirectory(DirectX-Headers)
set_target_folder(DirectX-Headers ${third_party_folder}/dxheaders)
set_target_folder(DirectX-Guids ${third_party_folder}/dxheaders)

# json
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(json)

# asset load (including assimp, stb, exr, etc...)
add_subdirectory(AssetLoad)

# mimalloc
set(MI_USE_CXX OFF)
set(MI_BUILD_STATIC      ON)
set(MI_BUILD_OBJECT      OFF)
set(MI_BUILD_TESTS       OFF)
add_subdirectory(mimalloc)
target_compile_options(mimalloc-static PRIVATE "$<$<COMPILE_LANG_AND_ID:C,Clang>:-Wno-strict-prototypes>")
set_target_folder(mimalloc-static ${third_party_folder}/mimalloc)
set_target_folder(mimalloc ${third_party_folder}/mimalloc)

# mesh process
add_subdirectory(MeshProcess)
set_target_folder(MeshProcess ${third_party_folder})

# volk
if (WIN32)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
elseif(LINUX)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_XLIB_KHR)
endif()
set(VULKAN_HEADERS_INSTALL_DIR ${moer_third_party_dir}/VulkanSDK)
add_subdirectory(volk)
set_target_folder(volk ${third_party_folder}/volk)

# smaa
include(${CMAKE_CURRENT_SOURCE_DIR}/smaa.cmake)
set_target_folder(smaa ${third_party_folder}/smaa)

# Find Windows SDK
get_filename_component (MOER_WINDOWS_SDK_PATH "[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows Kits\\Installed Roots;KitsRoot10]" ABSOLUTE CACHE)
if (MOER_WINDOWS_SDK_PATH)
    message(STATUS "Windows SDK found: ${MOER_WINDOWS_SDK_PATH}")
else ()
    message(WARNING "Windows SDK not found. DirectX backend, NRI and NRD may not build correctly.")
endif ()
find_file(MOER_WINDOWS_SDK_MANIFEST_PATH "SDKManifest.xml" HINTS "${MOER_WINDOWS_SDK_PATH}" NO_DEFAULT_PATH)
if (MOER_WINDOWS_SDK_MANIFEST_PATH)
    file(STRINGS "${MOER_WINDOWS_SDK_MANIFEST_PATH}" MOER_WINDOWS_SDK_IDENTITY REGEX "PlatformIdentity")
    if (MOER_WINDOWS_SDK_IDENTITY)
        string(REGEX REPLACE "PlatformIdentity = \".*Version=([0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+)\"" "\\1" MOER_WINDOWS_SDK_VERSION "${MOER_WINDOWS_SDK_IDENTITY}")
        string(STRIP "${MOER_WINDOWS_SDK_VERSION}" MOER_WINDOWS_SDK_VERSION)
        if (MOER_WINDOWS_SDK_VERSION MATCHES "PlatformIdentity")
            set(MOER_WINDOWS_SDK_VERSION "")
        endif ()
    endif ()
    if (MOER_WINDOWS_SDK_VERSION)
        message(STATUS "Windows SDK found: ${MOER_WINDOWS_SDK_VERSION}")
    else ()
        message(WARNING "Windows SDK found but failed to parse the version. DirectX backend, NRI and NRD may not build correctly.")
    endif ()
else ()
    message(WARNING "Windows SDK not found. DirectX backend, NRI and NRD may not build correctly.")
endif ()

# NRI
set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION ${MOER_WINDOWS_SDK_VERSION})
add_subdirectory(NRI)

# NRD
set(NRD_SHADERS_PATH ${moer_third_party_dir}/NRD/Shaders/Binary)
set(NRD_NORMAL_ENCODING "0")
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-msse4.1)
endif()
add_subdirectory(NRD)

# toml++
include(${CMAKE_CURRENT_SOURCE_DIR}/tomlplusplus.cmake)
set_target_folder(tomlplusplus ${third_party_folder}/tomlplusplus)

# post build
set(NRD_ENCODING_FILE "${moer_third_party_dir}/NRD/Shaders/Include/NRDEncoding.hlsli")
set(MOER_SHADER_NRD_DIR "${moer_shader_dir}/nrd")
add_custom_command(
    OUTPUT "${MOER_SHADER_NRD_DIR}/NRDEncoding.hlsli"
    COMMAND ${CMAKE_COMMAND} -E copy "${NRD_ENCODING_FILE}" "${MOER_SHADER_NRD_DIR}/NRDEncoding.hlsli"
    DEPENDS "${NRD_ENCODING_FILE}"
    COMMENT "Copying NRDEncoding.hlsli to ${MOER_SHADER_NRD_DIR}"
)

add_custom_target(CopyNRDEncodingFile ALL
    DEPENDS "${MOER_SHADER_NRD_DIR}/NRDEncoding.hlsli"
)

add_dependencies(NRD CopyNRDEncodingFile)