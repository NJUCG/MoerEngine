set(moer_test_folder "test")

if (WIN32)
    add_subdirectory(math)
endif()

# add_executable(RuntimeTest EngineTest.cpp)
# target_include_directories(RuntimeTest
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
# )
# target_link_libraries(RuntimeTest PUBLIC Moer::Runtime)
# set_target_properties(RuntimeTest PROPERTIES FOLDER ${moer_test_folder})

# add_executable(ShaderReflectionTest ShaderReflectionTest.cpp)
# target_include_directories(ShaderReflectionTest
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime>
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
# )
# target_link_libraries(ShaderReflectionTest 
#     PRIVATE Moer::Render
#     PRIVATE dxc
#     )
# set_target_properties(ShaderReflectionTest PROPERTIES FOLDER ${moer_test_folder})

# add_executable(VulkanRHITest VulkanRHITest.cpp)
# target_include_directories(VulkanRHITest
#     PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
# )
# target_link_libraries(VulkanRHITest
#     PRIVATE Moer::Render
#     PRIVATE glfw
# )
# set_target_properties(VulkanRHITest PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestTaskGraph)
add_executable(${test_target} "TaskGraphTest.cpp")
target_include_directories(${test_target}
PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
)
target_link_libraries(${test_target}
    # PUBLIC mimalloc
    PUBLIC Moer::Core
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(pipe_test_target TestTaskPipe)
add_executable(${pipe_test_target} "TaskPipeTest.cpp")
target_include_directories(${pipe_test_target}
PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
)
target_link_libraries(${pipe_test_target}
    PUBLIC Moer::Core
)
set_target_properties(${pipe_test_target} PROPERTIES FOLDER ${moer_test_folder})

set(string_test_target TestStringSystem)
add_executable(${string_test_target} "StringSystemTest.cpp")
target_include_directories(${string_test_target}
PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
)
target_link_libraries(${string_test_target}
    PUBLIC Moer::Core
)
set_target_properties(${string_test_target} PROPERTIES FOLDER ${moer_test_folder})

if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_custom_target(TestCoreLinux
        DEPENDS
            TestTaskGraph
            TestTaskPipe
            TestStringSystem
    )
    set_target_properties(TestCoreLinux PROPERTIES FOLDER ${moer_test_folder})
    return()
endif()

# add_executable(MeshProcessTest "MeshProcessTest.cpp")
# target_include_directories(MeshProcessTest 
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
#     PRIVATE $<BUILD_INTERFACE:${moer_third_party_dir}/AssetLoad/assimp/include>)
# target_link_libraries(MeshProcessTest PRIVATE render MeshProcess AssetLoad)
# set_target_properties(MeshProcessTest PROPERTIES FOLDER ${moer_test_folder})

# add_executable(VulkanRTRHITest VulkanRTRHITest.cpp)
# target_include_directories(VulkanRTRHITest
#     PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
# )
# target_link_libraries(VulkanRTRHITest
#     PRIVATE Moer::Render
#     PRIVATE glfw
# )
# set_target_properties(VulkanRTRHITest PROPERTIES FOLDER ${moer_test_folder})

# Raster
# file(GLOB_RECURSE RASTER_TEST_CPPS Raster/*.cpp)
# add_executable(RHITest ${RASTER_TEST_CPPS})
# target_include_directories(RHITest
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
#     PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Raster>
# )
# target_link_libraries(RHITest
#     PRIVATE Moer::Render
#     PRIVATE glfw
#     PRIVATE Moer::Resource
# )
# set_target_properties(RHITest PROPERTIES FOLDER ${moer_test_folder})

# # RaytracingTest
# file(GLOB_RECURSE RT_TEST_CPPS Raytracing/*.cpp)
# add_executable(RaytracingTest ${RT_TEST_CPPS})
# target_include_directories(RaytracingTest
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
#     PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Raytracing>
# )
# target_link_libraries(RaytracingTest
#     PRIVATE Moer::Render
#     PRIVATE glfw
#     PRIVATE Moer::Resource

# )
# set_target_properties(RaytracingTest PROPERTIES FOLDER ${moer_test_folder})

# nrd
# file(GLOB_RECURSE RT_NRD_TEST_CPPS RTNRD/*.cpp)
# add_executable(NRDTest ${RT_NRD_TEST_CPPS})
# target_include_directories(NRDTest
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
#     PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
#     PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Raytracing>
#     PRIVATE $<BUILD_INTERFACE:${moer_shader_dir}/nrd>
# )
# target_link_libraries(NRDTest
#     PRIVATE Moer::Render
#     PRIVATE glfw
#     PRIVATE Moer::Resource

# )
# set_target_properties(NRDTest PROPERTIES FOLDER ${moer_test_folder})

# DX RHI Test
set(test_target TestDxRhi)
add_executable(${test_target} RHITest/dxrhitest.cpp)
target_include_directories(${test_target}
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dxrhitest>
)
target_link_libraries(${test_target}
    PRIVATE Moer::Render
    PRIVATE glfw
)
if (TARGET Moer::Profile)
    target_link_libraries(${test_target} PRIVATE Moer::Profile)
    target_compile_definitions(${test_target} PRIVATE MOER_TEST_WITH_PROFILE=1)
endif()
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

# RHI Translate MultiQueue Test (Vulkan-only runtime path)
set(test_target TestRHITranslate)
add_executable(${test_target}
    RHITest/rhi_translate_multiqueue_test.cpp
    RHITest/rhi_commandlist_rg_baseline_test.cpp
    RHITest/rhi_rendergraph_contract_test.cpp
    ${moer_source_dir}/profiler/ProfileSession.cpp
)
target_include_directories(${test_target}
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/profiler>
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/RHITest>
)
target_link_libraries(${test_target}
    PRIVATE Moer::Render
    PRIVATE glfw
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestShaderDXC)
add_executable(${test_target} ShaderDXCTest.cpp)
target_include_directories(${test_target}
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

# CUDA Test
if (TARGET Moer::Cuda)
    set(test_target TestCuda)
    add_executable(${test_target} CudaTest.cpp)
    target_include_directories(${test_target}
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
    )
    target_link_libraries(${test_target}
        PRIVATE Moer::Render
        PRIVATE glfw
    )
    target_link_libraries(${test_target}
        PRIVATE Moer::Cuda # Link CUDA and add includes
    )
    set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
endif()

if (TARGET LibTorch)
    set(test_target TestTorch)
    add_executable(${test_target} TorchTest.cpp)
    target_include_directories(${test_target}
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
    )
    target_link_libraries(${test_target}
        PRIVATE Moer::Render
        PRIVATE glfw
    )
    target_link_libraries(${test_target}
        PRIVATE LibTorch
    )
    set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
endif()

if (TARGET TensorRT)
    set(test_target TestTensorRT)
    add_executable(${test_target} TensorRTTest.cpp)
    target_include_directories(${test_target}
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
    )
    target_link_libraries(${test_target}
        PRIVATE Moer::Render
        PRIVATE glfw
    )
    target_link_libraries(${test_target}
        PRIVATE TensorRT
    )
    set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
endif()

if (TARGET EnTT::EnTT)
    set(test_target TestEnTT)
    add_executable(${test_target} EnTTTest.cpp)
    target_link_libraries(${test_target}
        PRIVATE EnTT::EnTT
    )
    set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
endif()

if (TARGET gtl)
    set(test_target TestGtl)
    add_executable(${test_target} TestGtl.cpp)
    target_link_libraries(${test_target}
        PRIVATE gtl
    )
    set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
endif()
