set(target_name MoerCore)
file(GLOB_RECURSE CORE_HEADERS "include/*.h" "include/*.inl")
file(GLOB_RECURSE CORE_SOURCES "source/*.cpp")
file(GLOB_RECURSE CORE_PRIVATE_HEADERS "source/*.h")

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${CORE_HEADERS} ${CORE_SOURCES} ${CORE_PRIVATE_HEADERS})

option(MOER_CORE_SHARED_LIB "" ON)

if(MOER_CORE_SHARED_LIB)
    add_library(${target_name} SHARED ${CORE_HEADERS} ${CORE_SOURCES} ${CORE_PRIVATE_HEADERS})
    target_compile_definitions(${target_name} PRIVATE MOER_CORE_SHARED_LIB)
else()
    add_library(${target_name} STATIC ${CORE_HEADERS} ${CORE_SOURCES} ${CORE_PRIVATE_HEADERS})
endif()

set_target_properties(${target_name} PROPERTIES FOLDER "runtime" )

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
)
target_compile_definitions(${target_name} 
    PRIVATE "DEVELOP_SHADER_PATH=${moer_root_dir}/shaders"
)
