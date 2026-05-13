#pragma once

#include "scripting/ScriptingApi.h"

#include <string>

namespace Moer::scripting {

// 描述一次脚本执行请求的输入内容
struct SCRIPTING_API ScriptExecutionRequest {
    std::string source_name = "<script>";
    std::string code;
};

} // namespace Moer::scripting
