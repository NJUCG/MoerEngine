# target_name 对应 MoerEditor 可执行文件
# editor_library_target 对应 moer_editor_app 库目标
# Moer::EditorApp 是 editor_library_target 的 alias library

set(target_name MoerEditor)
set(editor_library_target moer_editor_app)

# 收集 editor 源文件
file(GLOB_RECURSE EDITOR_HEADERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
file(GLOB_RECURSE EDITOR_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
set(EDITOR_APP_SOURCES ${EDITOR_SOURCES})
list(REMOVE_ITEM EDITOR_APP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")

# 组织 VS 工程视图
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${EDITOR_HEADERS} ${EDITOR_SOURCES})

# 构建 EditorApp 库
add_library(${editor_library_target} STATIC ${EDITOR_HEADERS} ${EDITOR_APP_SOURCES})
add_library(Moer::EditorApp ALIAS ${editor_library_target})

# 共享 editor 编译定义
add_compile_definitions("moer_root_dir=${BINARY_ROOT_DIR}")

# 配置 EditorApp 库属性与依赖
set_target_properties(${editor_library_target} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerEditorApp")
set_target_properties(${editor_library_target} PROPERTIES FOLDER "editor")

target_link_libraries(${editor_library_target}
    PUBLIC Moer::Runtime
    PRIVATE nfd
)

if(WIN32)
    # StartupSplash is a renderer-independent native Win32/GDI window.
    target_link_libraries(${editor_library_target} PRIVATE user32 gdi32 msimg32)
    target_compile_definitions(${editor_library_target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

target_include_directories(
  ${editor_library_target}
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_compile_options(${editor_library_target} PUBLIC "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/WX->")

# 构建 MoerEditor 可执行文件
add_executable(${target_name} "main.cpp")

# 配置 MoerEditor 可执行文件属性与依赖
set_target_properties(${target_name} PROPERTIES CXX_STANDARD 20 OUTPUT_NAME "MoerEditor")
set_target_properties(${target_name} PROPERTIES FOLDER "editor")

target_link_libraries(${target_name}
    PRIVATE Moer::EditorApp
)

target_include_directories(${target_name}
  PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
