#include "Engine.h"

// Runtime
#include "config/ConfigManager.h"
#include "misc/ScopedLogTimer.h"
#include "remote/RemoteConfig.h"
#include "remote/RemoteModule.h"
#include "RenderThread.h"
#include "rhi/RHI.h"
#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptHost.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"


// Editor
#include "renderer/common/RuntimeAssets.h"
#include "renderer/raster/RasterRenderer.h"
#include "renderer/raytracing/RaytracingRenderer.h"

// 3rd party (std)
#include <cassert>
#include <nfd.hpp>
#include <type_traits>

// namespace
using namespace Moer::Render;

namespace Moer {

static UniquePtr<NFD::Guard> nfd_guard = nullptr;

static bool ContainsNonAscii(const std::filesystem::path& p);

namespace {

ERenderMethod ParseDefaultRenderMethod(std::string_view render_method_name) {
    if (render_method_name == "Raster") {
        return ERenderMethod::Raster;
    }
    if (render_method_name == "Raytracing") {
        return ERenderMethod::Raytracing;
    }

    LOG_WARNING("Invalid default render method: {}. Use Raster instead.", render_method_name);
    return ERenderMethod::Raster;
}

template<typename PrepareFunction, typename RenderFunction, typename ApplyFunction, typename StopFunction>
void RunBoundedRenderLoop(
    RenderThreadService& service,
    uint                 max_frame_lag,
    PrepareFunction      prepare_frame,
    RenderFunction       render_frame,
    ApplyFunction        apply_feedback,
    StopFunction         should_stop
) {
    using FramePacket = std::remove_cvref_t<std::invoke_result_t<PrepareFunction&>>;
    using Feedback = std::remove_cvref_t<std::invoke_result_t<RenderFunction&, FramePacket>>;

    BoundedRenderFrameQueue<Feedback> frame_queue(service, max_frame_lag);
    bool                              overlap_logged = false;
    auto retire_feedback = [&](Feedback feedback) {
        std::invoke(apply_feedback, std::move(feedback));
    };

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        frame_queue.RetireCompleted(retire_feedback);

        FramePacket frame_packet = std::invoke(prepare_frame);
        const uint64 frame_id     = frame_packet.frame_id;
        frame_queue.Submit(
            frame_id,
            [render_frame, frame_packet = std::move(frame_packet)]() mutable -> Feedback {
                assert(IsCurrentlyRenderThread());
                return std::invoke(render_frame, std::move(frame_packet));
            }
        );

        if (!overlap_logged && max_frame_lag > 0 && frame_queue.PendingFrameCount() > 1) {
            overlap_logged = true;
            LOG_INFO(
                "[Threading] GT/RT overlap active at frame {}; pending render frames={}; max_frame_lag={}.",
                frame_id,
                frame_queue.PendingFrameCount(),
                max_frame_lag
            );
        }

        frame_queue.EnforceLagLimit(retire_feedback);
        if (std::invoke(should_stop)) {
            break;
        }
    }

    frame_queue.Flush(retire_feedback);
    LOG_INFO("[Threading] Render frame queue drained before renderer shutdown/reload.");
}

} // namespace

Engine::Engine() {}

Engine::~Engine() {
    assert(m_has_shutdown && "Engine::ShutDown() was not called before Engine destruction!");
}

