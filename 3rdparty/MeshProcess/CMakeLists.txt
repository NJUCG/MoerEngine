set(third_lib_for_mesh_process MeshProcess)

add_library(${third_lib_for_mesh_process} SHARED  dummy.cpp) 

set_target_properties(${third_lib_for_mesh_process} PROPERTIES
WINDOWS_EXPORT_ALL_SYMBOLS ON)

copy_dll(metis metis SHARED ${CMAKE_CURRENT_SOURCE_DIR}/metis)
add_subdirectory(meshoptimizer)
target_include_directories(${third_lib_for_mesh_process} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/meshoptimizer/src")

target_compile_definitions(${third_lib_for_mesh_process} PUBLIC "IDXTYPEWIDTH=32" "REALTYPEWIDTH=64")
set_target_folder(meshoptimizer "${third_party_folder}/meshoptimizer")
target_link_libraries(${third_lib_for_mesh_process} PUBLIC meshoptimizer metis)
