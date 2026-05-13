#include "scripting/ScriptHost.h"

#include "log/LogSystem.h"
#include "scripting/PythonRuntime.h"

#include <stdexcept>
#include <string_view>

namespace Moer::scripting {

struct ScriptHost::PendingExecution {
    ScriptExecutionRequest              request;
    std::promise<ScriptExecutionResult> promise;
};

namespace {

ScriptExecutionResult MakeScriptHostError(std::string_view message) {
    ScriptExecutionResult result;
    result.exception_text = std::string(message);
    return result;
}

} // namespace

ScriptHost::ScriptHost(PythonRuntimeConfig runtime_config) : m_runtime_config(std::move(runtime_config)) {}

ScriptHost::~ScriptHost() {
    Stop();
}

void ScriptHost::Start() {
    std::unique_lock lock(m_mutex);
    if (m_worker_thread.joinable()) {
        return;
    }

    m_stop_requested = false;
    m_is_running     = false;

    std::promise<std::string> startup_promise;
    auto                      startup_future = startup_promise.get_future();

    m_worker_thread = std::thread(&ScriptHost::WorkerMain, this, std::move(startup_promise));
    lock.unlock();

    const std::string startup_error = startup_future.get();
    if (!startup_error.empty()) {
        if (m_worker_thread.joinable()) {
            m_worker_thread.join();
        }
        throw std::runtime_error(startup_error);
    }

    LOG_INFO("ScriptHost started.");
}

void ScriptHost::Stop() {
    {
        std::lock_guard lock(m_mutex);
        if (!m_worker_thread.joinable()) {
            m_is_running     = false;
            m_stop_requested = false;
            return;
        }

        m_stop_requested = true;
    }

    m_condition.notify_all();
    m_worker_thread.join();

    std::deque<std::unique_ptr<PendingExecution>> pending_executions;
    {
        std::lock_guard lock(m_mutex);
        pending_executions.swap(m_pending_executions);
        m_is_running     = false;
        m_stop_requested = false;
    }

    for (auto& pending_execution : pending_executions) {
        pending_execution->promise.set_value(
            MakeScriptHostError("ScriptHost stopped before request execution.")
        );
    }

    LOG_INFO("ScriptHost stopped.");
}

std::future<ScriptExecutionResult> ScriptHost::SubmitSnippet(ScriptExecutionRequest request) {
    auto pending_execution     = std::make_unique<PendingExecution>();
    auto execution_future      = pending_execution->promise.get_future();
    pending_execution->request = std::move(request);

    {
        std::lock_guard lock(m_mutex);
        if (!m_worker_thread.joinable() || !m_is_running || m_stop_requested) {
            pending_execution->promise.set_value(MakeScriptHostError("ScriptHost is not running."));
            return execution_future;
        }

        m_pending_executions.push_back(std::move(pending_execution));
    }

    m_condition.notify_one();
    return execution_future;
}

void ScriptHost::WorkerMain(std::promise<std::string> startup_promise) {
    PythonRuntime runtime;
    bool          startup_value_sent = false;

    try {
        runtime.Initialize(m_runtime_config);
        {
            std::lock_guard lock(m_mutex);
            m_is_running = true;
        }

        startup_promise.set_value({});
        startup_value_sent = true;

        while (true) {
            std::unique_ptr<PendingExecution> pending_execution;
            {
                std::unique_lock lock(m_mutex);
                m_condition.wait(lock, [this]() {
                    return m_stop_requested || !m_pending_executions.empty();
                });

                if (m_pending_executions.empty()) {
                    if (m_stop_requested) {
                        break;
                    }
                    continue;
                }

                pending_execution = std::move(m_pending_executions.front());
                m_pending_executions.pop_front();
            }

            pending_execution->promise.set_value(runtime.ExecuteSnippet(pending_execution->request));
        }
    } catch (const std::exception& ex) {
        std::deque<std::unique_ptr<PendingExecution>> pending_executions;
        {
            std::lock_guard lock(m_mutex);
            pending_executions.swap(m_pending_executions);
            m_is_running     = false;
            m_stop_requested = true;
        }

        for (auto& pending_execution : pending_executions) {
            pending_execution->promise.set_value(MakeScriptHostError(ex.what()));
        }

        if (!startup_value_sent) {
            startup_promise.set_value(ex.what());
            startup_value_sent = true;
        }

        LOG_ERROR("ScriptHost worker failed: {}", ex.what());
    }

    if (runtime.IsInitialized()) {
        runtime.Finalize();
    }

    {
        std::lock_guard lock(m_mutex);
        m_is_running = false;
    }
}

} // namespace Moer::scripting
