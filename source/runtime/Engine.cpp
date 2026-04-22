#include "Engine.h"

// Runtime
#include "config/ConfigManager.h"
#include "file/FileDialog.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"

// Editor
#include "../editor/EditorUI.h"
#include "../editor/console/ConsoleSystem.h"
#include "renderer/common/RuntimeAssets.h"
#include "renderer/raster/RasterRenderer.h"
#include "renderer/raytracing/RaytracingRenderer.h"

// 3rd party (std)
#include <algorithm>

// namespace
using namespace Moer::Render;

namespace Moer {

Engine::Engine() {}

Engine::~Engine() {
    MOER_ASSERT(has_shutdown, "Engine::ShutDown() must be called before Engine destruction");
}

void Engine::Init(const SharedPtr<EditorConfig>& editor_config, bool fullscreen) {
    // Init TaskSystem
    TaskSystem::Init();

    if (!FileDialog::Init()) {
        LOG_WARNING("Native file dialog is unavailable in editor runtime.");
    }

    // Init RenderDevice
    std::string rhi_type_str = ConfigManager::GetInstance().GetConfig().engine.rhi.type;
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

        LOG_WARNING(
            "Unknown RHI type '{}', fallback to Vulkan",
            ConfigManager::GetInstance().GetConfig().engine.rhi.type
        );
        return ERHIType::Vulkan;
    }();

    RenderDevice::Init(
        std::move(
            DeviceInitInfo{
                .rhi_type        = rhi_type,
                .name            = "MoerEngine",
                .rhi_api_version = ConfigManager::GetInstance().GetConfig().engine.rhi.api_version,
            }
        )
    );

    ShaderManager::Get(); // Explicit Init ShaderManager

    m_editor_config = editor_config;

    // Init WindowContext
    LOG_INFO(
        "Editor Window Resolution : {}x{}; Fullscreen : {}",
        m_editor_config->GetResolution().x,
        m_editor_config->GetResolution().y,
        fullscreen
    );

    WindowContext::Init(SurfaceInitInfo(
        RenderDevice::Get().GetRHIType(),
        m_editor_config->GetResolution().x,
        m_editor_config->GetResolution().y,
        "MoerEditor",
        fullscreen
    ));

    m_runtime_assets =
        MakeUnique<RuntimeAssets>(ConfigManager::GetInstance().GetEditorResourcePath(), RenderDevice::Get());

    m_editor_ui = MakeUnique<EditorUI>(MakeUnique<Render::UIRenderer>(RenderDevice::Get()), m_editor_config);
    m_console_system = MakeShared<ConsoleSystem>(m_editor_config, m_command_processor);
    m_editor_ui->RegisterOverlayFunc(
        "Console",
        [console = m_console_system]() {
            console->TickUI();
        }
    );
    m_editor_ui->BindConsoleWindowState(
        [console = m_console_system]() {
            return console->IsEditorConsoleOpen();
        },
        [console = m_console_system](bool open) {
            console->SetEditorConsoleOpen(open);
        }
    );
}

void Engine::Run() {
    const EngineHooks hooks{
        .on_tick_ui =
            [this]() {
                m_editor_ui->TickUI();
                m_command_processor.ProcessPending();
            },
        .on_render_gui =
            [this](CommandList& cmd_list, TextureRef output_image) {
                m_editor_ui->RenderGUI(cmd_list, output_image);
            },
        .on_present_windows =
            [this]() {
                m_editor_ui->PresentWindows();
            },
        .on_is_need_reload =
            [this]() {
                return m_editor_ui->IsNeedReload();
            },
        .on_ui_combine_pass =
            [this](
                UiCombinePass* ui_combine_pass,
                CommandList&   cmd_list,
                TextureView    input_color_texture,
                TextureView    input_ui_texture,
                TextureView    default_output_texture
            ) {
                auto scene_window_target = m_editor_ui->GetSceneWindowTarget();
                return ui_combine_pass->Process(
                    cmd_list,
                    scene_window_target.is_separate_window,
                    m_editor_ui->GetConfig()->GetResolution(),
                    m_editor_ui->GetSceneColorPos(),
                    m_editor_ui->GetSceneColorResolution(),
                    scene_window_target.frame_buffer,
                    input_color_texture,
                    input_ui_texture,
                    default_output_texture
                );
            },
        .on_register_renderer_config_section =
            [this](std::string renderer_name, std::string section_name, std::function<void(void)> draw_func) {
                m_editor_ui->RegisterRendererConfigSection(
                    std::move(renderer_name),
                    std::move(section_name),
                    std::move(draw_func)
                );
            },
        .on_unregister_renderer_config_section =
            [this](std::string renderer_name, std::string section_name) {
                m_editor_ui->UnregisterRendererConfigSection(
                    std::move(renderer_name),
                    std::move(section_name)
                );
            },
    };

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        LOG_INFO(
            "Selecting Render Method : {}",
            k_render_method_names[static_cast<uint>(m_editor_config->selected_render_method)]
        );

        if (m_editor_config->selected_render_method == ERenderMethod::Raster) {
            m_renderer = MakeUnique<Raster::RasterRenderer>(
                m_editor_config->GetResolution(), m_editor_config, hooks, *m_runtime_assets
            );

        } else if (m_editor_config->selected_render_method == ERenderMethod::Raytracing) {
            // Render::Raytracing::RaytracingMain(m_editor_ui, *m_runtime_assets);
            m_renderer = MakeUnique<Raytracing::RaytracingRenderer>(
                m_editor_config->GetResolution(), m_editor_config, hooks, *m_runtime_assets
            );

        } else {
            MOER_ASSERT(
                false,
                "Unknown render method: {}",
                static_cast<uint32_t>(m_editor_config->selected_render_method)
            );
        }

        m_renderer->Run(m_editor_config, hooks);

        // Switch Renderer
        m_renderer.reset();
    }
}

void Engine::ShutDown() {
    GeometryPassPsoManager::ShutDown(); // 如果这个单例没有Get过，则ShutDown时不会消耗额外资源

    m_renderer.reset();
    m_console_system.reset();
    m_editor_ui.reset();
    m_runtime_assets.reset(); // 释放RuntimeAssets资源

    FileDialog::ShutDown();
    WindowContext::ShutDown();
    ShaderManager::ShutDown();
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    RHIExecutor::ShutDown();
    RenderDevice::Dispose();
    TaskSystem::ShutDown();

    has_shutdown = true;
}

} // namespace Moer
