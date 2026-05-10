set(MOER_DXC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/dxc_2026_02_20")
set(MOER_DXC_INCLUDE_DIR "${MOER_DXC_ROOT}/inc")
set(MOER_DXC_BINARY_DIR "${MOER_DXC_ROOT}/bin/x64")
set(MOER_DXC_LIBRARY_DIR "${MOER_DXC_ROOT}/lib/x64")
set(MOER_DXC_EXECUTABLE "${MOER_DXC_BINARY_DIR}/dxc.exe")
set(MOER_DXC_DLL "${MOER_DXC_BINARY_DIR}/dxcompiler.dll")
set(MOER_DXC_DXIL_DLL "${MOER_DXC_BINARY_DIR}/dxil.dll")
set(MOER_DXC_IMPORT_LIBRARY "${MOER_DXC_LIBRARY_DIR}/dxcompiler.lib")

if (NOT WIN32)
    message(FATAL_ERROR "Vendored DXC currently only provides Windows x64 binaries.")
endif()

# The vendored package intentionally keeps only the x64 redistributable.
# x86 and arm64 executables are not included in the repository at the moment.
foreach(path
    ${MOER_DXC_ROOT}
    ${MOER_DXC_INCLUDE_DIR}
    ${MOER_DXC_BINARY_DIR}
    ${MOER_DXC_LIBRARY_DIR}
    ${MOER_DXC_EXECUTABLE}
    ${MOER_DXC_DLL}
    ${MOER_DXC_DXIL_DLL}
    ${MOER_DXC_IMPORT_LIBRARY}
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

message(STATUS "copy ${MOER_DXC_BINARY_DIR} to ${real_out_put_dir}")
add_custom_command(
    TARGET copy_dll_dxc
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${MOER_DXC_BINARY_DIR}"
    ${real_out_put_dir}
)

set_target_properties(dxc PROPERTIES
    IMPORTED_IMPLIB ${MOER_DXC_IMPORT_LIBRARY}
    IMPORTED_LOCATION ${MOER_DXC_DLL}
    INTERFACE_INCLUDE_DIRECTORIES "${MOER_DXC_INCLUDE_DIR}"
)
add_dependencies(dxc copy_dll_dxc)
set_target_folder(dxc ${third_party_folder})

set(MOER_DXC_ROOT "${MOER_DXC_ROOT}" PARENT_SCOPE)
set(MOER_DXC_INCLUDE_DIR "${MOER_DXC_INCLUDE_DIR}" PARENT_SCOPE)
set(MOER_DXC_BINARY_DIR "${MOER_DXC_BINARY_DIR}" PARENT_SCOPE)
set(MOER_DXC_LIBRARY_DIR "${MOER_DXC_LIBRARY_DIR}" PARENT_SCOPE)
set(MOER_DXC_EXECUTABLE "${MOER_DXC_EXECUTABLE}" PARENT_SCOPE)
set(MOER_DXC_DLL "${MOER_DXC_DLL}" PARENT_SCOPE)
set(MOER_DXC_DXIL_DLL "${MOER_DXC_DXIL_DLL}" PARENT_SCOPE)
set(MOER_DXC_IMPORT_LIBRARY "${MOER_DXC_IMPORT_LIBRARY}" PARENT_SCOPE)