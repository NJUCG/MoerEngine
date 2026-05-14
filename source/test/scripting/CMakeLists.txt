set(moer_scripting_test_folder "${moer_test_folder}/scripting")

function(moer_add_test_script_copy_target test_target script_name)
    set(copy_target "${test_target}_copy_${script_name}")
    add_custom_target(${copy_target}
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${test_target}>/test/scripting"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/${script_name}"
            "$<TARGET_FILE_DIR:${test_target}>/test/scripting/${script_name}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${script_name}"
    )
    add_dependencies(${test_target} ${copy_target})
    set_target_properties(${copy_target} PROPERTIES FOLDER ${moer_scripting_test_folder})
endfunction()

set(test_target TestScriptingSceneControl)
add_executable(${test_target} "TestScriptingSceneControl.cpp")
target_include_directories(${test_target}
    PRIVATE
        $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(${test_target}
    PRIVATE Moer::Runtime
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_scripting_test_folder})
moer_add_test_script_copy_target(${test_target} "scene_control_smoke.py")

set(test_target TestScriptingEditorMizukiMotion)
add_executable(${test_target} "TestScriptingEditorMizukiMotion.cpp")
target_include_directories(${test_target}
    PRIVATE
        $<BUILD_INTERFACE:${moer_source_dir}/editor>
        $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(${test_target}
    PRIVATE Moer::EditorApp
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_scripting_test_folder})
moer_add_test_script_copy_target(${test_target} "mizuki_motion_manual.py")

set(test_target TestScripting260514All)
add_executable(${test_target} "TestScripting260514All.cpp")
target_include_directories(${test_target}
    PRIVATE
        $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(${test_target}
    PRIVATE Moer::Runtime
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_scripting_test_folder})
moer_add_test_script_copy_target(${test_target} "scene_api_all_260514.py")
