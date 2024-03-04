# astc
set(ASTC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(ASTC_INCLUDE_DIR ${ASTC_DIR}/Source)
set(ASTC_SOURCE_DIR ${ASTC_DIR}/Source)

set(ASTC_SOURCES
        ${ASTC_SOURCE_DIR}/astc_image_load_store.cpp
        ${ASTC_SOURCE_DIR}/astc_pick_best_endpoint_format.cpp
        ${ASTC_SOURCE_DIR}/astc_color_quantize.cpp
        ${ASTC_SOURCE_DIR}/astc_weight_align.cpp
        ${ASTC_SOURCE_DIR}/astc_integer_sequence.cpp
        ${ASTC_SOURCE_DIR}/astc_ideal_endpoints_and_weights.cpp
        ${ASTC_SOURCE_DIR}/astc_find_best_partitioning.cpp
        ${ASTC_SOURCE_DIR}/astc_compress_symbolic.cpp
        ${ASTC_SOURCE_DIR}/astc_decompress_symbolic.cpp
        ${ASTC_SOURCE_DIR}/astc_symbolic_physical.cpp
        ${ASTC_SOURCE_DIR}/astc_toplevel.cpp
        ${ASTC_SOURCE_DIR}/astc_stb_tga.cpp
        ${ASTC_SOURCE_DIR}/softfloat.cpp
        ${ASTC_SOURCE_DIR}/mathlib.cpp
        ${ASTC_SOURCE_DIR}/astc_quantization.cpp
        ${ASTC_SOURCE_DIR}/astc_block_sizes2.cpp
        ${ASTC_SOURCE_DIR}/astc_percentile_tables.cpp
        ${ASTC_SOURCE_DIR}/astc_averages_and_directions.cpp
        ${ASTC_SOURCE_DIR}/astc_partition_tables.cpp
        ${ASTC_SOURCE_DIR}/astc_color_unquantize.cpp
        ${ASTC_SOURCE_DIR}/astc_encoding_choice_error.cpp
        ${ASTC_SOURCE_DIR}/astc_kmeans_partitioning.cpp
        ${ASTC_SOURCE_DIR}/astc_weight_quant_xfer_tables.cpp
        ${ASTC_SOURCE_DIR}/astc_compute_variance.cpp
        ${ASTC_SOURCE_DIR}/astc_ktx_dds.cpp
)

add_library(astc STATIC ${ASTC_SOURCES})
target_include_directories(astc PUBLIC ${ASTC_INCLUDE_DIR})
target_compile_definitions(astc PRIVATE -DNO_STB_IMAGE_IMPLEMENTATION)
set_target_properties(astc PROPERTIES FOLDER "ext" POSITION_INDEPENDENT_CODE ON)