#pragma once

#include "scripting/ScriptingApi.h"

#include <string>

namespace Moer::scripting {

// 描述一次脚本执行请求的输出结果
struct SCRIPTING_API ScriptExecutionResult {
    bool        success = false;
    std::string stdout_text;
    std::string stderr_text;
    std::string exception_text;
};

// 预留给后续流式输出回调的最小输出事件结构
struct SCRIPTING_API ScriptOutputChunk {
    bool        is_stderr = false;
    std::string text;
};

} // namespace Moer::scripting
