#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteExecutionState.h"

#include <string>

namespace Moer::remote {

// 表示一条要广播给远程客户端的事件
struct REMOTE_API RemoteEvent {
    std::string           type;
    std::string           request_id;
    std::string           message;
    std::string           stdout_chunk;
    std::string           stderr_chunk;
    ERemoteExecutionState state = ERemoteExecutionState::Queued;
};

} // namespace Moer::remote