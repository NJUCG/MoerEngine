cmake_minimum_required(VERSION 3.20)
project(DescriptorReservationBenchmark LANGUAGES CXX)
find_package(Threads REQUIRED)
add_executable(DescriptorReservationBenchmark benchmark.cpp
    ../../source/runtime/render/rhi/vulkan/VulkanDescriptorBufferAllocator.cpp)
target_compile_features(DescriptorReservationBenchmark PRIVATE cxx_std_20)
target_include_directories(DescriptorReservationBenchmark PRIVATE
    ../../source/runtime/render/rhi/vulkan)
target_link_libraries(DescriptorReservationBenchmark PRIVATE Threads::Threads)
set_property(TARGET DescriptorReservationBenchmark PROPERTY INTERPROCEDURAL_OPTIMIZATION FALSE)
