set(TARGET_NAME Moer2Runtime)
set(RUNTIME_DIR ${ENGINE_ROOT_DIR}/runtime)


set(RUNTIME_CORE_DIR ${RUNTIME_DIR}/core)
set(RUNTIME_MODULE_ROOT_DIR ${RUNTIME_DIR}/modules)
add_subdirectory(core)

add_subdirectory(modules/render)

add_library(${TARGET_NAME} INTERFACE)

set_target_properties(${TARGET_NAME} PROPERTIES FOLDER "engine")