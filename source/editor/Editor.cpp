#include "Editor.h"

// Runtime
#include "config/ConfigManager.h"
#include "misc/MMemory.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHI.h"
#include "shader/GeometryPassPsoManager.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/TaskSystem.h"
#include "window/WindowContext.h"

// Editor
#include "raster/RasterMain.h"
#include "raytracing/RaytracingMain.h"
#include "ui/EditorUI.h"

// 3rd party (std)
#include <cassert>
#include <nfd.hpp>

// namespace
using namespace Moer::Render;

namespace Moer {

static UniquePtr<NFD::Guard> nfd_guard = nullptr;

Editor::Editor() {}

Editor::~Editor() {}

void Editor::Init(int argc, const char** argv) {
    // Init LogSystem
    LogSystem::Init(); // for LOG_DEBUG & LOG_TRACE when debug mode

    // Init ConfigManager
    std::filesystem::path path = argv[0];
    ConfigManager::GetInstance().Init(
        path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path
    );

    // Init TaskSystem
    TaskSystem::Init();

    // Init RenderDevice
    RenderDevice::Init(std::move(DeviceInitInfo{
        .type            = ERHIType::Vulkan,
        .name            = "MoerEngine",
        .rhi             = ConfigManager::GetInstance().GetConfig().engine.rhi.rhi,
        .rhi_api_version = ConfigManager::GetInstance().GetConfig().engine.rhi.vulkan.api_version,
    }));

    ShaderManager::Get(); // Explicit Init ShaderManager

    // Init WindowContext
    uint2 resolution = {
        ConfigManager::GetInstance().GetConfig().editor.width,
        ConfigManager::GetInstance().GetConfig().editor.height
    };
    bool b_fullscreen = ConfigManager::GetInstance().GetConfig().editor.fullscreen;
    LOG_INFO("Editor Window Resolution : {}x{}; Fullscreen : {}", resolution.x, resolution.y, b_fullscreen);
    WindowContext::Init(SurfaceInitInfo("Vulkan", resolution.x, resolution.y, "MoerEditor", b_fullscreen));

    // Init EditorUI
    auto ui_renderer = MakeUnique<Render::UIRenderer>(RenderDevice::Get());

    m_editor_ui = MakeShared<EditorUI>(std::move(ui_renderer), resolution);
}

void Editor::Run() {
    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        const auto& config = m_editor_ui->GetConfig();

        LOG_INFO(
            "Selecting Render Method : {}",
            k_render_method_names[static_cast<uint>(config.selected_render_method)]
        );

        if (config.selected_render_method == ERenderMethod::Raster) {
            Render::Raster::RasterMain(m_editor_ui);

        } else if (config.selected_render_method == ERenderMethod::Raytracing) {
            Render::Raytracing::RaytracingMain(m_editor_ui);

        } else {
            assert(false && "Unknown render method");
        }
    }
}

void Editor::ShutDown() {
    GeometryPassPsoManager::ShutDown(); // 如果这个单例没有Get过，则ShutDown时不会消耗额外资源

    m_editor_ui.reset(); // 释放EditorUI资源

    WindowContext::ShutDown();
    ShaderManager::ShutDown();
    RenderDevice::Dispose();
    TaskSystem::ShutDown();
}

void Editor::Init3rdParty() { nfd_guard = MakeUnique<NFD::Guard>(); }

void Editor::ShutDown3rdParty() {
    nfd_guard.release();
    // TODO: check nfd_guard
}

} // namespace Moer