set(moer_test_folder "test")

add_subdirectory(math)
add_subdirectory(remote)
add_subdirectory(scripting)

set(test_target TestEngineOnly)
add_executable(${test_target} "TestEngineOnly.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Runtime
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestShaderStageSupport)
add_executable(${test_target} "ShaderStageSupportTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestRenderGraph)
add_executable(${test_target} "RenderGraphTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RenderGraphContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHICmdReorderer)
add_executable(${test_target} "RHICmdReordererTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHICmdReordererContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHICommandMarker)
add_executable(${test_target} "RHICommandMarkerTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHICommandMarkerContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestExternalCpuJoin)
add_executable(${test_target} "ExternalCpuJoinTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME ExternalCpuJoinContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHIRecordDiagnostics)
add_executable(${test_target} "RHIRecordDiagnosticsTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIRecordDiagnosticsContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHIParallelRecord)
add_executable(${test_target} "RHIParallelRecordTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIParallelRecordContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHISubmissionTopology)
add_executable(${test_target} "RHISubmissionTopologyTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHISubmissionTopologyContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHIExecutorRecordingHandoff)
add_executable(${test_target} "RHIExecutorRecordingHandoffTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIExecutorRecordingHandoffContract COMMAND $<TARGET_FILE:${test_target}>)

# CPU-only contract for the Vulkan recorder work heuristic. This uses RHI
# command payloads but does not create a Vulkan instance or device.
set(test_target TestVulkanParallelRecordWorkEstimator)
add_executable(${test_target} "VulkanParallelRecordWorkEstimatorTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME VulkanParallelRecordWorkEstimatorContract COMMAND $<TARGET_FILE:${test_target}>)

# Vulkan integration seam for ordered multi-primary submission and the
# recoverable worker-failure path. It is intentionally run explicitly because
# it requires a Vulkan-capable machine; the CPU-only contracts above remain in
# the default CTest suite.
set(test_target TestRHIParallelRecordVulkan)
add_executable(${test_target} "RHIParallelRecordVulkanTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestProbePhysicalAllocator)
add_executable(${test_target} "ProbePhysicalAllocatorTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestProbeAdaptiveLayout)
add_executable(${test_target} "ProbeAdaptiveLayoutTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestProbeGeometryClassifier)
add_executable(${test_target} "ProbeGeometryClassifierTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestProbeDirtyTracker)
add_executable(${test_target} "ProbeDirtyTrackerTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestProbeClipmap)
add_executable(${test_target} "ProbeClipmapTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

set(test_target TestProbeStreaming)
add_executable(${test_target} "ProbeStreamingTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})

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

add_executable(MeshProcessTest "MeshProcessTest.cpp")
target_link_libraries(MeshProcessTest
    PRIVATE Moer::Core
    PRIVATE AssetLoad
    PRIVATE meshoptimizer
    PRIVATE metis
)
set_target_properties(MeshProcessTest PROPERTIES FOLDER ${moer_test_folder})

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
