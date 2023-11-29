set(imgui_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/imgui)
file(GLOB imgui_sources CONFIGURE_DEPENDS "${imgui_source_dir}/*.cpp")
file(GLOB imgui_headers CONFIGURE_DEPENDS "${imgui_source_dir}/*.h")

add_library(imgui SHARED ${imgui_headers} ${imgui_sources})
target_include_directories(imgui PUBLIC 
$<BUILD_INTERFACE:${imgui_source_dir}>
)

add_library(ImGui::imgui ALIAS imgui)

if(WIN32)
    set_property(TARGET imgui PROPERTY WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()