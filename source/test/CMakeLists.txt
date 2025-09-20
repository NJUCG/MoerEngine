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

# add_executable(ShaderReflectionTest ShaderReflectionTest.cpp)
# target_include_directories(ShaderReflectionTest
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime>
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
# )
# target_link_libraries(ShaderReflectionTest 
#     PRIVATE MoerRender
#     PRIVATE dxc
#     )
# set_target_properties(ShaderReflectionTest PROPERTIES FOLDER ${moer_test_folder})

# add_executable(VulkanRHITest VulkanRHITest.cpp)
# target_include_directories(VulkanRHITest
#     PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/glfw/include>
#     PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
# )
# target_link_libraries(VulkanRHITest
#     PRIVATE MoerRender
#     PRIVATE glfw
# )
# set_target_properties(VulkanRHITest PROPERTIES FOLDER ${moer_test_folder})


add_executable(TaskGraphTest "TaskGraphTest.cpp")
target_include_directories(TaskGraphTest
PUBLIC $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
)
target_link_libraries(TaskGraphTest
    # PUBLIC mimalloc
    PUBLIC MoerCore
)
set_target_properties(TaskGraphTest PROPERTIES FOLDER ${moer_test_folder})

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
#     PRIVATE MoerRender
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
#     PRIVATE MoerRender
#     PRIVATE glfw
#     PRIVATE MoerResource
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
#     PRIVATE MoerRender
#     PRIVATE glfw
#     PRIVATE MoerResource

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
#     PRIVATE MoerRender
#     PRIVATE glfw
#     PRIVATE MoerResource

# )
# set_target_properties(NRDTest PROPERTIES FOLDER ${moer_test_folder})

# VertexFactoryTest
add_executable(VertexFactoryTest VertexFactoryTest.cpp)
target_include_directories(VertexFactoryTest
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
)
target_link_libraries(VertexFactoryTest
    PRIVATE MoerRender
    PRIVATE glfw
    PRIVATE MoerResource
)
set_target_properties(VertexFactoryTest PROPERTIES FOLDER ${moer_test_folder})

# DX RHI Test
add_executable(dxrhitest RHITest/dxrhitest.cpp)
target_include_directories(dxrhitest
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
    PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/dxrhitest>
)
target_link_libraries(dxrhitest
    PRIVATE MoerRender
    PRIVATE glfw
    PRIVATE MoerResource
)
set_target_properties(dxrhitest PROPERTIES FOLDER ${moer_test_folder})

# CUDA Test
if (TARGET moer_cuda)
    add_executable(cuda_test CudaTest.cpp)
    target_include_directories(cuda_test
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/core/include>
        PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime/render/include>
    )
    target_link_libraries(cuda_test
        PRIVATE MoerRender
        PRIVATE glfw
        PRIVATE MoerResource
    )
    target_link_libraries(cuda_test
        PRIVATE moer_cuda # Link CUDA and add includes
    )
    set_target_properties(cuda_test PROPERTIES FOLDER ${moer_test_folder})
endif()