#pragma once

#include "scripting/ScriptingApi.h"

#include <string>

namespace Moer::scripting {

enum class EScriptRequestOrigin {
    EditorUiPanel,
    Terminal,
    Mcp,
};

enum class EScriptExecutionKind {
    ExecSnippet,
};

enum class EScriptSessionPolicy {
    SharedGlobal,
    NamedSession,
    Stateless,
};

// 描述一次脚本执行请求的完整输入语义
struct SCRIPTING_API ScriptExecutionRequest {
    EScriptRequestOrigin origin         = EScriptRequestOrigin::EditorUiPanel;
    EScriptExecutionKind execution_kind = EScriptExecutionKind::ExecSnippet;
    EScriptSessionPolicy session_policy = EScriptSessionPolicy::SharedGlobal;
    std::string          session_id;
    std::string          source_name = "<script>";
    std::string          code;
};

} // namespace Moer::scripting
