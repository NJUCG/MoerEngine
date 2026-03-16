# Export Moer::Core target

set(target_name moer_core)

file(GLOB_RECURSE core_headers CONFIGURE_DEPENDS "include/*.h" "include/*.inl")
file(GLOB_RECURSE core_sources CONFIGURE_DEPENDS "source/*.cpp")
file(GLOB_RECURSE core_private_headers CONFIGURE_DEPENDS "source/*.h")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${core_headers} ${core_sources} ${core_private_headers})

option(MOER_CORE_SHARED_LIB "" ON)

if(MOER_CORE_SHARED_LIB)
    add_library(${target_name} SHARED ${core_headers} ${core_sources} ${core_private_headers})
    target_compile_definitions(${target_name} PRIVATE MOER_CORE_SHARED_LIB)
else()
    add_library(${target_name} STATIC ${core_headers} ${core_sources} ${core_private_headers})
endif()
add_library(Moer::Core ALIAS ${target_name})

set_target_properties(${target_name} PROPERTIES FOLDER "runtime")

target_include_directories(${target_name}
	PUBLIC $<BUILD_INTERFACE:${moer_runtime_core_dir}/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/spdlog/include>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/zpp_bits>
    PUBLIC $<BUILD_INTERFACE:${moer_third_party_dir}/json/include>
)

target_link_libraries(${target_name}
    PUBLIC mimalloc-static
    PUBLIC spdlog
    PUBLIC nlohmann_json::nlohmann_json
    PUBLIC tomlplusplus
)

if(WIN32)
    target_link_libraries(${target_name} PUBLIC ws2_32)
endif()

# add_dependencies(${target_name} CopyMimallocDlls )
