set(target_name MoerEditor)

file(GLOB_RECURSE EDITOR_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${EDITOR_HEADERS} ${EDITOR_SOURCES})

add_executable(${target_name} ${EDITOR_SOURCES})

add_compile_definitions("moer_root_dir=${BINARY_ROOT_DIR}")

# Target properties
set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerEditor")
set_target_properties(${target_name} PROPERTIES FOLDER "editor")

# Dependencies : MoerRuntime (including MoerCore, MoerRender, MoerResource, MoerIO)
target_link_libraries(${target_name}
    PRIVATE MoerRuntime
    PRIVATE nfd
)
message("[Engine] runtime dir: ${moer_source_dir}/runtime")

# Include directories
target_include_directories(
  ${target_name}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Compiler options
# I want to remove the following line
target_compile_options(${target_name} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX->")

# Copy resources
# Tip: I have removed `POST_BUILD` to copy resources when building MoerEditor fails

if(CMAKE_GENERATOR MATCHES "Ninja" OR UNIX AND NOT APPLE) 
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}") 
else() 
    set(output_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>") 
endif()

add_custom_target(
	CopyResources
    COMMAND ${CMAKE_COMMAND} -E make_directory "${output_dir}/resource"
    # COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resource" "${output_dir}/resource"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_root_dir}/asset" "${output_dir}/resource"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_source_dir}/${moer_config_dir}" "${output_dir}/config"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${moer_root_dir}/shaders" "${output_dir}/resource/shaders"
    # COMMAND ${CMAKE_COMMAND} #print "Copying resources to ${output_dir}/resource"
    COMMENT "Copying resources to ${output_dir}/resource"
)

add_dependencies(${target_name} CopyResources)