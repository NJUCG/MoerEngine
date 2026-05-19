#pragma once

#include "remote/RemoteApi.h"
#include "scripting/ScriptExecutionFuture.h"
#include "scripting/ScriptExecutionRequest.h"

#include <functional>

namespace Moer::remote {

// 定义 Remote 模块提交脚本执行请求所依赖的回调签名
using RemoteSubmitScriptExecutionFn =
    std::function<scripting::ScriptExecutionFuture(scripting::ScriptExecutionRequest)>;

} // namespace Moer::remote