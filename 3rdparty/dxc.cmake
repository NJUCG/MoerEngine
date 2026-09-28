if (NOT WIN32)
    # Use a native DXC installation on macOS/Linux. MOER_DXC_ROOT can select a
    # specific SDK; otherwise derive the installation prefix from dxc on PATH.
    set(MOER_DXC_ROOT "" CACHE PATH "Root of a native DXC installation")

    if (MOER_DXC_ROOT)
        set(dxc_target_root "${MOER_DXC_ROOT}")
        find_program(dxc_target_executable NAMES dxc
            HINTS "${dxc_target_root}/bin" NO_DEFAULT_PATH NO_CACHE)
    else()
        set(_dxc_program_hints)
        if (DEFINED ENV{VULKAN_SDK})
            list(APPEND _dxc_program_hints "$ENV{VULKAN_SDK}/bin")
        endif()
        find_program(dxc_target_executable NAMES dxc
            HINTS ${_dxc_program_hints} NO_CACHE)
        if (dxc_target_executable)
            get_filename_component(dxc_target_binary_dir "${dxc_target_executable}" DIRECTORY)
            get_filename_component(dxc_target_root "${dxc_target_binary_dir}" DIRECTORY)
        endif()
    endif()

    if (NOT dxc_target_executable)
        message(FATAL_ERROR
            "Native DXC executable not found. Install DXC or set MOER_DXC_ROOT.")
    endif()

    find_path(dxc_target_include_dir NAMES dxcapi.h
        HINTS "${dxc_target_root}/include/dxc" "${dxc_target_root}/include"
        NO_DEFAULT_PATH NO_CACHE)
    find_library(dxc_target_library NAMES dxcompiler
        HINTS "${dxc_target_root}/lib" "${dxc_target_root}/lib64"
        NO_DEFAULT_PATH NO_CACHE)

    if (NOT dxc_target_include_dir OR NOT dxc_target_library)
        message(FATAL_ERROR
            "Native DXC headers or library not found under ${dxc_target_root}. "
            "Set MOER_DXC_ROOT to an installation containing bin/dxc, "
            "include/dxc/dxcapi.h, and lib/libdxcompiler.")
    endif()

    add_library(dxc SHARED IMPORTED GLOBAL)
    set_target_properties(dxc PROPERTIES
        IMPORTED_LOCATION "${dxc_target_library}"
        INTERFACE_INCLUDE_DIRECTORIES "${dxc_target_include_dir}"
        DXC_TARGET_ROOT "${dxc_target_root}"
        DXC_TARGET_INCLUDE_DIR "${dxc_target_include_dir}"
        DXC_TARGET_EXECUTABLE "${dxc_target_executable}"
    )
    set_target_folder(dxc ${third_party_folder})
    message(STATUS "Using native DXC: ${dxc_target_library}")
    return()
endif()

set(dxc_target_root "${CMAKE_CURRENT_SOURCE_DIR}/dxc_2026_02_20")
set(dxc_target_include_dir "${dxc_target_root}/inc")
set(dxc_target_binary_dir "${dxc_target_root}/bin/x64")
set(dxc_target_library_dir "${dxc_target_root}/lib/x64")
set(dxc_target_executable "${dxc_target_binary_dir}/dxc.exe")
set(dxc_target_dxcompiler_dll "${dxc_target_binary_dir}/dxcompiler.dll")
set(dxc_target_dxil_dll "${dxc_target_binary_dir}/dxil.dll")
set(dxc_target_import_library "${dxc_target_library_dir}/dxcompiler.lib")

# The vendored package intentionally keeps only the x64 redistributable.
# x86 and arm64 executables are not included in the repository at the moment.
foreach(path
    ${dxc_target_root}
    ${dxc_target_include_dir}
    ${dxc_target_binary_dir}
    ${dxc_target_library_dir}
    ${dxc_target_executable}
    ${dxc_target_dxcompiler_dll}
    ${dxc_target_dxil_dll}
    ${dxc_target_import_library}
)
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "Required DXC package path does not exist: ${path}")
    endif()
endforeach()

if (NOT TARGET copy_dll_dxc)
    add_custom_target(copy_dll_dxc)
    set_target_properties(copy_dll_dxc PROPERTIES FOLDER "utils")
endif()

if (NOT TARGET dxc)
    add_library(dxc SHARED IMPORTED GLOBAL)
endif()

message(STATUS "copy ${dxc_target_binary_dir} to ${real_out_put_dir}")
add_custom_command(
    TARGET copy_dll_dxc
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${dxc_target_binary_dir}"
    ${real_out_put_dir}
)

set_target_properties(dxc PROPERTIES
    IMPORTED_IMPLIB "${dxc_target_import_library}"
    IMPORTED_LOCATION "${dxc_target_dxcompiler_dll}"
    INTERFACE_INCLUDE_DIRECTORIES "${dxc_target_include_dir}"
    DXC_TARGET_ROOT "${dxc_target_root}"
    DXC_TARGET_INCLUDE_DIR "${dxc_target_include_dir}"
    DXC_TARGET_BINARY_DIR "${dxc_target_binary_dir}"
    DXC_TARGET_LIBRARY_DIR "${dxc_target_library_dir}"
    DXC_TARGET_EXECUTABLE "${dxc_target_executable}"
    DXC_TARGET_DXCOMPILER_DLL "${dxc_target_dxcompiler_dll}"
    DXC_TARGET_DXIL_DLL "${dxc_target_dxil_dll}"
    DXC_TARGET_IMPORT_LIBRARY "${dxc_target_import_library}"
)
add_dependencies(dxc copy_dll_dxc)
set_target_folder(dxc ${third_party_folder})
