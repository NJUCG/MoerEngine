set(moer_test_folder "test")

add_subdirectory(math)

add_executable(RuntimeTest EngineTest.cpp)

target_include_directories(RuntimeTest
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)

target_link_libraries(RuntimeTest PUBLIC MoerRuntime)
set_target_properties(RuntimeTest PROPERTIES FOLDER ${moer_test_folder})

add_executable(ShaderReflectionTest ShaderReflectionTest.cpp)
target_include_directories(ShaderReflectionTest
	PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/boost>

    PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime>
	PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
	PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(ShaderReflectionTest PRIVATE render)
set_target_properties(ShaderReflectionTest PROPERTIES FOLDER ${moer_test_folder})

# add_test(NAME ShaderReflectionTest_ COMMAND ShaderReflectionTest)