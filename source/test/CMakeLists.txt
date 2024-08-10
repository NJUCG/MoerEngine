set(moer_test_folder "test")

add_subdirectory(math)

add_executable(RuntimeTest EngineTest.cpp)

target_include_directories(RuntimeTest
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)

target_link_libraries(RuntimeTest PUBLIC MoerEngineRuntime)
set_target_properties(RuntimeTest PROPERTIES FOLDER ${moer_test_folder})

add_executable(ShaderReflectionTest ShaderReflectionTest.cpp)
target_include_directories(ShaderReflectionTest
        PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(ShaderReflectionTest 
    PRIVATE MoerRender
    PRIVATE dxc
    )
set_target_properties(ShaderReflectionTest PROPERTIES FOLDER ${moer_test_folder})

add_executable(VulkanRHITest VulkanRHITest.cpp)
target_include_directories(VulkanRHITest
        PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
        PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(VulkanRHITest
        PRIVATE MoerRender
        PRIVATE glfw
)

set_target_properties(VulkanRHITest PROPERTIES FOLDER ${moer_test_folder})


add_executable(TaskGraphTest "TaskGraphTest.cpp")
target_include_directories(TaskGraphTest
PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
)
target_link_libraries(TaskGraphTest
        # PUBLIC mimalloc
        PUBLIC MoerRuntimeCore
)
set_target_properties(TaskGraphTest PROPERTIES FOLDER ${moer_test_folder})

add_executable(MeshProcessTest "MeshProcessTest.cpp")

target_include_directories(MeshProcessTest 
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
        PRIVATE $<BUILD_INTERFACE:${moer_third_party_dir}/AssetLoad/assimp/include>)
target_link_libraries(MeshProcessTest PRIVATE render MeshProcess AssetLoad)
set_target_properties(MeshProcessTest PROPERTIES FOLDER ${moer_test_folder})

add_executable(VulkanRTRHITest VulkanRTRHITest.cpp)
target_include_directories(VulkanRTRHITest
        PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
        PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(VulkanRTRHITest
        PRIVATE MoerRender
        PRIVATE glfw
)
set_target_properties(VulkanRTRHITest PROPERTIES FOLDER ${moer_test_folder})

add_executable(RHITest RHITest.cpp)
target_include_directories(RHITest
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(RHITest
        PRIVATE MoerRender
        PRIVATE glfw
)
set_target_properties(RHITest PROPERTIES FOLDER ${moer_test_folder})