void Engine::Init(int argc, const char** argv) {
    ScopedLogTimer startup_timer("[Startup][Engine] Engine::Init total");

    // Init LogSystem
    LogSystem::Init(); // for LOG_DEBUG & LOG_TRACE when debug mode

    // Init ConfigManager
    std::filesystem::path path = argv[0];
    path = path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path;

    LOG_INFO("Workspace Path : {}", path.string());

    if (ContainsNonAscii(path)) {
        LOG_ERROR(
            "Workspace Path contains non-ASCII characters (e.g., Chinese characters)! This may cause "
            "unexpected issues. Current path: {}",
            path.string()
        );
    }

    ConfigManager::GetInstance().Init(path);
    const auto& config = ConfigManager::GetInstance().GetConfig();

    // Init TaskSystem
    TaskSystem::Init();

    LOG_INFO("[Threading] GameThread id = {}", GetGameThreadId());
    LOG_INFO(
        "[Threading] render_thread={}, rhi_thread={}, rhi_bypass={}, max_frame_lag={}",
        config.engine.threading.render_thread,
        config.engine.threading.rhi_thread,
        config.engine.threading.rhi_bypass,
        config.engine.threading.max_frame_lag
    );

    if (config.engine.threading.rhi_thread) {
        LOG_WARNING("[Threading] RHI thread mode is configured but not implemented; using synchronous RHI.");
    }

    if (config.engine.threading.render_thread) {
        m_max_frame_lag = std::min(config.engine.threading.max_frame_lag, uint{1});
        if (config.engine.threading.max_frame_lag > 1) {
            LOG_WARNING(
                "[Threading] max_frame_lag={} exceeds the validated range; clamping to 1.",
                config.engine.threading.max_frame_lag
            );
        }

        m_render_thread_service = MakeUnique<RenderThreadService>();
        m_render_thread_service->Start();
        LOG_INFO("[Threading] Effective Render Thread max_frame_lag={}", m_max_frame_lag);
    } else if (config.engine.threading.max_frame_lag != 0) {
        LOG_WARNING("[Threading] max_frame_lag is ignored while render_thread=false.");
    }

    // Init RenderDevice
    std::string rhi_type_str = config.engine.rhi.type;
    std::transform(rhi_type_str.begin(), rhi_type_str.end(), rhi_type_str.begin(), ::tolower);

    ERHIType rhi_type = [&]() {
        if (rhi_type_str == "vulkan") {
            LOG_INFO("Using Vulkan as RHI backend");
            return ERHIType::Vulkan;
        }
        if (rhi_type_str == "d3d12") {
            LOG_INFO("Using D3D12 as RHI backend");
            return ERHIType::D3D12;
        }

        LOG_WARNING("Unknown RHI type '{}', fallback to Vulkan", config.engine.rhi.type);
        return ERHIType::Vulkan;
    }();

    RenderDevice::Init(
        std::move(
            DeviceInitInfo{
                .rhi_type        = rhi_type,
                .name            = "MoerEngine",
                .rhi_api_version = config.engine.rhi.api_version,
            }
        )
    );

    ShaderManager::Get(); // Explicit Init ShaderManager

    m_editor_config = MakeShared<EditorConfig>();
    m_editor_config->selected_render_method =
        ParseDefaultRenderMethod(config.engine.render.default_render_method);
    m_editor_config->scene_path = config.engine.scene.scene_path;

    // Init WindowContext
    m_editor_config->SetResolution(config.editor.width, config.editor.height);
    bool b_fullscreen = config.editor.fullscreen;
    LOG_INFO(
        "Editor Window Resolution : {}x{}; Fullscreen : {}",
        m_editor_config->GetResolution().x,
        m_editor_config->GetResolution().y,
        b_fullscreen
    );

    WindowContext::Init(SurfaceInitInfo(
        RenderDevice::Get().GetRHIType(),
        m_editor_config->GetResolution().x,
        m_editor_config->GetResolution().y,
        "MoerEditor",
        b_fullscreen
    ));

    m_runtime_assets =
        MakeUnique<RuntimeAssets>(ConfigManager::GetInstance().GetEditorResourcePath(), RenderDevice::Get());
    m_runtime_assets->WaitUntilReady();
    LOG_INFO("[Threading] RuntimeAssets are immutable-ready before renderer/UI frame overlap begins.");

    m_script_host = MakeUnique<scripting::ScriptHost>(scripting::PythonRuntimeConfig::Default());
    m_script_host->Start();

    // 初始化 RemoteModule
    auto remote_config = remote::MakeRemoteConfigFromGlobalConfig(config);
    const bool remote_enabled_by_config = remote_config.enable;
    auto       submit_fn                = [this](scripting::ScriptExecutionRequest request) {
        return SubmitScriptExecution(std::move(request));
    };

    m_remote_module = MakeUnique<remote::RemoteModule>(std::move(remote_config), std::move(submit_fn));
    if (remote_enabled_by_config && !m_remote_module->SetEnabled(true)) {
        LOG_WARNING("Remote module failed to start. Continue running without remote access.");
    }
}

