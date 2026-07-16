#include "subui/RemoteExamplesUI.h"

// Generates copy-ready HTTP, WebSocket, JSON, and Python examples for the active remote endpoint.

#include <imgui.h>

#include <string>
#include <string_view>

namespace Moer {

namespace {

// 将 0.0.0.0 这类监听地址转换成适合本机访问示例的 host
std::string BuildRemoteExampleHost(std::string_view bind_address) {
    if (bind_address.empty() || bind_address == "0.0.0.0") {
        return "127.0.0.1";
    }

    return std::string(bind_address);
}

// 转义字符串，生成合法的 JSON string 内容
std::string EscapeJsonString(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size() + 32);

    for (const char ch : text) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\r':
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped.push_back(ch);
                break;
        }
    }

    return escaped;
}

// 生成适合 Apifox 直接粘贴的 JSON payload
std::string BuildRemoteExecuteJsonPayload(std::string_view python_code) {
    return "{\n  \"code\": \"" + EscapeJsonString(python_code) + "\"\n}";
}

// 生成适合 PowerShell 直接执行的 curl.exe 示例，Python 脚本保持原始多行文本
std::string BuildRemoteExecuteExample(std::string_view execute_url, std::string_view python_code) {
    return "$code = @'\n" + std::string(python_code) +
           "\n'@\n$body = @{ code = $code } | ConvertTo-Json -Compress\ncurl.exe -X POST " +
           std::string(execute_url) + " -H \"Content-Type: application/json\" --data-raw $body";
}

} // namespace

// 绘制 Remote 示例窗口，并提供复制按钮
void RemoteExamplesUI::ShowWindow(const remote::RemoteModuleController& remote_controller) {
    if (!m_b_show) {
        return;
    }

    if (!remote_controller.IsEnabled()) {
        m_b_show = false;
        return;
    }

    const auto        remote_config = remote_controller.GetConfigSnapshot();
    const std::string remote_host   = BuildRemoteExampleHost(remote_config.bind_address);
    const std::string http_base_url =
        "http://" + remote_host + ":" + std::to_string(remote_config.http_port);
    const std::string websocket_events_url =
        "ws://" + remote_host + ":" + std::to_string(remote_config.websocket_port) + "/ws/events";
    const std::string healthz_url                  = http_base_url + "/healthz";
    const std::string status_url                   = http_base_url + "/api/remote/status";
    const std::string execute_url                  = http_base_url + "/api/script/execute";
    const std::string healthz_example              = "curl.exe " + healthz_url;
    const std::string status_example               = "curl.exe " + status_url;
    const std::string execute_python               = "print('hello remote')";
    const std::string execute_json_payload         = BuildRemoteExecuteJsonPayload(execute_python);
    const std::string execute_example              = BuildRemoteExecuteExample(execute_url, execute_python);
    const std::string named_transform_python       = R"py(import moer

TARGET_NAME = "RootNode"  # replace with a node name from your scene

scene = moer.scene()
target_entity = scene.find_node_entity_by_name(TARGET_NAME)
if not scene.is_valid_node_entity(target_entity):
    raise RuntimeError(f"node not found: {TARGET_NAME}")

target_translation = moer.float3(3.0, 1.25, -2.0)
# Quaternion(w, x, y, z), here it is a 45-degree yaw around the Y axis
target_rotation = moer.Quaternion(0.9238795, 0.0, 0.3826834, 0.0)

assert scene.set_node_translation(target_entity, target_translation), "set_node_translation failed"
assert scene.set_node_rotation(target_entity, target_rotation), "set_node_rotation failed"

print(f"updated {TARGET_NAME}, entity={target_entity}")
print(f"translation = {target_translation}")
print(f"rotation = {target_rotation}")
)py";
    const std::string named_transform_json_payload = BuildRemoteExecuteJsonPayload(named_transform_python);
    const std::string named_transform_example =
        BuildRemoteExecuteExample(execute_url, named_transform_python);
    const std::string timed_animation_python       = R"py(import math
import time
import moer

TARGET_NAME = "RootNode"  # replace with a visible node name from your scene
DURATION_SECONDS = 5.0
UPDATES_PER_SECOND = 120.0
X_AMPLITUDE = 1.0
Y_AMPLITUDE = 0.5
Z_AMPLITUDE = 0.25
BASE_HEIGHT = 1.0
YAW_DEGREES_PER_SECOND = 15.0

