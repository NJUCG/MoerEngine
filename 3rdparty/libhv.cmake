set(BUILD_EXAMPLES OFF)
set(BUILD_UNITTEST OFF)
set(BUILD_SHARED ON)
set(BUILD_STATIC OFF)
set(WITH_EVPP ON)
set(WITH_HTTP ON)
set(WITH_HTTP_SERVER ON)
set(WITH_HTTP_CLIENT ON)
set(WITH_OPENSSL OFF)

set(moer_libhv_source_dir ${CMAKE_CURRENT_SOURCE_DIR}/libhv)
set(moer_libhv_build_source_dir ${CMAKE_CURRENT_BINARY_DIR}/libhv-src)

# 此处是因为libhv会修改本身代码文件，所以我们将libhv拷贝后再进行编译
file(REMOVE_RECURSE ${moer_libhv_build_source_dir})
file(COPY ${moer_libhv_source_dir}/ DESTINATION ${moer_libhv_build_source_dir}
    PATTERN ".git" EXCLUDE
    PATTERN "build" EXCLUDE
)

add_subdirectory(${moer_libhv_build_source_dir} ${CMAKE_CURRENT_BINARY_DIR}/libhv-build)

if(TARGET hv)
    set_target_properties(hv PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_ARCHIVE_OUTPUT_DIRECTORY}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_PDB_OUTPUT_DIRECTORY}"
    )
    set_target_folder(hv ${third_party_folder}/libhv)
endif()

if(TARGET libhv)
    set_target_folder(libhv ${third_party_folder}/libhv)
endif()

unset(BUILD_EXAMPLES)
unset(BUILD_UNITTEST)
unset(BUILD_SHARED)
unset(BUILD_STATIC)
unset(WITH_EVPP)
unset(WITH_HTTP)
unset(WITH_HTTP_SERVER)
unset(WITH_HTTP_CLIENT)
unset(WITH_OPENSSL)
unset(moer_libhv_source_dir)
unset(moer_libhv_build_source_dir)