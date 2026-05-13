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

} // namespace Moer::scripting
