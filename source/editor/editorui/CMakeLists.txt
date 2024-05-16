set(target_name MoerEditorUI)

file(GLOB UI_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${UI_HEADERS} ${UI_SOURCES})

add_library(${target_name} STATIC ${UI_HEADERS} ${UI_SOURCES})

set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerEditorUI")
set_target_properties(${target_name} PROPERTIES FOLDER "editor")
target_include_directories(
  ${target_name}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
  # PRIVATE $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
)

target_compile_options(${target_name} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX->")
target_link_libraries(${target_name} MoerEngineRuntime)