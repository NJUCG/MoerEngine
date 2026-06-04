## 限制当前 vendored CPython 包只在已支持的 Windows 主机上使用
if (NOT WIN32)
    message(FATAL_ERROR "Vendored Python 3.12 currently only provides Windows x64 binaries.")
endif()

## 统一定义 vendored CPython 的路径，方便下游通过 target property 获取而不是依赖零散变量
set(python312_target_root "${CMAKE_CURRENT_SOURCE_DIR}/python312/x64")
set(python312_target_include_dir "${python312_target_root}/include")
set(python312_target_library_dir "${python312_target_root}/libs")
set(python312_target_stdlib_dir "${python312_target_root}/Lib")
set(python312_target_dll_dir "${python312_target_root}/DLLs")
set(python312_target_runtime_dll "${python312_target_root}/python312.dll")
set(python312_target_runtime_dll_alt "${python312_target_root}/python3.dll")
set(python312_target_vcruntime_dll "${python312_target_root}/vcruntime140.dll")
set(python312_target_vcruntime_dll_alt "${python312_target_root}/vcruntime140_1.dll")
set(python312_target_import_library "${python312_target_library_dir}/python312.lib")

## 当 vendored Python 包布局不完整或路径放错时尽早报错
foreach(path
    ${python312_target_root}
    ${python312_target_include_dir}
    ${python312_target_library_dir}
    ${python312_target_stdlib_dir}
    ${python312_target_dll_dir}
    ${python312_target_runtime_dll}
    ${python312_target_runtime_dll_alt}
    ${python312_target_vcruntime_dll}
    ${python312_target_vcruntime_dll_alt}
    ${python312_target_import_library}
)
    if (NOT EXISTS "${path}")
        message(FATAL_ERROR "Required Python 3.12 package path does not exist: ${path}")
    endif()
endforeach()

## 创建一个辅助 target，专门负责把 Python 运行时文件拷贝到编辑器输出目录
if (NOT TARGET copy_dll_python312_embed)
    add_custom_target(copy_dll_python312_embed)
    set_target_properties(copy_dll_python312_embed PROPERTIES FOLDER "utils")
endif()

## 将 vendored Python embed 库暴露为 imported target，供普通 target_link_libraries 使用
if (NOT TARGET python312_embed)
    add_library(python312_embed SHARED IMPORTED GLOBAL)
endif()

## 将运行时 DLL 和 Python 标准库拷贝到最终可执行文件布局中
message(STATUS "copy ${python312_target_root} runtime to ${real_out_put_dir}")
add_custom_command(
    TARGET copy_dll_python312_embed
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${python312_target_runtime_dll}"
    ${real_out_put_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${python312_target_runtime_dll_alt}"
    ${real_out_put_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${python312_target_vcruntime_dll}"
    ${real_out_put_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${python312_target_vcruntime_dll_alt}"
    ${real_out_put_dir}
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${python312_target_stdlib_dir}"
    "${real_out_put_dir}/Lib"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${python312_target_dll_dir}"
    "${real_out_put_dir}/DLLs"
)

## 在 imported Python target 上发布链接、头文件、运行时和元数据属性
set_target_properties(python312_embed PROPERTIES
    IMPORTED_IMPLIB "${python312_target_import_library}"
    IMPORTED_LOCATION "${python312_target_runtime_dll}"
    INTERFACE_INCLUDE_DIRECTORIES "${python312_target_include_dir}"
    INTERFACE_LINK_OPTIONS "LINKER:/NODEFAULTLIB:python312_d.lib"
    PYTHON312_TARGET_ROOT "${python312_target_root}"
    PYTHON312_TARGET_INCLUDE_DIR "${python312_target_include_dir}"
    PYTHON312_TARGET_LIBRARY_DIR "${python312_target_library_dir}"
    PYTHON312_TARGET_STDLIB_DIR "${python312_target_stdlib_dir}"
    PYTHON312_TARGET_DLL_DIR "${python312_target_dll_dir}"
    PYTHON312_TARGET_RUNTIME_DLL "${python312_target_runtime_dll}"
    PYTHON312_TARGET_RUNTIME_DLL_ALT "${python312_target_runtime_dll_alt}"
    PYTHON312_TARGET_VCRUNTIME_DLL "${python312_target_vcruntime_dll}"
    PYTHON312_TARGET_VCRUNTIME_DLL_ALT "${python312_target_vcruntime_dll_alt}"
    PYTHON312_TARGET_IMPORT_LIBRARY "${python312_target_import_library}"
)

## 确保消费者会触发运行时拷贝步骤，并让这个 target 在 IDE 树里保持归类一致
add_dependencies(python312_embed copy_dll_python312_embed)
set_target_folder(python312_embed ${third_party_folder}/python312)
