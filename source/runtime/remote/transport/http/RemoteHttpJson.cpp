#include "remote/transport/http/RemoteHttpJson.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <utility>

namespace Moer::remote {

namespace {

using Json = nlohmann::json;

scripting::EScriptExecutionKind ParseExecutionKind(const Json& json) {
    const std::string value = json.value("execution_kind", "Snippet");
    if (value == "Snippet" || value == "ExecSnippet") {
        return scripting::EScriptExecutionKind::ExecSnippet;
    }

    throw std::runtime_error("Unsupported execution_kind: " + value);
}

scripting::EScriptSessionPolicy ParseSessionPolicy(const Json& json) {
    const std::string value = json.value("session_policy", "Stateless");
    if (value == "SharedGlobal") {
        return scripting::EScriptSessionPolicy::SharedGlobal;
    }
    if (value == "NamedSession") {
        return scripting::EScriptSessionPolicy::NamedSession;
    }
    if (value == "Stateless") {
        return scripting::EScriptSessionPolicy::Stateless;
    }

    throw std::runtime_error("Unsupported session_policy: " + value);
}

scripting::EScriptRequestOrigin ParseOrigin(const Json& json) {
    const std::string value = json.value("origin", "Terminal");
    if (value == "EditorUiPanel") {
        return scripting::EScriptRequestOrigin::EditorUiPanel;
    }
    if (value == "Terminal") {
        return scripting::EScriptRequestOrigin::Terminal;
    }
    if (value == "Mcp") {
        return scripting::EScriptRequestOrigin::Mcp;
    }

    throw std::runtime_error("Unsupported origin: " + value);
}

Json ScriptResultToJson(const scripting::ScriptExecutionResult& result) {
    return Json{
        {"success", result.success},
        {"stdout_text", result.stdout_text},
        {"stderr_text", result.stderr_text},
        {"exception_text", result.exception_text},
    };
}

} // namespace

bool ParseExecuteScriptRequest(
    std::string_view            body,
    RemoteExecuteScriptRequest& request,
    std::string&                error_message
) {
    try {
        const Json json = Json::parse(body);
        if (!json.contains("code") || !json["code"].is_string()) {
            error_message = "Request field 'code' must be a string.";
            return false;
        }

        request.code           = json["code"].get<std::string>();
        request.source_name    = json.value("source_name", "remote/http");
        request.session_id     = json.value("session_id", "");
        request.execution_kind = ParseExecutionKind(json);
        request.session_policy = ParseSessionPolicy(json);
        request.origin         = ParseOrigin(json);
        return true;
    } catch (const std::exception& ex) {
        error_message = ex.what();
        return false;
    }
}

std::string MakeHealthzJson() {
    return Json{{"ok", true}, {"service", "remote"}}.dump();
}

std::string MakeRemoteStatusJson(const RemoteConfig& config, bool running) {
    return Json{
        {"enabled", config.enable},
        {"running", running},
        {"bind_address", config.bind_address},
        {"http_port", config.http_port},
        {"websocket_port", config.websocket_port},
    }
        .dump();
}

std::string MakeExecuteScriptResponseJson(const RemoteExecuteScriptResponse& response) {
    Json json          = ScriptResultToJson(response.result);
    json["request_id"] = response.request_id;
    json["state"]      = ToString(response.state);
    return json.dump();
}

std::string MakeRequestRecordJson(const RemoteRequestRecord& record) {
    Json json{
        {"request_id", record.request_id},
        {"state", ToString(record.state)},
        {"source_name", record.request.source_name},
        {"session_id", record.request.session_id},
    };

    if (record.result.has_value()) {
        json["result"] = ScriptResultToJson(*record.result);
    }

    return json.dump();
}

std::string MakeErrorJson(std::string_view message) {
    return Json{{"ok", false}, {"error", std::string(message)}}.dump();
}

} // namespace Moer::remote