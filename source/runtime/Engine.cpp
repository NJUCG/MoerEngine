#include "Engine.h"

// Runtime
#include "config/ConfigManager.h"
#include "misc/MMemory.h"
#include "renderer/common/UIRenderer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "shader/GeometryPassPsoManager.h"
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

// namespace
using namespace Moer::Render;

namespace Moer {

static UniquePtr<NFD::Guard> nfd_guard = nullptr;

static bool ContainsNonAscii(const std::filesystem::path& p);

Engine::Engine() {}

Engine::~Engine() {
    assert(has_shutdown && "Engine::ShutDown() was not called before Engine destruction!");
}

void Engine::Init(int argc, const char** argv) {
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

    // Init TaskSystem
    TaskSystem::Init();

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

    m_editor_config = MakeShared<EditorConfig>();

    // Init WindowContext
    m_editor_config->SetResolution(
        ConfigManager::GetInstance().GetConfig().editor.width,
        ConfigManager::GetInstance().GetConfig().editor.height
    );
    bool b_fullscreen = ConfigManager::GetInstance().GetConfig().editor.fullscreen;
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
}

void Engine::Run(const EngineHooks& hooks) {
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
            assert(false && "Unknown render method");
        }

        m_renderer->Run(m_editor_config, hooks);

        // Switch Renderer
        m_renderer.reset();
    }
}

void Engine::ShutDown() {
    GeometryPassPsoManager::ShutDown(); // 如果这个单例没有Get过，则ShutDown时不会消耗额外资源

    m_runtime_assets.reset(); // 释放RuntimeAssets资源

    WindowContext::ShutDown();
    ShaderManager::ShutDown();
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    RHIExecutor::ShutDown();
    RenderDevice::Dispose();
    TaskSystem::ShutDown();

    has_shutdown = true;
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
