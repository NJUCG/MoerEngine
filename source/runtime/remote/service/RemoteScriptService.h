#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteExecutionState.h"
#include "remote/RemoteRequestRecord.h"
#include "remote/RemoteSubmitFn.h"
#include "scripting/ScriptExecutionRequest.h"
#include "scripting/ScriptExecutionResult.h"

#include <optional>
#include <string>
#include <string_view>

namespace Moer::remote {

class RemoteEventHub;
class RemoteRequestRegistry;

// 表示一次来自远程接口的脚本执行请求参数
struct REMOTE_API RemoteExecuteScriptRequest {
    std::string                     code;
    std::string                     source_name    = "remote/http";
    scripting::EScriptExecutionKind execution_kind = scripting::EScriptExecutionKind::ExecSnippet;
    scripting::EScriptSessionPolicy session_policy = scripting::EScriptSessionPolicy::Stateless;
    std::string                     session_id;
    scripting::EScriptRequestOrigin origin = scripting::EScriptRequestOrigin::Terminal;
};

// 表示远程脚本执行接口返回给调用方的结果
struct REMOTE_API RemoteExecuteScriptResponse {
    std::string                      request_id;
    ERemoteExecutionState            state = ERemoteExecutionState::Queued;
    scripting::ScriptExecutionResult result;
};

// 串联请求转换、脚本提交、状态记录和事件广播的核心服务
//
// TODO: 脚本执行取消
// - 说明：允许外部停止长时间运行的脚本请求
// - 路径：补齐 ScriptExecutionFuture、ScriptHost、remote service 的 cancel 链路，并增加对应 HTTP 接口
class REMOTE_API RemoteScriptService {
public:
    // 使用脚本提交回调和共享基础设施构造服务对象
    RemoteScriptService(
        RemoteSubmitScriptExecutionFn submit_fn,
        RemoteRequestRegistry&        registry,
        RemoteEventHub&               event_hub
    );

    // 执行一次远程脚本请求并等待结果返回
    RemoteExecuteScriptResponse ExecuteAndWait(RemoteExecuteScriptRequest request);
    // 查询一条远程请求当前保存的记录
    std::optional<RemoteRequestRecord> FindRequest(std::string_view request_id) const;

private:
    RemoteSubmitScriptExecutionFn m_submit_fn;
    RemoteRequestRegistry&        m_registry;
    RemoteEventHub&               m_event_hub;
};

} // namespace Moer::remote