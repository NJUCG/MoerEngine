#include "Engine.h"
#include "PixelFormat.h"
#include "config/ConfigManager.h"

#include "log/LogSystem.h"

#include "Core.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include "RenderSystem.h"
#include "ui/UIBase.h"
#include "window/WindowContext.h"
#include "EngineLoop.h"

#include <filesystem>
#include <stdint.h>

#include "modules/resource/include/loader/gltf/Parser.h"

namespace Moer {

    void Engine::Init(const EngineInitInfo& _info) {
        LOG_INFO("Engine Begin Initilization");

        InitCore(_info.workspace_path);
        InitRenderSystem();
        InitWindow();

        LOG_INFO("Engine Initilization Finished");
    }

    void Engine::PostInit() {
        LOG_INFO("Engine Begin Post Init");
        PostInitRenderSystem();
        LOG_INFO("Engine Post Init Finished");

        
        //todo : in now we need to move scene file to bin/Debug/resource/scenes 
        auto sponza_scene_path = ConfigManager::GetInstance().GetEditorResourcePath() /"scenes/sponza/Sponza01.gltf";
        //之前用的场景的texture是ktx格式的 目前还不支持 所以要渲染带纹理的用这个场景 文件传到moerengine scenes仓库里了
        //todo config this
        sponza_scene_path = "E:/code/DirectX-Graphics-Samples/Samples/Desktop/D3D12Raytracing/src/D3D12RaytracingMiniEngineSample/Sponza/pbr/sponza2.gltf";
        Scene::SetDefaultScene(Resource::Gltf::Parser::LoadSceneFromFile(sponza_scene_path).release());
    }
    void Engine::Run() {
        LOG_INFO("Engine Start Running");

        EngineLoop& loop = EngineLoop::GetInstance();
        loop.Init();

        while (!loop.ShouldEndLoop()) {
            loop.Run();
        }
        loop.Quit();

        LOG_INFO("Engine Stop Running");
    }

    void Engine::InitCore(const std::filesystem::path& workspace_path) {
        Moer::ConfigManager::GetInstance().Init(workspace_path);
        Moer::TaskSystem::Init();
        Moer::LogSystem::Init();
    }
    void Engine::ShutDownCore() {
        Moer::TaskSystem::ShutDown();
    }

    void Engine::InitRenderSystem() {

        RenderSystem::Init();
    }
    void Engine::PostInitRenderSystem() {
        RenderSystem::PostInit();
    }
    void Engine::ShutDownRenderSystem() {
        RenderSystem::ShutDown();
    }

    void Engine::InitWindow() {
        SurfaceInitInfo info;
#if defined(EDITOR_MODE_ON)
        const auto& config_data = ConfigManager::GetInstance().GetInitConfig();
        info.width              = config_data.editor_width;
        info.height             = config_data.editor_height;
        info.title              = "MoerEditor";
        info.b_fullscreen       = config_data.editor_fullscreen;
        info.b_vsync            = config_data.editor_vsync;
#else
        //load application info

#endif

        WindowContext::Init(info);
    }
    void Engine::ShutDownWindow() {
        WindowContext::ShutDown();
    }

    void Engine::Quit() {
        b_request_quiting = true;
        ShutDownRenderSystem();
        ShutDownWindow();
        ShutDownCore();

        LOG_INFO("Engine Quit");
    }

    void Engine::RegisterOnDrawUI(std::function<void()> _func) {
        EngineLoop::GetInstance().RegisterOnDrawUI(_func);
    }
}// namespace Moer
