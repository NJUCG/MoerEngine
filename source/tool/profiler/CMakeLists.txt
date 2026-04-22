set(target_name MoerProfiler)

set(profiler_source_dir ${moer_source_dir}/profiler)

file(GLOB_RECURSE PROFILER_HEADERS CONFIGURE_DEPENDS ${profiler_source_dir}/*.h)
file(GLOB_RECURSE PROFILER_SOURCES CONFIGURE_DEPENDS ${profiler_source_dir}/*.cpp)

source_group(TREE "${profiler_source_dir}" FILES ${PROFILER_HEADERS} ${PROFILER_SOURCES})

add_executable(${target_name} ${PROFILER_SOURCES} ${PROFILER_HEADERS})

set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerProfiler")
set_target_properties(${target_name} PROPERTIES FOLDER "tool/profiler")

target_link_libraries(${target_name}
  PRIVATE Moer::Core
  PRIVATE Moer::Render
)

target_include_directories(
  ${target_name}
  PRIVATE $<BUILD_INTERFACE:${profiler_source_dir}>
)
