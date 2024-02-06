
set(third_lib_for_asset_load_target AssetLoad)

add_library(${third_lib_for_asset_load_target} SHARED dummy.cpp)
set_target_properties(${third_lib_for_asset_load_target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)

#########################################################
# common function
function(set_ide_folder target_name folder_name)
    if(TARGET ${target_name})
        set_target_properties(${target_name} PROPERTIES FOLDER ${folder_name})
    else()
        message(STATUS "${target_name} does not exist.")
    endif()
endfunction()

function(add_sub_target lib_name target_name folder_name include_dir)
    if(NOT TARGET ${target_name})
        add_subdirectory(${lib_name})

        target_link_libraries(${third_lib_for_asset_load_target} PUBLIC ${target_name})

        if(NOT(include_dir EQUAL ""))
            target_include_directories(${target_name} PUBLIC ${include_dir})
        endif()

        set_ide_folder(${target_name} ${folder_name})
    endif()
endfunction()
#########################################################
set_ide_folder(${third_lib_for_asset_load_target} ${third_party_folder})

# add_custom_target(file-copy COMMENT "Copy necessary files for third-party libraries")
# set_ide_folder(file-copy ${third_party_folder})

set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE INTERNAL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE INTERNAL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE INTERNAL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE INTERNAL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE INTERNAL "" FORCE)
add_sub_target(assimp assimp ${third_party_folder}/assimp "")
target_compile_options(assimp 
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wdeprecated-non-prototype>")
set_ide_folder(UpdateAssimpLibsDebugSymbolsAndDLLs ${third_party_folder}/assimp)
set_ide_folder(zlibstatic ${third_party_folder}/assimp)

target_link_libraries(${third_lib_for_asset_load_target} PUBLIC assimp)
target_include_directories(${third_lib_for_asset_load_target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/assimp)

add_sub_target(stb stb ${third_party_folder} ${CMAKE_CURRENT_SOURCE_DIR}/stb)
target_compile_options(stb 
    PRIVATE "$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-Wunused-function>")

set_property(GLOBAL PROPERTY CTEST_TARGETS_ADDED 1)
set(BUILD_SHARED_LIBS ON CACHE INTERNAL "" FORCE)
add_sub_target(pugixml pugixml-shared ${third_party_folder} "")

add_sub_target(exr tinyexr ${third_party_folder}/tinyexr "")
set_ide_folder(miniz ${third_party_folder}/tinyexr)