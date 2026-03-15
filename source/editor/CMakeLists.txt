set(target_name MoerEditor)

file(GLOB_RECURSE EDITOR_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${EDITOR_HEADERS} ${EDITOR_SOURCES})

add_executable(${target_name} ${EDITOR_SOURCES})

add_compile_definitions("moer_root_dir=${BINARY_ROOT_DIR}")

# Target properties
set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerEditor")
set_target_properties(${target_name} PROPERTIES FOLDER "editor")

# Dependencies : Moer::Runtime (including Moer::Core, Moer::Render)
target_link_libraries(${target_name}
    PRIVATE Moer::Runtime
    PRIVATE nfd
)

# Include directories
target_include_directories(
  ${target_name}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Compiler options
# I want to remove the following line
target_compile_options(${target_name} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX->")

if (WITH_PROFILE)
    target_link_libraries(${target_name}
        PRIVATE moer_profile
    )
endif()