if(WITH_PROFILE)
    message(STATUS "CMake WITH_PROFILE == ON")
    
    set(target_name moer_profile)
    set(moer_profile_dir ${moer_source_dir}/profile)
    file(GLOB PROFILE_HEADERS CONFIGURE_DEPENDS "${moer_profile_dir}/*.h")
    file(GLOB PROFILE_SOURCES CONFIGURE_DEPENDS "${moer_profile_dir}/*.cpp")
    list(REMOVE_ITEM PROFILE_SOURCES "${moer_profile_dir}/moer_profiler.cpp")
    list(REMOVE_ITEM PROFILE_SOURCES "${moer_profile_dir}/VkLayer.cpp")
    

    add_library(${target_name} SHARED ${PROFILE_HEADERS} ${PROFILE_SOURCES})

    add_library(Moer::Profile ALIAS ${target_name}) # 别名，方便其他模块引用

    set_target_properties(${target_name} PROPERTIES 
        FOLDER "profile"
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )

    set(PERFETTO_ROOT "${moer_root_dir}/3rdparty/perfetto")
    
    add_library(perfetto STATIC 
        ${PERFETTO_ROOT}/sdk/perfetto.cc
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
    set(moer_third_party_dir "${moer_root_dir}/3rdparty")
    target_include_directories(${target_name} PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${PERFETTO_ROOT}/sdk"
        "${MINHOOK_ROOT}/include"
        "${MINHOOK_ROOT}/src"
        "${MINHOOK_ROOT}/src/hde"   
        "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/core/include"
        "${moer_source_dir}/runtime/render/rhi/vulkan"
        "${moer_source_dir}/runtime/render"
        "${moer_root_dir}/3rdparty/VulkanSDK/include"
        "${moer_root_dir}/3rdparty/imgui"
    )
    
    target_link_libraries(${target_name}
        PRIVATE perfetto
        PRIVATE ${CMAKE_THREAD_LIBS_INIT}
        dbghelp.lib
        Threads::Threads
        kernel32
        PRIVATE MinHookStatic
        mimalloc-static
    )

    target_compile_definitions(${target_name} PUBLIC WITH_PROFILE=1)
    message(STATUS "WITH_PROFILE=1 in Moer::Profile")

    set(exe_target moer_profiler)
    add_executable(${exe_target} "${moer_profile_dir}/moer_profiler.cpp")
    target_link_libraries(${exe_target} PRIVATE kernel32.lib)

    target_compile_definitions(${target_name} PRIVATE MOER_PROFILE_EXPORTS)

    if (WIN32)
        target_link_libraries(perfetto PRIVATE ws2_32)

        # 仅在 MSVC 或 clang-cl 前端下使用 /bigobj，避免 clang++ 把它当作文件名
        if (MSVC OR (CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
                     AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
            target_compile_options(perfetto PRIVATE "/bigobj")
        endif()

        target_compile_definitions(perfetto PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    if(MSVC)
        target_compile_options(perfetto PRIVATE "/permissive-")
    endif()
endif()
