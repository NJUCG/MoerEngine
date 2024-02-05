set(third_lib_for_mesh_process MeshProcess)

add_library(${third_lib_for_mesh_process} SHARED  dummy.cpp) 

set_target_properties(${third_lib_for_mesh_process} PROPERTIES
WINDOWS_EXPORT_ALL_SYMBOLS ON)

function(add_compiled_lib target_name folder_name status)
    add_library(${target_name} ${status} IMPORTED GLOBAL)

    set(lib_path "")
    #copy dll to output dir
    if(WIN32 AND status STREQUAL "SHARED")
        if(CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
            set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
        else() 
            set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") 
        endif()
        add_custom_target(${target_name}_copy)
        set(lib_path "${CMAKE_CURRENT_SOURCE_DIR}/${folder_name}/lib/Windows/${target_name}.lib" )
        set(dll_runtime_dir "${CMAKE_CURRENT_SOURCE_DIR}/${folder_name}/bin/Windows")

        add_custom_command(
            TARGET ${target_name}_copy
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dll_runtime_dir}/${target_name}.dll"
            ${real_out_put_dir}
        )

        set_target_properties(${target_name} PROPERTIES 
        IMPORTED_IMPLIB  ${lib_path}
        IMPORTED_LOCATION  ${dll_runtime_dir}/${target_name}.dll)
        add_dependencies(${third_lib_for_mesh_process} ${target_name}_copy)
    elseif(WIN32 AND status STREQUAL "STATIC")
        set(lib_path "${CMAKE_CURRENT_SOURCE_DIR}/${folder_name}/lib/Windows/${target_name}.lib" )
        set(lib_dir "${CMAKE_CURRENT_SOURCE_DIR}/${folder_name}/lib/Windows")
        set_target_properties(${target_name} PROPERTIES 
        IMPORTED_IMPLIB  ${lib_path}
        IMPORTED_LOCATION  ${lib_path})
    elseif(LINUX)
    endif()

    target_include_directories(${third_lib_for_mesh_process} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/${folder_name}/include")
    target_link_libraries(${third_lib_for_mesh_process} PUBLIC ${target_name})
    
endfunction()

add_compiled_lib(metis metis SHARED)

add_subdirectory(meshoptimizer)
target_link_libraries(${third_lib_for_mesh_process} PUBLIC meshoptimizer)
target_include_directories(${third_lib_for_mesh_process} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src")

target_compile_definitions(${third_lib_for_mesh_process} PUBLIC "IDXTYPEWIDTH=32" "REALTYPEWIDTH=64")