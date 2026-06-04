set(moer_remote_test_folder "${moer_test_folder}/remote")

if (NOT TARGET hv)
    message(FATAL_ERROR "TestRemoteHttpSmoke requires hv from 3rdparty/libhv.cmake")
endif()

set(test_target TestRemoteHttpSmoke)
add_executable(${test_target} "TestRemoteHttpSmoke.cpp")
target_link_libraries(${test_target}
    PRIVATE hv
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_remote_test_folder})

set(test_target TestRemoteWebSocketSmoke)
add_executable(${test_target} "TestRemoteWebSocketSmoke.cpp")
target_link_libraries(${test_target}
    PRIVATE hv
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_remote_test_folder})

set(test_target TestRemoteDisabledByConfig)
add_executable(${test_target} "TestRemoteDisabledByConfig.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::remote
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_remote_test_folder})

set(test_target TestRemoteHttpExecuteSmoke)
add_executable(${test_target} "TestRemoteHttpExecuteSmoke.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::remote
    PRIVATE Moer::scripting
    PRIVATE hv
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_remote_test_folder})

set(test_target TestRemoteWebSocketEventSmoke)
add_executable(${test_target} "TestRemoteWebSocketEventSmoke.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::remote
    PRIVATE Moer::scripting
    PRIVATE hv
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_remote_test_folder})

set(test_target TestRemoteEngineLoopbackDemo)
add_executable(${test_target} "TestRemoteEngineLoopbackDemo.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Runtime
    PRIVATE hv
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_remote_test_folder})
add_custom_target(${test_target}_copy_remote_engine_loopback_demo_py
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${test_target}>/test"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/remote_engine_loopback_demo.py"
        "$<TARGET_FILE_DIR:${test_target}>/test/remote_engine_loopback_demo.py"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/remote_engine_loopback_demo.py"
)
add_dependencies(${test_target} ${test_target}_copy_remote_engine_loopback_demo_py)
set_target_properties(${test_target}_copy_remote_engine_loopback_demo_py PROPERTIES FOLDER ${moer_remote_test_folder})