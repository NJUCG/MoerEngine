set(imgui_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
file(GLOB imgui_sources CONFIGURE_DEPENDS "${imgui_source_dir}/*.cpp")
file(GLOB imgui_headers CONFIGURE_DEPENDS "${imgui_source_dir}/*.h")

add_library(imgui SHARED ${imgui_headers} ${imgui_sources})
target_include_directories(imgui PUBLIC 
$<BUILD_INTERFACE:${imgui_source_dir}>
)

add_library(ImGui::imgui ALIAS imgui)
target_compile_options(imgui
  PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:$<$<CONFIG:Debug>:/MTd>>
    $<$<CXX_COMPILER_ID:MSVC>:$<$<CONFIG:MinSizeRel>:/MT>>
    $<$<CXX_COMPILER_ID:MSVC>:$<$<CONFIG:Release>:/MT>>
    $<$<CXX_COMPILER_ID:MSVC>:$<$<CONFIG:RelWithDebInfo>:/MTd>>
)

if(WIN32)
    set_property(TARGET imgui PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

add_executable(binary_to_compressed ${imgui_source_dir}/misc/fonts/binary_to_compressed_c.cpp)
#release o3
target_compile_options(binary_to_compressed PUBLIC -O3 -Wall)

add_dependencies(imgui binary_to_compressed)