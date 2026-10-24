cmake_minimum_required(VERSION 3.20)
project(DescriptorAllocatorAB LANGUAGES C CXX)
find_package(Threads REQUIRED)
get_filename_component(MOER_ROOT "../.." ABSOLUTE)
add_executable(DescriptorAllocatorAB main.cpp legacy.cpp
    "${MOER_ROOT}/source/runtime/render/rhi/vulkan/VulkanDescriptorBufferAllocator.cpp")
target_compile_features(DescriptorAllocatorAB PRIVATE cxx_std_20)
target_include_directories(DescriptorAllocatorAB PRIVATE
    "${MOER_ROOT}/source/runtime/render/rhi/vulkan")
target_link_libraries(DescriptorAllocatorAB PRIVATE Threads::Threads)
# Keep both implementations in separate translation units, without LTO.
set_property(TARGET DescriptorAllocatorAB PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)

add_executable(DescriptorAllocatorFailureProbe failure.cpp
    "${MOER_ROOT}/source/runtime/render/rhi/vulkan/VulkanDescriptorBufferAllocator.cpp")
target_compile_features(DescriptorAllocatorFailureProbe PRIVATE cxx_std_20)
target_include_directories(DescriptorAllocatorFailureProbe PRIVATE
    "${MOER_ROOT}/source/runtime/render/rhi/vulkan")
target_link_libraries(DescriptorAllocatorFailureProbe PRIVATE Threads::Threads)

# Standalone, CPU-only experiment. Use the engine's Vulkan include entry point,
# with the same bundled headers/defines, without linking or initializing a GPU.
add_library(descriptor_vulkan_headers INTERFACE)
add_library(Moer::VulkanHeaders ALIAS descriptor_vulkan_headers)
set(VULKAN_HEADERS_INSTALL_DIR "${MOER_ROOT}/3rdparty/VulkanSDK")
set(VOLK_STATIC_DEFINES VK_NO_PROTOTYPES)
if(WIN32)
    list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_WIN32_KHR)
elseif(UNIX AND NOT APPLE)
    list(APPEND VOLK_STATIC_DEFINES VK_USE_PLATFORM_XLIB_KHR)
endif()
add_subdirectory("${MOER_ROOT}/3rdparty/volk" "${CMAKE_CURRENT_BINARY_DIR}/volk")
target_link_libraries(descriptor_vulkan_headers INTERFACE volk)
target_include_directories(descriptor_vulkan_headers INTERFACE
    "${MOER_ROOT}/source/runtime/render/rhi/vulkan"
    "${MOER_ROOT}/3rdparty/VulkanMemoryAllocator")
target_compile_definitions(descriptor_vulkan_headers INTERFACE VK_NO_PROTOTYPES)
if(WIN32)
    target_compile_definitions(descriptor_vulkan_headers INTERFACE VK_USE_PLATFORM_WIN32_KHR NOMINMAX)
elseif(UNIX AND NOT APPLE)
    target_compile_definitions(descriptor_vulkan_headers INTERFACE VK_USE_PLATFORM_XLIB_KHR)
endif()

add_library(DescriptorVmaCandidate STATIC vma_candidate.cpp)
target_compile_features(DescriptorVmaCandidate PUBLIC cxx_std_20)
target_link_libraries(DescriptorVmaCandidate PUBLIC Moer::VulkanHeaders Threads::Threads)
set_property(TARGET DescriptorVmaCandidate PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(DescriptorAllocatorVmaExperiment vma_experiment.cpp
    "${MOER_ROOT}/source/runtime/render/rhi/vulkan/VulkanDescriptorBufferAllocator.cpp")
target_link_libraries(DescriptorAllocatorVmaExperiment PRIVATE DescriptorVmaCandidate)
set_property(TARGET DescriptorAllocatorVmaExperiment PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
add_executable(DescriptorAllocatorVmaFailureProbe failure.cpp)
target_compile_definitions(DescriptorAllocatorVmaFailureProbe PRIVATE DESCRIPTOR_VMA_CANDIDATE)
target_link_libraries(DescriptorAllocatorVmaFailureProbe PRIVATE DescriptorVmaCandidate)
enable_testing()
add_test(NAME DescriptorAllocatorVmaContracts COMMAND DescriptorAllocatorVmaExperiment --test)
# MSVC Debug's noexcept vector default constructor allocates an iterator proxy.
# Blanket OOM injection terminates there instead of throwing to the caller.
# Keep the recovery probes scoped to the configurations they actually validate;
# Debug still runs the full allocation/lifetime/atom-isolation contracts.
set(descriptor_oom_configurations)
if(MSVC)
    set(descriptor_oom_configurations CONFIGURATIONS Release RelWithDebInfo MinSizeRel)
endif()
add_test(NAME DescriptorAllocatorCurrentFailure COMMAND DescriptorAllocatorFailureProbe ${descriptor_oom_configurations})
add_test(NAME DescriptorAllocatorVmaWrapperFailure COMMAND DescriptorAllocatorVmaFailureProbe ${descriptor_oom_configurations})
set_tests_properties(DescriptorAllocatorVmaContracts DescriptorAllocatorCurrentFailure
    DescriptorAllocatorVmaWrapperFailure PROPERTIES TIMEOUT 30)