void Engine::Run(const EngineHooks& hooks) {
    EngineHooks runtime_hooks       = hooks;
    runtime_hooks.on_tick_scripting = [this](Scene& scene) {
        if (m_script_host) {
            m_script_host->ProcessMainThreadCommands(scene);
        }
    };

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        const ERenderMethod selected_render_method = m_editor_config->selected_render_method;

        LOG_INFO(
            "Selecting Render Method : {}",
            k_render_method_names[static_cast<uint>(selected_render_method)]
        );

        auto create_renderer = [this, runtime_hooks, selected_render_method]() {
            if (selected_render_method == ERenderMethod::Raster) {
                m_renderer = MakeUnique<Raster::RasterRenderer>(
                    m_editor_config->GetResolution(), m_editor_config
                );

            } else if (selected_render_method == ERenderMethod::Raytracing) {
                m_renderer = MakeUnique<Raytracing::RaytracingRenderer>(
                    m_editor_config->GetResolution(), m_editor_config, *m_runtime_assets
                );

            } else {
                assert(false && "Unknown render method");
            }
        };

        const bool use_render_thread = m_render_thread_service != nullptr;

        if (use_render_thread) {
            m_render_thread_service->RunAndWait(std::move(create_renderer));
            assert(m_renderer && m_renderer->SupportsSynchronizedRenderThread());

            if (runtime_hooks.on_show_config_sub_ui) {
                runtime_hooks.on_show_config_sub_ui();
            }

            if (selected_render_method == ERenderMethod::Raster) {
                auto* raster_renderer = static_cast<Raster::RasterRenderer*>(m_renderer.get());
                RunBoundedRenderLoop(
                    *m_render_thread_service,
                    m_max_frame_lag,
                    [this, raster_renderer, &runtime_hooks]() {
                        return raster_renderer->PrepareFrame(m_editor_config, runtime_hooks);
                    },
                    [raster_renderer](Raster::RasterFramePacket frame_packet) {
                        return raster_renderer->RenderFrame(std::move(frame_packet));
                    },
                    [this, raster_renderer, &runtime_hooks](Raster::RasterFrameFeedback feedback) {
                        raster_renderer->ApplyFrameFeedback(
                            std::move(feedback), m_editor_config->raster_config, runtime_hooks
                        );
                    },
                    [&runtime_hooks]() {
                        return runtime_hooks.on_is_need_reload && runtime_hooks.on_is_need_reload();
                    }
                );

            } else if (selected_render_method == ERenderMethod::Raytracing) {
                auto* raytracing_renderer = static_cast<Raytracing::RaytracingRenderer*>(m_renderer.get());
                RunBoundedRenderLoop(
                    *m_render_thread_service,
                    m_max_frame_lag,
                    [this, raytracing_renderer, &runtime_hooks]() {
                        return raytracing_renderer->PrepareFrame(m_editor_config, runtime_hooks);
                    },
                    [raytracing_renderer](Raytracing::RaytracingFramePacket frame_packet) {
                        return raytracing_renderer->RenderFrame(std::move(frame_packet));
                    },
                    [this, raytracing_renderer](Raytracing::RaytracingFrameFeedback feedback) {
                        raytracing_renderer->ApplyFrameFeedback(
                            std::move(feedback), m_editor_config->raytracing_config
                        );
                    },
                    [&runtime_hooks]() {
                        return runtime_hooks.on_is_need_reload && runtime_hooks.on_is_need_reload();
                    }
                );
                raytracing_renderer->Shutdown(runtime_hooks);

            } else {
                assert(false && "Unknown render method");
            }
        } else {
            create_renderer();
            if (runtime_hooks.on_show_config_sub_ui) {
                runtime_hooks.on_show_config_sub_ui();
            }
            if (selected_render_method == ERenderMethod::Raytracing) {
                auto* raytracing_renderer =
                    static_cast<Raytracing::RaytracingRenderer*>(m_renderer.get());
                while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
                    auto frame_packet =
                        raytracing_renderer->PrepareFrame(m_editor_config, runtime_hooks);
                    raytracing_renderer->ApplyFrameFeedback(
                        raytracing_renderer->RenderFrame(std::move(frame_packet)),
                        m_editor_config->raytracing_config
                    );

                    if (runtime_hooks.on_is_need_reload && runtime_hooks.on_is_need_reload()) {
                        break;
                    }
                }
                raytracing_renderer->Shutdown(runtime_hooks);
            } else {
                m_renderer->Run(m_editor_config, runtime_hooks);
            }
        }

        if (m_script_host) {
            m_script_host->CancelPendingSceneCommands(
                "Scene became unavailable during renderer switch or shutdown."
            );
        }

        // Switch Renderer
        if (use_render_thread) {
            m_render_thread_service->RunAndWait([this]() {
                LOG_INFO("[Threading] Destroying renderer on Render Thread.");
                m_renderer.reset();
                LOG_INFO("[Threading] Renderer destroyed on Render Thread.");
            });
        } else {
            m_renderer.reset();
        }
    }
}

