#include "log/LogSystem.h"
#include "remote/RemoteConfig.h"
#include "remote/RemoteModule.h"
#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptHost.h"

#include "hv/WebSocketClient.h"
#include "hv/requests.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr int  kHttpPort      = 18182;
constexpr int  kWebSocketPort = 18183;
constexpr auto kWaitTimeout   = std::chrono::seconds(5);
constexpr auto kRetryDelay    = std::chrono::milliseconds(50);
constexpr int  kMaxAttempts   = 20;

struct TestState {
    bool        opened  = false;
    bool        done    = false;
    bool        success = false;
    std::string detail;
};

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

void Finish(
    TestState&               state,
    std::mutex&              mutex,
    std::condition_variable& cv,
    bool                     success,
    std::string              detail
) {
    std::lock_guard lock(mutex);
    if (state.done) {
        return;
    }

    state.done    = true;
    state.success = success;
    state.detail  = std::move(detail);
    cv.notify_one();
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

    std::mutex              mutex;
    std::condition_variable cv;
    TestState               state;

    hv::WebSocketClient client;
    client.onopen = [&]() {
        std::lock_guard lock(mutex);
        state.opened = true;
        cv.notify_one();
    };
    client.onmessage = [&](const std::string& msg) {
        try {
            const auto json    = nlohmann::json::parse(msg);
            const bool matches = json.value("type", "") == "script.completed" &&
                                 json.value("state", "") == "Completed" &&
                                 json.value("stdout_chunk", "") == "hello remote websocket\n";
            if (!matches) {
                Finish(state, mutex, cv, false, "unexpected websocket event: " + msg);
                client.close();
                return;
            }

            Finish(state, mutex, cv, true, "received expected event");
            client.close();
        } catch (const std::exception& ex) {
            Finish(state, mutex, cv, false, ex.what());
            client.close();
        }
    };
    client.onclose = [&]() {
        std::lock_guard lock(mutex);
        if (!state.done && !state.opened) {
            state.done    = true;
            state.success = false;
            state.detail  = "failed to open websocket";
            cv.notify_one();
        }
    };

    const std::string ws_url = "ws://127.0.0.1:" + std::to_string(kWebSocketPort) + "/ws/events";
    if (client.open(ws_url.c_str()) != 0) {
        std::cerr << "Failed to open websocket client." << std::endl;
        module.Stop();
        script_host.Stop();
        return 3;
    }

    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, kWaitTimeout, [&] {
                return state.opened || state.done;
            })) {
            state.done    = true;
            state.success = false;
            state.detail  = "timed out waiting for websocket open";
        }
    }

    if (!state.opened) {
        std::cerr << "WebSocket did not open: " << state.detail << std::endl;
        client.stop();
        module.Stop();
        script_host.Stop();
        return 4;
    }

    const std::string base_url = "http://127.0.0.1:" + std::to_string(kHttpPort);
    const std::string body =
        R"json({"code":"print('hello remote websocket')","session_policy":"Stateless"})json";
    auto response = PostWithRetry(base_url + "/api/script/execute", body);
    if (response == nullptr || response->status_code != 200) {
        std::cerr << "POST /api/script/execute failed." << std::endl;
        client.stop();
        module.Stop();
        script_host.Stop();
        return 5;
    }

    {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, kWaitTimeout, [&] {
                return state.done;
            })) {
            state.done    = true;
            state.success = false;
            state.detail  = "timed out waiting for websocket event";
        }
    }

    client.stop();
    module.SetEnabled(false);
    script_host.Stop();

    if (!state.success) {
        std::cerr << "TestRemoteWebSocketEventSmoke failed: " << state.detail << std::endl;
        return 6;
    }

    std::cout << "TestRemoteWebSocketEventSmoke passed on " << ws_url << std::endl;
    return 0;
}