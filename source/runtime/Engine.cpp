#include "Engine.h"

// Runtime
#include "config/ConfigManager.h"
#include "misc/MMemory.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHI.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"

// Editor
#include "renderer/common/RuntimeAssets.h"
#include "renderer/raster/RasterRenderer.h"

// 3rd party (std)
#include <cassert>
#include <nfd.hpp>

// namespace
using namespace Moer::Render;

namespace Moer {

static UniquePtr<NFD::Guard> nfd_guard = nullptr;

Engine::Engine() {}

Engine::~Engine() {}

void Engine::Init(int argc, const char** argv) {
    // Init LogSystem
    LogSystem::Init(); // for LOG_DEBUG & LOG_TRACE when debug mode

    // Init ConfigManager
    std::filesystem::path path = argv[0];
    path = path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path;

    ConfigManager::GetInstance().Init(path);

    // Init TaskSystem
    TaskSystem::Init();

    // Init RenderDevice
    std::string rhi_type_str = ConfigManager::GetInstance().GetConfig().engine.rhi.type;
    std::transform(rhi_type_str.begin(), rhi_type_str.end(), rhi_type_str.begin(), ::tolower);

    ERHIType rhi_type = [&]() {
        if (rhi_type_str == "vulkan") {
            LOG_INFO("Using Vulkan as RHI backend");
            return ERHIType::Vulkan;
        } else if (rhi_type_str == "d3d12") {
            LOG_INFO("Using D3D12 as RHI backend");
            return ERHIType::D3D12;
        } else {
            LOG_WARNING(
                "Unknown RHI type '{}', fallback to Vulkan",
                ConfigManager::GetInstance().GetConfig().engine.rhi.type
            );
            return ERHIType::Vulkan;
        }
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

    m_editor_config = MakeShared<EditorConfig>();

    // Init WindowContext
    m_editor_config->resolution = MakeShared<uint2>(
        ConfigManager::GetInstance().GetConfig().editor.width,
        ConfigManager::GetInstance().GetConfig().editor.height
    );
    bool b_fullscreen = ConfigManager::GetInstance().GetConfig().editor.fullscreen;
    LOG_INFO(
        "Editor Window Resolution : {}x{}; Fullscreen : {}",
        m_editor_config->resolution->x,
        m_editor_config->resolution->y,
        b_fullscreen
    );

    WindowContext::Init(SurfaceInitInfo(
        RenderDevice::Get().GetRHIType(),
        m_editor_config->resolution->x,
        m_editor_config->resolution->y,
        "MoerEditor",
        b_fullscreen
    ));

    m_runtime_assets =
        MakeUnique<RuntimeAssets>(ConfigManager::GetInstance().GetEditorResourcePath(), RenderDevice::Get());
}

void Engine::Run(const EngineHooks& hooks) {

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        LOG_INFO(
            "Selecting Render Method : {}",
            k_render_method_names[static_cast<uint>(m_editor_config->selected_render_method)]
        );

        if (m_editor_config->selected_render_method == ERenderMethod::Raster) {
            m_renderer = MakeUnique<Raster::RasterRenderer>(m_editor_config->resolution, m_editor_config);

        } else if (m_editor_config->selected_render_method == ERenderMethod::Raytracing) {
            // Render::Raytracing::RaytracingMain(m_editor_ui, *m_runtime_assets);

        } else {
            assert(false && "Unknown render method");
        }

        // MARK: Main Loop

        while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
            if (!m_renderer->Run(m_editor_config, hooks)) {
                break;
            }
        }

        // Switch Renderer
        m_renderer.reset();
    }
}

void Engine::ShutDown() {
    GeometryPassPsoManager::ShutDown(); // 如果这个单例没有Get过，则ShutDown时不会消耗额外资源

    m_runtime_assets.reset(); // 释放RuntimeAssets资源

    WindowContext::ShutDown();
    ShaderManager::ShutDown();
    RenderDevice::Dispose();
    TaskSystem::ShutDown();
}

void Engine::Init3rdParty() {
    nfd_guard = MakeUnique<NFD::Guard>();
}

void Engine::ShutDown3rdParty() {
    nfd_guard.release();
}

} // namespace Moer