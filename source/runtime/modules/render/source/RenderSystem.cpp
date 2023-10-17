#include "RenderSystem.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "shader/Shader.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include "RenderThread.h"
#include <chrono>
#include <ratio>
#include <thread>
#include "rhi/RHI.h"
namespace Moer {
    RenderSystem::~RenderSystem() {
    }

    void RenderSystem::Init() {

        InitShaderLibrary();

        //
        StartRenderThread();
    }
    //use in single thread mode
    void RenderSystem::Tick() {
    }
    void RenderSystem::ShutDown() {
        StopRenderThread();
        LOG_INFO("Render System Shut down.");
    }

    void RenderSystem::InitShaderLibrary() {
        ShaderCompiler::Init();
        ShaderTypeRegistration::SubmitRegistrations();

        // auto& map = ShaderMetaType::GetNameToTypeMap();

        // RHI::Test();
        int i = 1;
    }

}// namespace Moer