file(GLOB metis_sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/metis/libmetis/*.c)

add_library(metis STATIC ${metis_sources})

target_compile_definitions(metis
    PUBLIC IDXTYPEWIDTH=32 REALTYPEWIDTH=64
)

target_include_directories(metis
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/metis/include>
    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/metis/libmetis>
)

target_link_libraries(metis
    PUBLIC GKlib::GKlib
    PUBLIC $<$<NOT:$<BOOL:${WIN32}>>:m>
)

set_target_properties(metis PROPERTIES POSITION_INDEPENDENT_CODE ON)