#include "RenderSystem.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"

#include "RenderThread.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"

#include "platform/RenderPlatform.h"
#include "rhi/RHI.h"
// #include "config/ConfigManager.h"

#include <algorithm>

#include <stdexcept>
namespace Moer {
class RenderLoop {
    RenderLoop()  = default;
    ~RenderLoop() = default;
    //singleton
public:
    static RenderLoop& GetInstance() {
        static RenderLoop loop;
        return loop;
    }
    void Init();
    void Run();
    //quiting render loop
    void AfterLoop();

private:
};

RenderSystem::~RenderSystem() {}

void RenderSystem::Init() {
    //init rhi first
    InitRHI();

    InitShaderResources();

    StartRenderThread();
}
void RenderSystem::PostInit() {
    PostInitRHI();
    RenderLoop::GetInstance().Init();
}
//for dispatching task to render thread
void RenderSystem::Tick() {
    RenderLoop::GetInstance().Run();
}
void RenderSystem::ShutDown() {

    RenderLoop::GetInstance().AfterLoop();

    FreeShaderResources();

    ShutDownRHI();

    StopRenderThread();

    LOG_INFO("Render System Shut down.");
}

void RenderSystem::InitRHI() {
    //todo: init by config

    LOG_WARNING("RenderSystem needs to be refactored");
    // const auto& config_data = Moer::ConfigManager::GetInstance().GetConfig();
    // if (strcmp(config_data.engine.rhi.default_rhi.c_str(), RHI_VULKAN_NAME) == 0) {
    //     g_rhi = new VulkanRHIImpl();
    // } else {
    //     //throw unimplemented error
    //     throw std::runtime_error("RHI not implemented");
    // }
}

void RenderSystem::PostInitRHI() {
    RHIInitInfo info;

    // const auto& config_data = Moer::ConfigManager::GetInstance().GetConfig();

    // info.max_frame_in_flight = config_data.engine.rhi.max_frame_in_flight;
    // // info.ray_tracing         = config_data.ray_tracing;
    // g_rhi->Initialize(info);
    // g_rhi->PostInit();
}
void RenderSystem::InitShaderResources() {
    //init shader compiler
    ShaderCompiler::Init();
}

void RenderSystem::ShutDownRHI() {}

void RenderSystem::FreeShaderResources() {}

void RenderLoop::Init() {}

void RenderLoop::Run() {
    //dispatch render passes to render thread
}
void RenderLoop::AfterLoop() {}
} // namespace Moer