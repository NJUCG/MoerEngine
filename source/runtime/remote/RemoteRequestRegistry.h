#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteRequestRecord.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Moer::remote {

// 负责线程安全地维护远程请求记录及状态变更
class REMOTE_API RemoteRequestRegistry {
public:
    std::string CreateRequest(scripting::ScriptExecutionRequest request);
    void        MarkRunning(std::string_view request_id);
    void        MarkCompleted(std::string_view request_id, scripting::ScriptExecutionResult result);
    void        MarkFailed(std::string_view request_id, scripting::ScriptExecutionResult result);
    void        MarkCancelled(std::string_view request_id, std::string_view reason);
    std::optional<RemoteRequestRecord> Find(std::string_view request_id) const;

private:
    // 生成下一个稳定递增的远程请求 ID
    std::string NextRequestId();

    // 统一处理结束态请求的状态写回与结果保存
    void MarkFinished(
        std::string_view                 request_id,
        ERemoteExecutionState            state,
        scripting::ScriptExecutionResult result
    );

    mutable std::mutex                                   m_mutex;
    std::unordered_map<std::string, RemoteRequestRecord> m_records;
    std::atomic<uint64_t>                                m_next_request_index = 1;
};

} // namespace Moer::remote