#include "Engine.h"

#include "config/ConfigManager.h"
#include "scene/Scene.h"

#include "hv/requests.h"
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto             k_poll_delay           = std::chrono::milliseconds(50);
constexpr auto             k_scene_ready_timeout  = std::chrono::seconds(30);
constexpr auto             k_before_execute_delay = std::chrono::seconds(5);
constexpr auto             k_healthz_timeout      = std::chrono::seconds(10);
constexpr auto             k_validation_timeout   = std::chrono::seconds(5);
constexpr float            k_float_epsilon        = 1e-4f;
constexpr std::string_view k_expected_name        = "RemoteLoopbackRoot";
const Moer::float3         k_expected_translation(4.0f, 5.0f, 6.0f);

struct TestState {
    std::atomic<bool> scene_ready           = false;
    std::atomic<bool> client_done           = false;
    std::atomic<bool> client_success        = false;
    bool              exit_requested        = false;
    int               exit_code             = 3;
    Clock::time_point validation_start_time = {};
    std::mutex        mutex;
    std::string       request_id;
    std::string       error_message;
};

std::filesystem::path ResolveExecutableDir(const char* argv0) {
    std::filesystem::path path = argv0;
    return path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path;
}

bool ReadTextFile(const std::filesystem::path& file_path, std::string& out_text) {
    std::ifstream stream(file_path, std::ios::in | std::ios::binary);
    if (!stream.is_open()) {
        return false;
    }

    out_text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

void FinishClient(TestState& state, bool success, std::string error_message, std::string request_id = "") {
    {
        std::lock_guard lock(state.mutex);
        state.request_id    = std::move(request_id);
        state.error_message = std::move(error_message);
    }
    state.client_success = success;
    state.client_done    = true;
}

bool WaitForFlag(const std::atomic<bool>& flag, Clock::duration timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (flag.load()) {
            return true;
        }

        std::this_thread::sleep_for(k_poll_delay);
    }

    return flag.load();
}

void WaitWithCountdown(const char* prefix, Clock::duration duration) {
    const auto start_time   = Clock::now();
    int        last_seconds = -1;

    while (true) {
        const auto elapsed = Clock::now() - start_time;
        if (elapsed >= duration) {
            break;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(duration - elapsed);
        const int  seconds   = static_cast<int>(remaining.count()) + 1;
        if (seconds != last_seconds) {
            std::cout << prefix << seconds << "s" << std::endl;
            last_seconds = seconds;
        }

        std::this_thread::sleep_for(k_poll_delay);
    }
}

void PrintClientReceived(const char* channel, const std::string& text) {
    size_t line_begin = 0;
    while (line_begin < text.size()) {
        const size_t line_end = text.find('\n', line_begin);
        const size_t line_size =
            line_end == std::string::npos ? text.size() - line_begin : line_end - line_begin;

        std::cout << "[client received] " << channel;
        if (line_size > 0) {
            std::cout << ": " << text.substr(line_begin, line_size);
        }
        std::cout << std::endl;

        if (line_end == std::string::npos) {
            return;
        }

        line_begin = line_end + 1;
    }
}

requests::Response GetWithRetry(const std::string& url, Clock::duration timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        auto response = requests::get(url.c_str());
        if (response != nullptr) {
            return response;
        }

        std::this_thread::sleep_for(k_poll_delay);
    }

    return nullptr;
}

requests::Response PostWithRetry(const std::string& url, const std::string& body, Clock::duration timeout) {
    http_headers headers;
    headers["Content-Type"] = "application/json";

    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        auto response = requests::post(url.c_str(), body, headers);
        if (response != nullptr) {
            return response;
        }

        std::this_thread::sleep_for(k_poll_delay);
    }

    return nullptr;
}

std::string MakeRemoteBaseUrl() {
    const auto& remote_config = Moer::ConfigManager::GetInstance().GetConfig().engine.remote;
    std::string host          = remote_config.bind_address;
    if (host.empty() || host == "0.0.0.0") {
        host = "127.0.0.1";
    }

    return "http://" + host + ":" + std::to_string(remote_config.http_port);
}

std::string MakeDemoScriptJsonBody(const std::string& script_code) {
    const nlohmann::json json{
        {"source_name", "remote-engine-loopback-demo.py"},
        {"session_policy", "Stateless"},
        {"origin", "Terminal"},
        {"code", script_code},
    };
    return json.dump();
}

bool NearlyEqual(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= k_float_epsilon;
}