def make_y_axis_rotation(degrees):
    half_radians = math.radians(degrees) * 0.5
    return moer.Quaternion(math.cos(half_radians), 0.0, math.sin(half_radians), 0.0)

scene = moer.scene()
target_entity = scene.find_node_entity_by_name(TARGET_NAME)
if not scene.is_valid_node_entity(target_entity):
    raise RuntimeError(f"node not found: {TARGET_NAME}")

start_time = time.monotonic()
frame_index = 0

while True:
    elapsed = time.monotonic() - start_time
    if elapsed >= DURATION_SECONDS:
        break

    orbit_phase = elapsed * math.tau * 0.5
    bob_phase = elapsed * math.tau

    translation = moer.float3(
        math.cos(orbit_phase) * X_AMPLITUDE,
        BASE_HEIGHT + math.sin(bob_phase) * Y_AMPLITUDE,
        math.sin(orbit_phase) * Z_AMPLITUDE,
    )
    rotation = make_y_axis_rotation(elapsed * YAW_DEGREES_PER_SECOND)

    assert scene.set_node_translation(target_entity, translation), "set_node_translation failed"
    assert scene.set_node_rotation(target_entity, rotation), "set_node_rotation failed"

    if frame_index % 15 == 0:
        print(f"t={elapsed:.2f}s translation={translation}")

    frame_index += 1
    time.sleep(1.0 / UPDATES_PER_SECOND)

print(f"animation finished for {TARGET_NAME}, entity={target_entity}")
)py";
    const std::string timed_animation_json_payload = BuildRemoteExecuteJsonPayload(timed_animation_python);
    const std::string timed_animation_example =
        BuildRemoteExecuteExample(execute_url, timed_animation_python);
    const std::string websocket_example = "# example client endpoint\n" + websocket_events_url;

    const auto draw_remote_example_section = [](const char*        title,
                                                const std::string& url,
                                                const std::string& example,
                                                std::string_view   script_body    = {},
                                                std::string_view   json_payload   = {},
                                                float example_height = 72.0f) {
        ImGui::SeparatorText(title);
        ImGui::PushID(title);

        if (ImGui::Button("Copy URL")) {
            ImGui::SetClipboardText(url.c_str());
        }

        ImGui::SameLine();
        if (ImGui::Button("Copy Example")) {
            ImGui::SetClipboardText(example.c_str());
        }

        if (!script_body.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Copy Script")) {
                const std::string script_text(script_body);
                ImGui::SetClipboardText(script_text.c_str());
            }
        }

        if (!json_payload.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Copy Json")) {
                const std::string json_text(json_payload);
                ImGui::SetClipboardText(json_text.c_str());
            }
        }

        ImGui::TextWrapped("URL: %s", url.c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("Example:");
        ImGui::BeginChild(
            "Example", ImVec2(0.0f, example_height), true, ImGuiWindowFlags_HorizontalScrollbar
        );
        ImGui::TextUnformatted(example.c_str());
        ImGui::EndChild();

        ImGui::PopID();
    };

    ImGui::SetNextWindowSize(ImVec2(820.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Remote Examples", &m_b_show, ImGuiWindowFlags_NoDocking)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped("Remote request examples for the current local endpoint");
    ImGui::TextWrapped(
        "POST script examples provide PowerShell command, raw Python script, and JSON payload"
    );

    draw_remote_example_section("Health Check", healthz_url, healthz_example);
    draw_remote_example_section("Remote Status", status_url, status_example);
    draw_remote_example_section(
        "Execute Script", execute_url, execute_example, execute_python, execute_json_payload, 150.0f
    );
    draw_remote_example_section(
        "Execute Script: Move And Rotate Named Node",
        execute_url,
        named_transform_example,
        named_transform_python,
        named_transform_json_payload,
        300.0f
    );
    draw_remote_example_section(
        "Execute Script: Timed Sine Animation For Named Node",
        execute_url,
        timed_animation_example,
        timed_animation_python,
        timed_animation_json_payload,
        300.0f
    );
    draw_remote_example_section("WebSocket Events", websocket_events_url, websocket_example);

    ImGui::End();
}

} // namespace Moer
