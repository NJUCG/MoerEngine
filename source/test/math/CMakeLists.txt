set(target_name TestMath)

add_executable(${target_name} main.cpp)
target_link_libraries(${target_name} PUBLIC Moer::Core)

set_target_properties(${target_name} PROPERTIES FOLDER ${moer_test_folder})