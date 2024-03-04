set(KTX_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_library(ktx INTERFACE)

target_sources(ktx INTERFACE
    ${KTX_DIR}/lib/texture.c
    ${KTX_DIR}/lib/hashlist.c
    ${KTX_DIR}/lib/checkheader.c
    ${KTX_DIR}/lib/swap.c
    ${KTX_DIR}/lib/memstream.c
    ${KTX_DIR}/lib/filestream.c)
target_include_directories(ktx INTERFACE ${KTX_DIR}/include ${KTX_DIR}/lib  ${KTX_DIR}/other_include)