#include "remote/service/RemoteScriptService.h"

#include "remote/RemoteEvent.h"
#include "remote/RemoteEventHub.h"
#include "remote/RemoteRequestRegistry.h"

#include <exception>
#include <utility>

namespace Moer::remote {

namespace {

RemoteEvent MakeStateEvent(
    std::string                             request_id,
    ERemoteExecutionState                   state,
    const scripting::ScriptExecutionResult& result
) {
    RemoteEvent event;
    event.type         = state == ERemoteExecutionState::Completed ? "script.completed" : "script.failed";
    event.request_id   = std::move(request_id);
    event.state        = state;
    event.message      = result.exception_text;
    event.stdout_chunk = result.stdout_text;
    event.stderr_chunk = result.stderr_text;
    return event;
}

} // namespace

RemoteScriptService::RemoteScriptService(
    RemoteSubmitScriptExecutionFn submit_fn,
    RemoteRequestRegistry&        registry,
    RemoteEventHub&               event_hub
) :
    m_submit_fn(std::move(submit_fn)),
    m_registry(registry),
    m_event_hub(event_hub) {}

RemoteExecuteScriptResponse RemoteScriptService::ExecuteAndWait(RemoteExecuteScriptRequest request) {
    scripting::ScriptExecutionRequest script_request;
    script_request.origin         = request.origin;
    script_request.execution_kind = request.execution_kind;
    script_request.session_policy = request.session_policy;
    script_request.session_id     = std::move(request.session_id);
    script_request.source_name    = std::move(request.source_name);
    script_request.code           = std::move(request.code);

    RemoteExecuteScriptResponse response;
    response.request_id = m_registry.CreateRequest(script_request);
    response.state      = ERemoteExecutionState::Running;
    m_registry.MarkRunning(response.request_id);

    try {
        if (!m_submit_fn) {
            response.result.exception_text = "Remote submit function is not available.";
            response.state                 = ERemoteExecutionState::Failed;
            m_registry.MarkFailed(response.request_id, response.result);
            m_event_hub.Publish(MakeStateEvent(response.request_id, response.state, response.result));
            return response;
        }

        auto future = m_submit_fn(std::move(script_request));
        if (!future.valid()) {
            response.result.exception_text = "Remote submit function returned an invalid future.";
            response.state                 = ERemoteExecutionState::Failed;
            m_registry.MarkFailed(response.request_id, response.result);
            m_event_hub.Publish(MakeStateEvent(response.request_id, response.state, response.result));
            return response;
        }

        response.result = future.get();
        response.state =
            response.result.success ? ERemoteExecutionState::Completed : ERemoteExecutionState::Failed;
        if (response.state == ERemoteExecutionState::Completed) {
            m_registry.MarkCompleted(response.request_id, response.result);
        } else {
            m_registry.MarkFailed(response.request_id, response.result);
        }
        m_event_hub.Publish(MakeStateEvent(response.request_id, response.state, response.result));
        return response;
    } catch (const std::exception& ex) {
        response.result.exception_text = ex.what();
    } catch (...) {
        response.result.exception_text = "Unknown remote script execution error.";
    }

    response.state = ERemoteExecutionState::Failed;
    m_registry.MarkFailed(response.request_id, response.result);
    m_event_hub.Publish(MakeStateEvent(response.request_id, response.state, response.result));
    return response;
}

std::optional<RemoteRequestRecord> RemoteScriptService::FindRequest(std::string_view request_id) const {
    return m_registry.Find(request_id);
}

} // namespace Moer::remote