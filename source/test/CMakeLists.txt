set(moer_test_folder "test")

add_subdirectory(math)
add_subdirectory(remote)
add_subdirectory(scripting)

set(test_target TestProfileDumpCore)
add_executable(${test_target} "ProfileDumpCoreTest.cpp")
target_include_directories(${test_target}
    PRIVATE "${moer_source_dir}/runtime/core/source/profile"
)
target_link_libraries(${test_target}
    PRIVATE Moer::Core
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME ProfileDumpCoreContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(ProfileDumpCoreContract PROPERTIES TIMEOUT 60)

set(test_target TestProfileDumpConsumer)
add_executable(${test_target} "ProfileDumpConsumerTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::ProfileConsumer
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME ProfileDumpConsumerContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(ProfileDumpConsumerContract PROPERTIES TIMEOUT 120)

set(test_target TestProfileSummaryCli)
add_executable(${test_target} "ProfileSummaryCliTest.cpp")
target_link_libraries(${test_target}
    PRIVATE
        Moer::ProfileConsumer
        Moer::Core
        nlohmann_json::nlohmann_json
)
add_dependencies(${test_target} MoerProfileSummary)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(
    NAME ProfileSummaryCliContract
    COMMAND $<TARGET_FILE:${test_target}> $<TARGET_FILE:MoerProfileSummary>
)
set_tests_properties(ProfileSummaryCliContract PROPERTIES TIMEOUT 120)

set(test_target TestProfileScopeProducer)
add_executable(${test_target} "ProfileScopeProducerTest.cpp")
target_include_directories(${test_target}
    PRIVATE "${moer_source_dir}/runtime/core/source/profile"
)
target_link_libraries(${test_target}
    PRIVATE Moer::Core
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME ProfileScopeProducerContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(ProfileScopeProducerContract PROPERTIES TIMEOUT 60)

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

set(test_target TestRenderGraphLowering)
add_executable(${test_target} "RenderGraphLoweringTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RenderGraphLoweringContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRenderGraphActiveLowering)
add_executable(${test_target} "RenderGraphActiveLoweringTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RenderGraphActiveLoweringContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRenderGraphResourcePool)
add_executable(${test_target} "RenderGraphResourcePoolTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RenderGraphResourcePoolContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestSceneRenderExtent)
add_executable(${test_target} "SceneRenderExtentTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME SceneRenderExtentContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHICmdReorderer)
add_executable(${test_target} "RHICmdReordererTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHICmdReordererContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHIExplicitBarrier)
add_executable(${test_target} "RHIExplicitBarrierTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIExplicitBarrierContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHICommandMarker)
add_executable(${test_target} "RHICommandMarkerTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHICommandMarkerContract COMMAND $<TARGET_FILE:${test_target}>)

set(test_target TestRHIQuery)
add_executable(${test_target} "RHIQueryContractTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIQueryContract COMMAND $<TARGET_FILE:${test_target}>)

# CPU-only contract for the bounded, Completion-fed GPU scope stream. It
# validates queue-domain hierarchy, ordered frame visibility, capacity
# recovery, rejection ownership, and move/close lifetime without a GPU.
set(test_target TestRHIGpuScope)
add_executable(${test_target} "RHIGpuScopeContractTest.cpp")
target_compile_definitions(moer_render
    PRIVATE MOER_RHI_GPU_SCOPE_TEST_HOOKS=1
)
target_compile_definitions(${test_target}
    PRIVATE MOER_RHI_GPU_SCOPE_TEST_HOOKS=1
)
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIGpuScopeContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(RHIGpuScopeContract PROPERTIES TIMEOUT 60)

set(test_target TestRenderProfileCapture)
add_executable(${test_target} "RenderProfileCaptureTest.cpp")
target_compile_definitions(moer_render
    PRIVATE MOER_RENDER_PROFILE_CAPTURE_TEST_HOOKS=1
)
target_compile_definitions(${test_target}
    PRIVATE MOER_RENDER_PROFILE_CAPTURE_TEST_HOOKS=1
)
target_include_directories(${test_target}
    PRIVATE "${moer_source_dir}/runtime/core/source/profile"
)
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RenderProfileCaptureContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(RenderProfileCaptureContract PROPERTIES TIMEOUT 60)

set(test_target TestProfileCaptureController)
add_executable(${test_target} "ProfileCaptureControllerTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Runtime
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME ProfileCaptureControllerContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(ProfileCaptureControllerContract PROPERTIES TIMEOUT 60)

set(test_target TestProfileCaptureControlModel)
add_executable(
    ${test_target}
    "ProfileCaptureControlModelTest.cpp"
    "${moer_source_dir}/editor/profile_capture_ui/ProfileCaptureControlModel.cpp"
)
target_include_directories(
    ${test_target}
    PRIVATE "${moer_source_dir}/editor"
)
target_link_libraries(${test_target}
    PRIVATE Moer::Runtime
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(
    NAME ProfileCaptureControlModelContract
    COMMAND $<TARGET_FILE:${test_target}>
)
set_tests_properties(ProfileCaptureControlModelContract PROPERTIES TIMEOUT 60)

set(test_target TestRHIGpuCompletion)
add_executable(${test_target} "RHIGpuCompletionContractTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIGpuCompletionContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(RHIGpuCompletionContract PROPERTIES TIMEOUT 60)

set(test_target TestRHIReadback)
add_executable(${test_target} "RHIReadbackContractTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIReadbackContract COMMAND $<TARGET_FILE:${test_target}>)
set_tests_properties(RHIReadbackContract PROPERTIES TIMEOUT 60)

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
set_tests_properties(
    RHIExecutorRecordingHandoffContract
    PROPERTIES TIMEOUT 60
)

# CPU-only ownership contract for the Executor -> Translate -> Submission ->
# Completion pipeline and the move-only queue lease crossing Translate/Submit.
set(test_target TestRHIThreadOwnership)
add_executable(${test_target} "RHIThreadOwnershipTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHIThreadOwnershipContract COMMAND $<TARGET_FILE:${test_target}>)

# CPU-only contract for the bounded cross-batch submission policy. It verifies
# the configured window clamp and the correctness-critical Graphics/Compute
# native-queue alias fallback without requiring alias-capable Vulkan hardware.
set(test_target TestRHISubmissionPipelinePolicy)
add_executable(${test_target} "RHISubmissionPipelinePolicyTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME RHISubmissionPipelinePolicyContract COMMAND $<TARGET_FILE:${test_target}>)

# CPU-only contract for the Vulkan recorder work heuristic. This uses RHI
# command payloads but does not create a Vulkan instance or device.
set(test_target TestVulkanParallelRecordWorkEstimator)
add_executable(${test_target} "VulkanParallelRecordWorkEstimatorTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME VulkanParallelRecordWorkEstimatorContract COMMAND $<TARGET_FILE:${test_target}>)

# CPU-only fault-injection contract for the Vulkan submission worker's
# per-request exception barrier. It verifies Submit rejection ownership and
# unconditional Sync/Stop completion without requiring a Vulkan device.
set(test_target TestVulkanSubmissionRequestDispatch)
add_executable(${test_target} "VulkanSubmissionRequestDispatchTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME VulkanSubmissionRequestDispatchContract COMMAND $<TARGET_FILE:${test_target}>)

# CPU-only dependency-wave contract for same-batch Vulkan translation.
# Explicit topology edges unlock at translated, while native-queue aliases and
# Copy/control boundaries retain packet ownership until released.
set(test_target TestVulkanTranslateWaveScheduler)
add_executable(${test_target} "VulkanTranslateWaveSchedulerTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::Render
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME VulkanTranslateWaveSchedulerContract COMMAND $<TARGET_FILE:${test_target}>)

# Compile-time contract for the engine's Volk integration. The source
# intentionally includes vulkan_core.h before engine Vulkan headers to ensure
# VK_NO_PROTOTYPES propagates through the same lightweight header interface
# used by Moer::Render.
set(test_target TestVulkanHeaderContract)
add_executable(${test_target} "VulkanHeaderContractTest.cpp")
target_link_libraries(${test_target}
    PRIVATE Moer::VulkanHeaders
)
set_target_properties(${test_target} PROPERTIES FOLDER ${moer_test_folder})
add_test(NAME VulkanHeaderContract COMMAND $<TARGET_FILE:${test_target}>)

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
