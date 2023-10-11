set(target_name math_test)

add_executable(${target_name} main.cpp)
target_link_libraries(${target_name} PUBLIC MoerRuntimeCore)

set_target_properties(${target_name} PROPERTIES FOLDER ${moer_test_folder})