bool ValidateSceneState(Moer::Scene& scene, std::string& error_message) {
    const entt::entity root = scene.GetRootNodeEntity();
    if (!scene.IsValidNodeEntity(root)) {
        error_message = "root node is invalid";
        return false;
    }

    if (scene.GetNodeDisplayName(root) != k_expected_name) {
        error_message = "root node name was not updated by remote script";
        return false;
    }

    const auto transform = scene.TryGetNodeLocalTransform(root);
    if (!transform.has_value()) {
        error_message = "root local transform is unavailable";
        return false;
    }

    if (!NearlyEqual(transform->translation.x, k_expected_translation.x) ||
        !NearlyEqual(transform->translation.y, k_expected_translation.y) ||
        !NearlyEqual(transform->translation.z, k_expected_translation.z)) {
        error_message = "root translation was not updated by remote script";
        return false;
    }

    return true;
}
} // namespace

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Remote Engine Loopback Demo Starting..." << std::endl;

    Moer::Engine                engine;
    TestState                   state;
    const std::filesystem::path executable_dir = ResolveExecutableDir(argv[0]);
    const std::filesystem::path script_path    = executable_dir / "test" / "remote_engine_loopback_demo.py";

    engine.Init(argc, argv);
    engine.GetEditorConfig()->selected_render_method = Moer::ERenderMethod::Raster;

    if (!engine.SetRemoteEnabled(true)) {
        std::cerr << "Failed to enable RemoteModule for loopback demo." << std::endl;
        engine.ShutDown();
        return 2;
    }

    const std::string base_url = MakeRemoteBaseUrl();
    std::thread       client_thread([&]() {
        try {
            std::cout << "[client] waiting for scene ready before remote execution" << std::endl;
            if (!WaitForFlag(state.scene_ready, k_scene_ready_timeout)) {
                FinishClient(state, false, "timed out waiting for scene ready");
                return;
            }

            std::cout << "[client] scene ready, delaying remote execution for 5 seconds" << std::endl;
            WaitWithCountdown("[client] remote execute countdown: ", k_before_execute_delay);

            std::cout << "[client] checking remote /healthz at " << base_url << std::endl;
            auto healthz_response = GetWithRetry(base_url + "/healthz", k_healthz_timeout);
            if (healthz_response == nullptr || healthz_response->status_code != 200) {
                FinishClient(state, false, "remote /healthz is not reachable");
                return;
            }

            std::cout << "[client] loading python script from: " << script_path << std::endl;
            std::string script_code;
            if (!ReadTextFile(script_path, script_code)) {
                FinishClient(state, false, "failed to read python script file: " + script_path.string());
                return;
            }

            std::cout << "[client] POST /api/script/execute" << std::endl;
            auto execute_response = PostWithRetry(
                base_url + "/api/script/execute", MakeDemoScriptJsonBody(script_code), k_healthz_timeout
            );
            if (execute_response == nullptr) {
                FinishClient(state, false, "POST /api/script/execute failed");
                return;
            }

            if (execute_response->status_code != 200) {
                FinishClient(
                    state,
                    false,
                    "execute status=" + std::to_string(execute_response->status_code) +
                        ", body=" + execute_response->body
                );
                return;
            }

            const auto execute_json = nlohmann::json::parse(execute_response->body);
            if (!execute_json.value("success", false) || execute_json.value("state", "") != "Completed") {
                FinishClient(
                    state, false, "remote execute response is not successful: " + execute_response->body
                );
                return;
            }

            const std::string stdout_text    = execute_json.value("stdout_text", std::string());
            const std::string stderr_text    = execute_json.value("stderr_text", std::string());
            const std::string exception_text = execute_json.value("exception_text", std::string());

            if (!stdout_text.empty()) {
                PrintClientReceived("stdout", stdout_text);
            }
            if (!stderr_text.empty()) {
                PrintClientReceived("stderr", stderr_text);
            }
            if (!exception_text.empty()) {
                PrintClientReceived("exception", exception_text);
            }

            const std::string request_id = execute_json.value("request_id", "");
            if (request_id.empty()) {
                FinishClient(state, false, "remote execute response did not contain request_id");
                return;
            }

            std::cout << "[client] request accepted. request_id=" << request_id << std::endl;

            auto record_response =
                GetWithRetry(base_url + "/api/script/requests/" + request_id, k_healthz_timeout);
            if (record_response == nullptr || record_response->status_code != 200) {
                FinishClient(state, false, "GET /api/script/requests/:request_id failed", request_id);
                return;
            }

            std::cout << "[client] request record fetched successfully" << std::endl;

            FinishClient(state, true, "", request_id);
        } catch (const std::exception& ex) {
            FinishClient(state, false, ex.what());
        }
    });

    engine.Run(Moer::Render::EngineHooks{.on_tick_test = [&](Moer::Scene& scene) {
        if (state.exit_requested) {
            return;
        }

        if (scene.IsReady()) {
            static bool has_printed_scene_ready = false;
            if (!has_printed_scene_ready) {
                std::cout << "[engine] scene is ready, remote client may start soon" << std::endl;
                has_printed_scene_ready = true;
            }
            state.scene_ready = true;
        }

        if (!state.client_done.load()) {
            return;
        }

        if (!state.client_success.load()) {
            std::string error_message;
            {
                std::lock_guard lock(state.mutex);
                error_message = state.error_message;
            }

            std::cerr << "Remote client step failed: " << error_message << std::endl;
            state.exit_code      = 1;
            state.exit_requested = true;
            engine.RequestExit();
            return;
        }

        if (state.validation_start_time == Clock::time_point{}) {
            state.validation_start_time = Clock::now();
        }

        std::string validation_error;
        if (ValidateSceneState(scene, validation_error)) {
            std::string request_id;
            {
                std::lock_guard lock(state.mutex);
                request_id = state.request_id;
            }

            std::cout << "[engine] scene validation passed after remote python execution" << std::endl;
            std::cout << "Remote loopback demo passed. request_id=" << request_id << std::endl;
            state.exit_code      = 0;
            state.exit_requested = true;
            engine.RequestExit();
            return;
        }

        if (Clock::now() - state.validation_start_time < k_validation_timeout) {
            return;
        }

        std::cerr << "Scene validation failed after remote script: " << validation_error << std::endl;
        state.exit_code      = 2;
        state.exit_requested = true;
        engine.RequestExit();
    }});

    if (client_thread.joinable()) {
        client_thread.join();
    }

    engine.ShutDown();
    return state.exit_code;
}