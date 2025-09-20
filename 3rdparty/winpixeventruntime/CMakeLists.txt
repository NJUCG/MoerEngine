project(winpixeventruntime)
 

set(MOER_WINPIX_VERSION_ID "1.0.240308001" CACHE INTERNAL "")


set(MOER_WINPIX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/winpixeventruntime.${MOER_WINPIX_VERSION_ID}" CACHE INTERNAL "")
message(STATUS "use winpixeventruntime sdk from ${MOER_WINPIX_DIR}") 
set(MOER_WINPIX_DLL_DIR "${MOER_WINPIX_DIR}/bin/x64" CACHE INTERNAL "")

function(deploy_winpix target_name )
    if(NOT TARGET copy_dll_${target_name})
        add_custom_target(copy_dll_${target_name})
        set_target_properties(copy_dll_${target_name} PROPERTIES FOLDER "utils")
    endif()
    add_library(${target_name} SHARED IMPORTED GLOBAL)
    target_include_directories(${target_name} INTERFACE "${MOER_WINPIX_DIR}/Include")
    
    set(lib "${MOER_WINPIX_DIR}/bin/x64/WinPixEventRuntime.lib")
    set(dll "${MOER_WINPIX_DIR}/bin/x64/WinPixEventRuntime.dll")
    message(STATUS "copy ${dll} to ${real_out_put_dir}")
    add_custom_command(
        TARGET copy_dll_${target_name}
        POST_BUILD
        COMMAND 
            ${CMAKE_COMMAND} -E copy_if_different
            "${dll}"
            "${real_out_put_dir}"
    )

    set_target_properties(${target_name} PROPERTIES 
    IMPORTED_IMPLIB  ${lib}
    IMPORTED_LOCATION  ${dll})

    add_dependencies(${target_name} copy_dll_${target_name})

endfunction()

deploy_winpix(winpixeventruntime)