#include "Engine.h"
#include "log/LogSystem.h"

#include "Core.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include "RenderSystem.h"
#include "window/WindowContext.h"
namespace Moer {
    void Engine::Init(const EngineInitInfo& _info) {
        LOG_INFO("Engine Begin Initilization");

        InitCore();
        InitWindow();
        InitRenderSystem();

        LOG_INFO("Engine Initilization Finished");
    }

    void Engine::PostInit() {
        LOG_INFO("Engine Begin Post Init");

        LOG_INFO("Engine Post Init Finished");
    }

    void Engine::Run() {
        LOG_INFO("Engine Start Running");

        WindowContext& context = WindowContext::GetInstance();
        // while (!context.ShouldClose()) {
        //     ;
        //     ;
        // }
        LOG_INFO("Engine Stop Running");
    }

    void Engine::InitCore() {
        Moer::TaskSystem::Init();
    }
    void Engine::ShutDownCore() {
        Moer::TaskSystem::ShutDown();
    }

    void Engine::InitRenderSystem() {

        RenderSystem::Init();
    }
    void Engine::ShutDownRenderSystem() {
        RenderSystem::ShutDown();
    }

    void Engine::InitWindow() {
        //todo: get from config
        SurfaceInfo info{"", 1920, 1080, "MoerEditor", false};
        WindowContext::GetInstance().Init(info);
    }
    void Engine::ShutDownWindow() {
    }
    void Engine::Tick() {
    }

    void Engine::Quit() {
        b_request_quiting = true;
        ShutDownRenderSystem();
        ShutDownWindow();
        ShutDownCore();

        SPDLOG_INFO("Engine Quit");
    }
}// namespace Moer
