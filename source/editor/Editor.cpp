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
#include "common/EditorAssets.h"
#include "raster/RasterMain.h"
#include "raytracing/RaytracingMain.h"
#include "ui/EditorUI.h"

// 3rd party (std)
#include <cassert>
#include <nfd.hpp>

extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = MOER_AGILITY_SDK_VERSION;
}
extern "C" {
__declspec(dllexport) extern const char8_t* D3D12SDKPath = u8".\\D3D12\\";
}

// namespace
using namespace Moer::Render;

namespace Moer {

UniquePtr<GPUCapturer> capturer = nullptr;

static UniquePtr<NFD::Guard> nfd_guard = nullptr;

Editor::Editor() {}

Editor::~Editor() {}

void Editor::Init(int argc, const char** argv) {
    // Init LogSystem
    LogSystem::Init(); // for LOG_DEBUG & LOG_TRACE when debug mode

    // Init ConfigManager
    std::filesystem::path path = argv[0];
    path = path.filename().string().find(".exe") != std::string::npos ? path.parent_path() : path;

    ConfigManager::GetInstance().Init(path);

    // Init TaskSystem
    TaskSystem::Init();

    DeviceInitInfo info;
    if (ConfigManager::GetInstance().GetConfig().engine.rhi.rhi == "vulkan") {
        info = {
            .type            = ERHIType::Vulkan,
            .name            = "MoerEngine",
            .rhi             = ConfigManager::GetInstance().GetConfig().engine.rhi.rhi,
            .rhi_api_version = ConfigManager::GetInstance().GetConfig().engine.rhi.vulkan.api_version,
        };
    } else {
        info = {.type = ERHIType::D3D12, .name = "MoerEngine", .rhi = "d3d12"};
        if (ConfigManager::GetInstance().GetConfig().engine.rhi.d3d12.allow_pix_attach) {
            capturer = CreatePIXCapturer(); // load pix runtime dll
        }
    }

    // Init RenderDevice
    RenderDevice::Init(std::move(info));

    ShaderManager::Get(); // Explicit Init ShaderManager

    // Init WindowContext
    uint2 resolution = {
        ConfigManager::GetInstance().GetConfig().editor.width,
        ConfigManager::GetInstance().GetConfig().editor.height
    };
    bool b_fullscreen = ConfigManager::GetInstance().GetConfig().editor.fullscreen;
    LOG_INFO("Editor Window Resolution : {}x{}; Fullscreen : {}", resolution.x, resolution.y, b_fullscreen);

    if (ConfigManager::GetInstance().GetConfig().engine.rhi.rhi == "vulkan") {
        WindowContext::Init(SurfaceInitInfo("Vulkan", resolution.x, resolution.y, "MoerEditor", b_fullscreen)
        );
    } else {
        WindowContext::Init(SurfaceInitInfo("D3D12", resolution.x, resolution.y, "MoerEditor", b_fullscreen));
    }

    // Init EditorUI
    auto ui_renderer = MakeUnique<Render::UIRenderer>(RenderDevice::Get());

    m_editor_ui     = MakeShared<EditorUI>(std::move(ui_renderer), resolution);
    m_editor_assets = nullptr;
    // MakeUnique<EditorAssets>(ConfigManager::GetInstance().GetEditorResourcePath(), RenderDevice::Get());
}

void Editor::Run() {
    //capturer->Begin("D:\\codebase\\repos\\MoerEngine\\test.wpix");
    //while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
    const auto& config = m_editor_ui->GetConfig();

    LOG_INFO(
        "Selecting Render Method : {}",
        k_render_method_names[static_cast<uint>(config.selected_render_method)]
    );

    if (config.selected_render_method == ERenderMethod::Raster) {
        Render::Raster::RasterMain(m_editor_ui);

    } else if (config.selected_render_method == ERenderMethod::Raytracing) {
        // Render::Raytracing::RaytracingMain(m_editor_ui, *m_editor_assets);

    } else {
        assert(false && "Unknown render method");
    }
    //}
    //capturer->End();
}

void Editor::ShutDown() {
    GeometryPassPsoManager::ShutDown(); // 如果这个单例没有Get过，则ShutDown时不会消耗额外资源

    m_editor_ui.reset(); // 释放EditorUI资源
    // m_editor_assets.reset(); // 释放EditorAssets资源

    WindowContext::ShutDown();
    ShaderManager::ShutDown();
    RenderDevice::Dispose();
    TaskSystem::ShutDown();
}

void Editor::Init3rdParty() { nfd_guard = MakeUnique<NFD::Guard>(); }

void Editor::ShutDown3rdParty() { nfd_guard.release(); }

} // namespace Moer