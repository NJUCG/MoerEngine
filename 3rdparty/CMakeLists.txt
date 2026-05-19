
set(third_party_folder "3rdparty")

# include(vulkan.cmake)
include(${moer_root_dir}/cmake/functions.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/dxc.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/pybind11.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/python312.cmake)

# spdlog
set(SPDLOG_USE_STD_FORMAT ON)
set(SPDLOG_BUILD_SHARED ON) # 需要build shared，否则Moer::Core模块设置log_level为debug，但是Moer::Render模块使用spdlog静态库时，log_level又是info，导致日志无法输出（一个经典的静态库/动态库问题）
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

# nfd (NativeFileDialogExtended, used by imgui, for file selection)
# Tip: dependencies: https://github.com/btzy/nativefiledialog-extended?tab=readme-ov-file#dependencies
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/nativefiledialog-extended-1.2.1)
set_target_folder(native_file_dialog_extended ${third_party_folder}/nativefiledialog-extended-1.2.1)

add_subdirectory(d3d12_agilitysdk)
set_target_folder(d3d12_agilitysdk ${third_party_folder}/d3d12_agilitysdk)
set_target_folder(d3d12_agilitysdk_d3dx12 ${third_party_folder}/d3d12_agilitysdk)

add_subdirectory(D3D12MemoryAllocator)
set_target_folder(D3D12MemoryAllocator ${third_party_folder}/D3D12MemoryAllocator)

add_subdirectory(winpixeventruntime)
set_target_folder(winpixeventruntime ${third_party_folder}/winpixeventruntime)

# json
set(JSON_BuildTests OFF CACHE INTERNAL "")
add_subdirectory(json)

# asset load (including assimp, stb, exr, etc...)
add_subdirectory(AssetLoad)

# mimalloc
set(MI_USE_CXX OFF)
set(MI_BUILD_STATIC      ON)
set(MI_BUILD_SHARED      ON)
set(MI_BUILD_OBJECT      OFF)
set(MI_WIN_REDIRECT      ON)
set(MI_BUILD_TESTS       OFF)
add_subdirectory(mimalloc)

string(TOLOWER "${CMAKE_BUILD_TYPE}" CMAKE_BUILD_TYPE_LC)
target_compile_options(mimalloc-static PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-strict-prototypes>")
target_compile_options(mimalloc-static PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-unused-variable>")
target_compile_options(mimalloc-static PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wno-missing-field-initializers>")
# set_target_folder(mimalloc-${CMAKE_BUILD_TYPE_LC} ${third_party_folder}/mimalloc)
set_target_folder(mimalloc-static ${third_party_folder}/mimalloc)

# message(STATUS "Found mimalloc installed at: ${MIMALLOC_LIBRARY_DIR} (${MIMALLOC_VERSION_DIR})")
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

set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION ${MOER_WINDOWS_SDK_VERSION})


# toml++
include(${CMAKE_CURRENT_SOURCE_DIR}/tomlplusplus.cmake)
set_target_folder(tomlplusplus ${third_party_folder}/tomlplusplus-3.4.0)

# entt
include(${CMAKE_CURRENT_SOURCE_DIR}/entt.cmake)
set_target_folder(entt ${third_party_folder}/entt)

# gtl
include(gtl.cmake)
set_target_folder(gtl ${third_party_folder}/gtl)

if (WITH_NRD)
    # NRI, NRD, SPIRV-Cross
    include(NRI.cmake)
    include(NRD.cmake)
endif()

include(SPIRV-Cross.cmake)
include(libhv.cmake)