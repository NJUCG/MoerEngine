#pragma once

#include "remote/RemoteApi.h"
#include "remote/RemoteConfig.h"
#include "remote/RemoteRequestRecord.h"
#include "remote/service/RemoteScriptService.h"

#include <string>
#include <string_view>

namespace Moer::remote {

// 将执行脚本 HTTP 请求体解析为内部请求 DTO
REMOTE_API bool ParseExecuteScriptRequest(
    std::string_view            body,
    RemoteExecuteScriptRequest& request,
    std::string&                error_message
);

// 生成健康检查接口使用的 JSON 文本
REMOTE_API std::string MakeHealthzJson();

// 生成 Remote 状态接口使用的 JSON 文本
REMOTE_API std::string MakeRemoteStatusJson(const RemoteConfig& config, bool running);

// 生成执行脚本接口的 JSON 返回体
REMOTE_API std::string MakeExecuteScriptResponseJson(const RemoteExecuteScriptResponse& response);

// 生成请求记录查询接口的 JSON 返回体
REMOTE_API std::string MakeRequestRecordJson(const RemoteRequestRecord& record);

// 生成统一错误响应的 JSON 文本
REMOTE_API std::string MakeErrorJson(std::string_view message);

} // namespace Moer::remote