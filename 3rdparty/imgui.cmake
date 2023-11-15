set(imgui_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
file(GLOB imgui_sources CONFIGURE_DEPENDS "${imgui_source_dir}/*.cpp")
file(GLOB imgui_headers CONFIGURE_DEPENDS "${imgui_source_dir}/*.h")
file (GLOB PLATFORM_SRC ${imgui_source_dir}/backends/imgui_impl_glfw.*)

add_library(imgui STATIC ${imgui_headers} ${imgui_sources} ${PLATFORM_SRC})
target_include_directories(imgui PUBLIC $<BUILD_INTERFACE:${imgui_source_dir}>)
target_link_libraries(imgui PUBLIC glfw)