void Engine::RequestExit() {
    WindowContext::RequestClose(WindowContext::GetMainWindow());
}

remote::RemoteModuleController Engine::GetRemoteModuleController() const {
    if (!m_remote_module) {
        return remote::RemoteModuleController();
    }

    return m_remote_module->GetController();
}

scripting::ScriptExecutionFuture Engine::SubmitScriptExecution(scripting::ScriptExecutionRequest request) {
    if (m_script_host) {
        return m_script_host->Submit(std::move(request));
    }

    std::promise<scripting::ScriptExecutionResult> promise;
    scripting::ScriptExecutionResult               result;
    result.exception_text = "ScriptHost is not available.";
    promise.set_value(std::move(result));
    return scripting::ScriptExecutionFuture(promise.get_future());
}

void Engine::ShutDown() {
    if (m_remote_module) {
        m_remote_module->Stop();
        m_remote_module.reset();
    }

    if (m_script_host) {
        m_script_host->CancelPendingSceneCommands("Scene became unavailable during engine shutdown.");
        m_script_host->Stop();
        m_script_host.reset();
    }

    if (m_render_thread_service) {
        LOG_INFO("[Threading] Stopping Render Thread service.");
        m_render_thread_service->Stop();
        m_render_thread_service.reset();
        LOG_INFO("[Threading] Render Thread service stopped.");
    }

    m_runtime_assets.reset(); // 释放RuntimeAssets资源

    WindowContext::ShutDown();
    ShaderManager::ShutDown();
    RenderDevice::Dispose();
    TaskSystem::ShutDown();

    m_has_shutdown = true;
}

void Engine::Init3rdParty() {
    nfd_guard = MakeUnique<NFD::Guard>();
}

void Engine::ShutDown3rdParty() {
    nfd_guard.release();
}

// 检测路径中是否包含非ASCII字符（包括中文）
bool ContainsNonAscii(const std::filesystem::path& p) {
    // std::filesystem::path 内部存储可能是 wchar_t (Windows) 或 char (其他平台)
    // 转换为 std::string (UTF-8) 或 std::wstring 进行检测更通用

    // 在Windows上，std::filesystem::path::string() 会根据当前 locale 转换为 narrow string
    // 但为了可靠检测非ASCII字符，最好是转换为宽字符串再检查，或者确保转换为UTF-8

    // 方法1：转换为 UTF-8 string 并检查 (更通用，但依赖std::codecvt_utf8_utf16)
    // std::string utf8_path_str = p.u8string(); // C++17，直接获取UTF-8编码
    // for (unsigned char c : utf8_path_str) {
    //     if (c > 127) { // 检查是否为非ASCII字符
    //         return true;
    //     }
    // }
    // return false;

    // 方法2：直接检查宽字符串 (更适合Windows，因为内部存储可能是宽字符)
    // 假设 std::filesystem::path 内部是 wchar_t 或可以转换为 wchar_t
    std::wstring wide_path_str = p.generic_wstring(); // 获取宽字符串表示

    for (wchar_t wc : wide_path_str) {
        // ASCII字符的 wchar_t 值范围是 0-127
        if (wc > 127) {
            return true; // 发现非ASCII字符
        }
    }
    return false;
}

} // namespace Moer
