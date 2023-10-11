#include "Engine.h"
#include "log/LogSystem.h"

#include "Core.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include "RenderSystem.h"
namespace Moer {
    void Engine::Init(const EngineInitInfo& _info) {
        LOG_INFO("Engine Begin Initilization");

        InitCore();
        InitRenderSystem();

        LOG_INFO("Engine Initilization Finished");
    }

    void Engine::PostInit() {
        LOG_INFO("Engine Begin Post Init");

        LOG_INFO("Engine Post Init Finished");
    }

    void Engine::Run() {
        LOG_INFO("Engine Start Running");
        for (;;) {
            //todo: currently not functions yet
            // Tick();
            break;
        }
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
    void Engine::Tick() {
    }

    void Engine::Quit() {
        b_request_quiting = true;
        RenderSystem::ShutDown();
        ShutDownCore();

        SPDLOG_INFO("Engine Quit");
    }
}// namespace Moer
