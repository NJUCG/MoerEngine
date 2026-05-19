#pragma once

#include "remote/RemoteApi.h"

namespace Moer::remote {

// 表示一条远程脚本请求在服务端的执行状态
enum class ERemoteExecutionState {
    Queued,
    Running,
    Completed,
    Failed,
    Cancelled,
};

// 将远程执行状态转换为稳定的字符串表示
REMOTE_API const char* ToString(ERemoteExecutionState state);

} // namespace Moer::remote