set(TARGET_NAME Moer2Editor)

file(GLOB EDITOR_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/include/*.h)
file(GLOB EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp)

# file(GLOB EDITOR_RESOURCE CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resource/PiccoloEditor.rc)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${EDITOR_HEADERS} ${EDITOR_SOURCES})

add_executable(${TARGET_NAME} ${EDITOR_HEADERS} ${EDITOR_SOURCES})

add_compile_definitions("MOER2_ROOT_DIR=${BINARY_ROOT_DIR}")

#target_include_directories(
#  ${TARGET_NAME}
#  PUBLIC $<BUILD_INTERFACE:${THIRD_PARTY_DIR}/stb>
#)

set_target_properties(${TARGET_NAME} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "Moer2Editor")
set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "engine")

target_compile_options(${TARGET_NAME} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX->")

#target_link_libraries(${TARGET_NAME} BeeRuntime)

set(POST_BUILD_COMMANDS
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BINARY_ROOT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resource" "${BINARY_ROOT_DIR}/resource"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_FILE_DIR:${TARGET_NAME}>/" "${BINARY_ROOT_DIR}"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_ROOT_DIR}/${CONFIG_DIR}" "${BINARY_ROOT_DIR}/config"

  # for pugixml dll
  #COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:pugixml-shared>" "$<TARGET_FILE_DIR:${TARGET_NAME}>"
)

if(${CMAKE_GENERATOR} MATCHES "Visual Studio*")
  set(EDITOR_DIR "${CMAKE_BINARY_DIR}/engine/source/editor/${CMAKE_BUILD_TYPE}")
elseif(${CMAKE_GENERATOR} MATCHES "Ninja")
  set(EDITOR_DIR "${CMAKE_BINARY_DIR}/engine/source/editor")
endif()

if(NOT ${CMAKE_BINARY_DIR} STREQUAL ${BINARY_ROOT_DIR})
  set(DEVELOP_BUILD_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resource" "${EDITOR_DIR}/resource"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ENGINE_ROOT_DIR}/${CONFIG_DIR}" "${EDITOR_DIR}/config"
  )
  add_custom_command(TARGET ${TARGET_NAME} ${DEVELOP_BUILD_COMMANDS})
endif()

message(${CMAKE_GENERATOR})
add_custom_command(TARGET ${TARGET_NAME} ${POST_BUILD_COMMANDS})