if(WITH_PROFILE)
    message(STATUS "CMake WITH_PROFILE == ON")
    set(target_name MoerProfile)
    set(moer_profile_dir ${moer_source_dir}/profile)

    set(PERFETTO_ROOT "${moer_root_dir}/3rdparty/perfetto")
    
    add_library(perfetto STATIC 
        ${PERFETTO_ROOT}/sdk/perfetto.cc
    )
    file(GLOB PROFILE_HEADERS CONFIGURE_DEPENDS "${moer_profile_dir}/*.h")
    file(GLOB PROFILE_SOURCES CONFIGURE_DEPENDS "${moer_profile_dir}/*.cpp")
    add_library(${target_name} STATIC ${PROFILE_HEADERS} ${PROFILE_SOURCES})

    set_target_properties(${target_name} PROPERTIES 
        FOLDER "profile"
    )

    target_include_directories(${target_name} 
        PUBLIC $<BUILD_INTERFACE:${moer_profile_dir}>
    )
    find_package(Threads REQUIRED)

    set(MINHOOK_ROOT "${moer_root_dir}/3rdparty/minhook")
    file(GLOB MINHOOK_SOURCES
        "${MINHOOK_ROOT}/src/*.c"
        "${MINHOOK_ROOT}/src/hde/*.c"
    )
    add_library(MinHookStatic STATIC ${MINHOOK_SOURCES})


    target_include_directories(MinHookStatic PUBLIC 
        "${MINHOOK_ROOT}/include"
    )
    set(MISC_ROOT "${moer_source_dir}/runtime/core/include/misc")
    target_include_directories(${target_name} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PERFETTO_ROOT}/sdk"
        "${MINHOOK_ROOT}/include"
        "${MINHOOK_ROOT}/src"
        "${MINHOOK_ROOT}/src/hde"   
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/core/include"
    )

    target_link_libraries(${target_name}
        PRIVATE perfetto
        PRIVATE ${CMAKE_THREAD_LIBS_INIT}
        dbghelp.lib
        Threads::Threads
        kernel32
        PRIVATE MinHookStatic
    )

    if (WIN32)
        target_link_libraries(perfetto PRIVATE ws2_32)
        target_compile_options(perfetto PRIVATE "/bigobj")
        target_compile_definitions(perfetto PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    if(MSVC)
        target_compile_options(perfetto PRIVATE "/permissive-")
    endif()
endif()
