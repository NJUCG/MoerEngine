#include "scripting/PythonRuntime.h"

#include "log/LogSystem.h"

#if defined(_DEBUG)
#define MOER_PYTHON_RUNTIME_RESTORE_DEBUG 1
#undef _DEBUG
#endif
#include <Python.h>
#if defined(MOER_PYTHON_RUNTIME_RESTORE_DEBUG)
#define _DEBUG 1
#undef MOER_PYTHON_RUNTIME_RESTORE_DEBUG
#endif
#include <pybind11/embed.h>
#include <pybind11/eval.h>

#include <stdexcept>
#include <string>

namespace py = pybind11;

namespace Moer::scripting {

struct PythonRuntime::State {
    py::dict globals;
};

namespace {

std::runtime_error MakePyConfigError(const char* fallback_message, const PyStatus& status) {
    return std::runtime_error(PyStatus_IsError(status) != 0 ? status.err_msg : fallback_message);
}

void SetConfigString(
    PyConfig&                    config,
    wchar_t**                    target,
    const std::filesystem::path& value,
    const char*                  label
) {
    const std::wstring wide_value = value.wstring();
    PyStatus           status     = PyConfig_SetString(&config, target, wide_value.c_str());
    if (PyStatus_Exception(status) != 0) {
        PyConfig_Clear(&config);
        throw MakePyConfigError(label, status);
    }
}

void AppendModuleSearchPath(PyConfig& config, const std::filesystem::path& value, const char* label) {
    const std::wstring wide_value = value.wstring();
    PyStatus           status     = PyWideStringList_Append(&config.module_search_paths, wide_value.c_str());
    if (PyStatus_Exception(status) != 0) {
        PyConfig_Clear(&config);
        throw MakePyConfigError(label, status);
    }
}

std::string ReadStringIO(py::handle buffer) {
    if (!buffer) {
        return {};
    }

    return py::str(buffer.attr("getvalue")()).cast<std::string>();
}

} // namespace

PythonRuntime::PythonRuntime() = default;

PythonRuntime::~PythonRuntime() {
    Finalize();
}

void PythonRuntime::Initialize(const PythonRuntimeConfig& config) {
    if (m_is_initialized) {
        return;
    }

    m_config          = config;
    m_owner_thread_id = std::this_thread::get_id();

    PyConfig py_config;
    PyConfig_InitPythonConfig(&py_config);

    py_config.parse_argv              = 0;
    py_config.install_signal_handlers = 0;
    py_config.use_environment         = 0;
    py_config.user_site_directory     = 0;
    py_config.write_bytecode          = 0;
    py_config.site_import             = 0;
    py_config.module_search_paths_set = 1;

    SetConfigString(py_config, &py_config.home, m_config.runtime_root, "Failed to set Python home");
    SetConfigString(
        py_config, &py_config.executable, m_config.program_path, "Failed to set Python executable"
    );
    SetConfigString(
        py_config, &py_config.program_name, m_config.program_path, "Failed to set Python program name"
    );

    AppendModuleSearchPath(py_config, m_config.runtime_root, "Failed to append Python runtime root");
    AppendModuleSearchPath(py_config, m_config.stdlib_dir, "Failed to append Python stdlib dir");
    AppendModuleSearchPath(py_config, m_config.dll_dir, "Failed to append Python DLL dir");

    const std::string program_path_utf8 = m_config.program_path.string();
    const char*       argv[]            = {program_path_utf8.c_str()};

    py::initialize_interpreter(&py_config, 1, argv, false);

    {
        py::gil_scoped_acquire guard;
        auto                   main_module = py::module_::import("__main__");

        m_state          = std::make_unique<State>();
        m_state->globals = main_module.attr("__dict__").cast<py::dict>();
    }

    m_is_initialized = true;

    LOG_INFO(
        "PythonRuntime initialized. runtime_root='{}', stdlib_dir='{}', dll_dir='{}'",
        m_config.runtime_root.generic_string(),
        m_config.stdlib_dir.generic_string(),
        m_config.dll_dir.generic_string()
    );
}

void PythonRuntime::Finalize() {
    if (!m_is_initialized) {
        return;
    }

    EnsureOwnerThread();

    if (m_state) {
        py::gil_scoped_acquire guard;
        if (m_state->globals) {
            py::handle globals = m_state->globals.release();
            globals.dec_ref();
        }
    }
    m_state.reset();

    py::finalize_interpreter();

    m_is_initialized  = false;
    m_owner_thread_id = {};

    LOG_INFO("PythonRuntime finalized.");
}

bool PythonRuntime::IsInitialized() const {
    return m_is_initialized;
}

ScriptExecutionResult PythonRuntime::ExecuteSnippet(const ScriptExecutionRequest& request) {
    if (!m_is_initialized) {
        ScriptExecutionResult result;
        result.exception_text = "PythonRuntime is not initialized.";
        return result;
    }

    EnsureOwnerThread();

    py::gil_scoped_acquire guard;
    return ExecuteSnippetOnGlobals(request, m_state->globals);
}

ScriptExecutionResult
PythonRuntime::ExecuteSnippet(const ScriptExecutionRequest& request, const py::dict& globals) {
    if (!m_is_initialized) {
        ScriptExecutionResult result;
        result.exception_text = "PythonRuntime is not initialized.";
        return result;
    }

    EnsureOwnerThread();

    py::gil_scoped_acquire guard;
    return ExecuteSnippetOnGlobals(request, globals);
}

py::dict PythonRuntime::GetSharedGlobals() const {
    if (!m_is_initialized) {
        throw std::runtime_error("PythonRuntime is not initialized.");
    }

    EnsureOwnerThread();

    py::gil_scoped_acquire guard;
    return m_state->globals;
}

py::dict PythonRuntime::CopySharedGlobals() const {
    if (!m_is_initialized) {
        throw std::runtime_error("PythonRuntime is not initialized.");
    }

    EnsureOwnerThread();

    py::gil_scoped_acquire guard;
    return m_state->globals.attr("copy")().cast<py::dict>();
}

ScriptExecutionResult
PythonRuntime::ExecuteSnippetOnGlobals(const ScriptExecutionRequest& request, const py::dict& globals) {
    ScriptExecutionResult result;

    if (!globals) {
        result.exception_text = "PythonRuntime globals is empty.";
        return result;
    }

    try {
        py::object io_module     = py::module_::import("io");
        py::object stdout_buffer = io_module.attr("StringIO")();
        py::object stderr_buffer = io_module.attr("StringIO")();

        py::dict locals;
        locals["__moer_code__"]    = request.code;
        locals["__moer_globals__"] = globals;
        locals["__moer_stdout__"]  = stdout_buffer;
        locals["__moer_stderr__"]  = stderr_buffer;

        try {
            py::exec(
                R"(
import contextlib
with contextlib.redirect_stdout(__moer_stdout__), contextlib.redirect_stderr(__moer_stderr__):
    exec(__moer_code__, __moer_globals__, __moer_globals__)
)",
                m_state->globals,
                locals
            );

            result.success = true;
        } catch (const py::error_already_set& ex) {
            result.exception_text = ex.what();
        }

        result.stdout_text = ReadStringIO(stdout_buffer);
        result.stderr_text = ReadStringIO(stderr_buffer);
    } catch (const std::exception& ex) {
        result.exception_text = ex.what();
    }

    return result;
}

void PythonRuntime::EnsureOwnerThread() const {
    if (m_owner_thread_id != std::this_thread::get_id()) {
        throw std::runtime_error("PythonRuntime must be used from its owner thread.");
    }
}

} // namespace Moer::scripting
