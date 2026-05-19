#include "log/LogSystem.h"
#include "remote/RemoteConfig.h"
#include "remote/RemoteModule.h"
#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptHost.h"

#include "hv/requests.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

constexpr int  kHttpPort      = 18180;
constexpr int  kWebSocketPort = 18181;
constexpr auto kRetryDelay    = std::chrono::milliseconds(50);
constexpr int  kMaxAttempts   = 20;

Moer::scripting::PythonRuntimeConfig BuildRuntimeConfig(const char* argv0) {
    const std::filesystem::path executable_path = std::filesystem::path(argv0);
    const std::filesystem::path executable_dir  = executable_path.parent_path();

    Moer::scripting::PythonRuntimeConfig config;
    config.runtime_root = executable_dir;
    config.program_path = executable_path;
    config.stdlib_dir   = executable_dir / "Lib";
    config.dll_dir      = executable_dir / "DLLs";
    return config;
}

requests::Response PostWithRetry(const std::string& url, const std::string& body) {
    http_headers headers;
    headers["Content-Type"] = "application/json";

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto response = requests::post(url.c_str(), body, headers);
        if (response != nullptr) {
            return response;
        }

        std::this_thread::sleep_for(kRetryDelay);
    }

    return nullptr;
}

requests::Response GetWithRetry(const std::string& url) {
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        auto response = requests::get(url.c_str());
        if (response != nullptr) {
            return response;
        }

        std::this_thread::sleep_for(kRetryDelay);
    }

    return nullptr;
}

} // namespace

int main(int argc, const char** argv) {
    if (argc <= 0) {
        std::cerr << "Invalid argc." << std::endl;
        return 1;
    }

    Moer::LogSystem::Init();

    Moer::scripting::ScriptHost script_host(BuildRuntimeConfig(argv[0]));
    script_host.Start();

    Moer::remote::RemoteConfig config;
    config.enable         = false;
    config.bind_address   = "127.0.0.1";
    config.http_port      = kHttpPort;
    config.websocket_port = kWebSocketPort;

    Moer::remote::RemoteModule module(
        config, [&script_host](Moer::scripting::ScriptExecutionRequest request) {
            return script_host.Submit(std::move(request));
        }
    );

    if (!module.SetEnabled(true)) {
        std::cerr << "Failed to enable RemoteModule." << std::endl;
        script_host.Stop();
        return 2;
    }

    const std::string base_url = "http://127.0.0.1:" + std::to_string(kHttpPort);
    const std::string body = R"json({"code":"print('hello remote http')","session_policy":"Stateless"})json";
    auto              response = PostWithRetry(base_url + "/api/script/execute", body);
    if (response == nullptr) {
        std::cerr << "POST /api/script/execute failed." << std::endl;
        module.Stop();
        script_host.Stop();
        return 3;
    }

    if (response->status_code != 200) {
        std::cerr << "Unexpected execute status: " << response->status_code << ", body=" << response->body
                  << std::endl;
        module.Stop();
        script_host.Stop();
        return 4;
    }

    const auto json = nlohmann::json::parse(response->body);
    if (!json.value("success", false) || json.value("stdout_text", "") != "hello remote http\n" ||
        json.value("state", "") != "Completed") {
        std::cerr << "Unexpected execute response: " << response->body << std::endl;
        module.Stop();
        script_host.Stop();
        return 5;
    }

    const std::string request_id      = json.value("request_id", "");
    auto              record_response = GetWithRetry(base_url + "/api/script/requests/" + request_id);
    if (record_response == nullptr || record_response->status_code != 200) {
        std::cerr << "GET request record failed." << std::endl;
        module.Stop();
        script_host.Stop();
        return 6;
    }

    module.SetEnabled(false);
    script_host.Stop();

    std::cout << "TestRemoteHttpExecuteSmoke passed on " << base_url << std::endl;
    return 0;
}