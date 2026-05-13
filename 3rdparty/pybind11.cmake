set(pybind11_target_root "${CMAKE_CURRENT_SOURCE_DIR}/pybind11")
set(pybind11_target_include_dir "${pybind11_target_root}/include")

if (NOT EXISTS "${pybind11_target_include_dir}")
    message(FATAL_ERROR "Required pybind11 include path does not exist: ${pybind11_target_include_dir}")
endif()

if (NOT TARGET pybind11_headers)
    add_library(pybind11_headers INTERFACE)
    target_include_directories(pybind11_headers INTERFACE "${pybind11_target_include_dir}")
    add_library(pybind11::headers ALIAS pybind11_headers)
endif()

set_target_folder(pybind11_headers ${third_party_folder}/pybind11)