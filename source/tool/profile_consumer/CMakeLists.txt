set(target_name moer_profile_consumer)

file(GLOB_RECURSE profile_consumer_headers CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE profile_consumer_sources CONFIGURE_DEPENDS "source/*.cpp")

source_group(
    TREE ${CMAKE_CURRENT_SOURCE_DIR}
    FILES ${profile_consumer_headers} ${profile_consumer_sources}
)

add_library(
    ${target_name}
    STATIC
    ${profile_consumer_headers}
    ${profile_consumer_sources}
)
add_library(Moer::ProfileConsumer ALIAS ${target_name})

target_include_directories(
    ${target_name}
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(
    ${target_name}
    PUBLIC Moer::Core
)

set_target_properties(${target_name} PROPERTIES FOLDER "tool")
set_target_properties(${target_name} PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_executable(
    MoerProfileSummary
    "app/ProfileSummaryMain.cpp"
)
target_link_libraries(
    MoerProfileSummary
    PRIVATE
        Moer::ProfileConsumer
        nlohmann_json::nlohmann_json
)
set_target_properties(MoerProfileSummary PROPERTIES FOLDER "tool")
