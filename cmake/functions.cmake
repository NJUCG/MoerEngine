function(set_target_folder target_name folder_name)
    if(TARGET ${target_name})
        set_target_properties(${target_name} PROPERTIES FOLDER ${folder_name})
    else()
        message(STATUS "${target_name} does not exist.")
    endif()
endfunction()

# 与顶层 CMakeLists 一致：CMAKE_RUNTIME_OUTPUT_DIRECTORY 已含 $<CONFIG>，MSVC/Clang 均为 target/bin/<Config>/
set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

# copy dll and lib from src_root to out_put_dir
function(copy_dll target_name dll_name type src_root )
    if(NOT TARGET copy_dll_${target_name})
        add_custom_target(copy_dll_${target_name})
        set_target_properties(copy_dll_${target_name} PROPERTIES FOLDER "utils")
    endif()
    add_library(${target_name} ${type} IMPORTED GLOBAL)

    if(WIN32 AND ${type} STREQUAL "SHARED")
        set(lib "${src_root}/lib/Windows/${dll_name}.lib" )
        set(dll "${src_root}/bin/Windows/${dll_name}.dll")
        message(STATUS "copy ${dll} to ${real_out_put_dir}")
        add_custom_command(
            TARGET copy_dll_${target_name}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${src_root}/bin/Windows"
            ${real_out_put_dir}
        )

        set_target_properties(${target_name} PROPERTIES 
        IMPORTED_IMPLIB  ${lib}
        IMPORTED_LOCATION  ${dll})

        add_dependencies(${target_name} copy_dll_${target_name})
    elseif(WIN32 AND ${type} STREQUAL "STATIC")
        set(lib "${src_root}/lib/Windows/${dll_name}.lib" )
        set_target_properties(${target_name} PROPERTIES 
        IMPORTED_IMPLIB  ${lib}
        IMPORTED_LOCATION  ${lib})
    endif()
endfunction()

function(add_subdirectory_silent dir)
    # 1. 静默 CMake 自身的开发者警告和 deprecated 警告
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 1 CACHE INTERNAL "")
    set(CMAKE_WARN_DEPRECATED OFF CACHE INTERNAL "")

    # 2. 静默编译器警告：临时为子目录添加 -w（Clang/GCC）或 /w（MSVC）
    set(_saved_c_flags "${CMAKE_C_FLAGS}")
    set(_saved_cxx_flags "${CMAKE_CXX_FLAGS}")
    if(MSVC)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /w")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /w")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")
    endif()

    # 3. 添加第三方库（SYSTEM 标记 include 路径，CMake 3.25+）
    add_subdirectory(${dir} SYSTEM)

    # 4. 恢复编译器警告设置
    set(CMAKE_C_FLAGS "${_saved_c_flags}")
    set(CMAKE_CXX_FLAGS "${_saved_cxx_flags}")

    # 5. 恢复 CMake 警告设置
    set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS 0 CACHE INTERNAL "")
    set(CMAKE_WARN_DEPRECATED ON CACHE INTERNAL "")
endfunction()