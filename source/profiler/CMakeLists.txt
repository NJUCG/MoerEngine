set(target_name MoerProfiler)

file(GLOB_RECURSE PROFILER_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE PROFILER_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${PROFILER_HEADERS} ${PROFILER_SOURCES})

add_executable(${target_name} ${PROFILER_SOURCES})

set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerProfiler")
set_target_properties(${target_name} PROPERTIES FOLDER "profiler")

target_link_libraries(${target_name}
    PRIVATE Moer::Runtime
)

target_include_directories(
  ${target_name}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

