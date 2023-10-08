#include "RenderSystem.h"
#include "log/LogSystem.h"
#include "shader/Shader.h"
#include "shader/ShaderCommon.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include "RenderThread.h"
namespace Moer {
    RenderSystem::~RenderSystem() {
    }

    void RenderSystem::Init() {
        StartRenderingThread();
    }
    //use in single thread mode
    void RenderSystem::Tick() {
    }
    void RenderSystem::ShutDown() {
        StopRenderingThread();
        ShutDownRenderThread();
        LOG_INFO("Render System Shut down.");
    }

}// namespace Moer