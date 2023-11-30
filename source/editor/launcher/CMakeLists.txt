set(target_name MoerEditor)

file(GLOB EDITOR_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${EDITOR_HEADERS} ${EDITOR_SOURCES})

add_executable(${target_name} ${EDITOR_HEADERS} ${EDITOR_SOURCES})

add_compile_definitions("moer_root_dir=${BINARY_ROOT_DIR}")

set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerEditor")
set_target_properties(${target_name} PROPERTIES FOLDER "engine")
target_include_directories(
  ${target_name}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../editorui/include>
  PRIVATE $<BUILD_INTERFACE:${moer_source_dir}/runtime>
  PRIVATE $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
)

message("[Engine] runtime dir: ${moer_source_dir}/runtime")

target_compile_options(${target_name} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX->")
target_link_libraries(${target_name}
    PRIVATE MoerEngineRuntime
    PRIVATE MoerEditorUI
    )

if( CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
else() 
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") 
endif()

# copy resource
add_custom_command( 
    TARGET ${target_name} 
    POST_BUILD 
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resource" "${output_dir}/resource"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_source_dir}/${moer_config_dir}" "${output_dir}/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_root_dir}/shaders" "${output_dir}/resource/shaders"
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resource ${moer_source_dir}/${moer_config_dir}
)