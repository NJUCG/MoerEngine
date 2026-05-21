#include "remote/RemoteRequestRegistry.h"

#include <format>
#include <utility>

namespace Moer::remote {

std::string RemoteRequestRegistry::CreateRequest(scripting::ScriptExecutionRequest request) {
    RemoteRequestRecord record;
    record.request_id = NextRequestId();
    record.request    = std::move(request);
    record.state      = ERemoteExecutionState::Queued;

    const std::string request_id = record.request_id;
    {
        std::lock_guard lock(m_mutex);
        m_records.emplace(request_id, std::move(record));
    }

    return request_id;
}

void RemoteRequestRegistry::MarkRunning(std::string_view request_id) {
    std::lock_guard lock(m_mutex);
    auto            iter = m_records.find(std::string(request_id));
    if (iter != m_records.end()) {
        iter->second.state = ERemoteExecutionState::Running;
    }
}

void RemoteRequestRegistry::MarkCompleted(
    std::string_view                 request_id,
    scripting::ScriptExecutionResult result
) {
    MarkFinished(request_id, ERemoteExecutionState::Completed, std::move(result));
}

void RemoteRequestRegistry::MarkFailed(std::string_view request_id, scripting::ScriptExecutionResult result) {
    MarkFinished(request_id, ERemoteExecutionState::Failed, std::move(result));
}

void RemoteRequestRegistry::MarkCancelled(std::string_view request_id, std::string_view reason) {
    scripting::ScriptExecutionResult result;
    result.exception_text = std::string(reason);
    MarkFinished(request_id, ERemoteExecutionState::Cancelled, std::move(result));
}

std::optional<RemoteRequestRecord> RemoteRequestRegistry::Find(std::string_view request_id) const {
    std::lock_guard lock(m_mutex);
    auto            iter = m_records.find(std::string(request_id));
    if (iter == m_records.end()) {
        return std::nullopt;
    }

    return iter->second;
}

std::string RemoteRequestRegistry::NextRequestId() {
    const uint64_t index = m_next_request_index.fetch_add(1, std::memory_order_relaxed);
    return std::format("remote-{0:06}", index);
}

void RemoteRequestRegistry::MarkFinished(
    std::string_view                 request_id,
    ERemoteExecutionState            state,
    scripting::ScriptExecutionResult result
) {
    std::lock_guard lock(m_mutex);
    auto            iter = m_records.find(std::string(request_id));
    if (iter != m_records.end()) {
        iter->second.state  = state;
        iter->second.result = std::move(result);
    }
}

} // namespace Moer::remote