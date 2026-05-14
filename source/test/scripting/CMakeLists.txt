set(moer_scripting_test_folder "${moer_test_folder}/scripting")

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

add_custom_command(TARGET ${test_target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${test_target}>/test/scripting"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/scene_control_smoke.py"
        "$<TARGET_FILE_DIR:${test_target}>/test/scripting/scene_control_smoke.py"
)
