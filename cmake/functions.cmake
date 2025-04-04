function(set_target_folder target_name folder_name)
    if(TARGET ${target_name})
        set_target_properties(${target_name} PROPERTIES FOLDER ${folder_name})
    else()
        message(STATUS "${target_name} does not exist.")
    endif()
endfunction()

if(CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
    set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
else() 
    set(real_out_put_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") 
endif()

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
    elseif(LINUX)
    endif()
endfunction()