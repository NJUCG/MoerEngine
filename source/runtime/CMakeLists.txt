set(TARGET_NAME MoerEngineRuntime)
set(RUNTIME_DIR ${ENGINE_ROOT_DIR}/runtime)


set(RUNTIME_CORE_DIR ${RUNTIME_DIR}/core)
set(RUNTIME_MODULE_ROOT_DIR ${RUNTIME_DIR}/modules)


add_subdirectory(core)

add_subdirectory(modules/render)

add_library(${TARGET_NAME} Engine.h Engine.cpp)

set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "engine")

target_include_directories(${TARGET_NAME} 
    PRIVATE $<BUILD_INTERFACE:${RUNTIME_CORE_DIR}/include>
    PRIVATE $<BUILD_INTERFACE:${RUNTIME_MODULE_ROOT_DIR}/render/include>
)

target_link_libraries(${TARGET_NAME}
    PUBLIC MoerRuntimeCore
    PUBLIC render
)


set(RUNTIME_TEST ON)

if(${RUNTIME_TEST})

add_executable(RuntimeTest EngineTest.cpp)

target_include_directories(RuntimeTest
    PRIVATE $<BUILD_INTERFACE:${RUNTIME_CORE_DIR}/include>
    PRIVATE $<BUILD_INTERFACE:${RUNTIME_MODULE_ROOT_DIR}/render/include>
    
    )

    target_link_libraries(RuntimeTest 
       PUBLIC ${TARGET_NAME})

       set_target_properties(RuntimeTest PROPERTIES FOLDER "test")
endif()