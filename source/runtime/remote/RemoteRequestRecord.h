#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteExecutionState.h"
#include "scripting/ScriptExecutionRequest.h"
#include "scripting/ScriptExecutionResult.h"

#include <optional>
#include <string>

namespace Moer::remote {

// 保存一条远程脚本请求及其当前状态快照
struct REMOTE_API RemoteRequestRecord {
    std::string                                     request_id;
    ERemoteExecutionState                           state = ERemoteExecutionState::Queued;
    scripting::ScriptExecutionRequest               request;
    std::optional<scripting::ScriptExecutionResult> result;
};

} // namespace Moer::remote