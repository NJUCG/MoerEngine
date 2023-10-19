#include "RenderSystem.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHICommon.h"
#include "rhi/vulkan/IVulkanRHI.h"
#include "shader/Shader.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"
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
        //init rhi first

        InitShaderResources();

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
    class FakeRHI : public IVulkanRHI {
    public:
        FakeRHI() {
            rhi_type = ERHIType::Vulkan;
        }
    };
    void RenderSystem::InitRHI() {
        //todo: init by config
        g_rhi = new FakeRHI();
    }
    void RenderSystem::InitShaderResources() {
        //init shader compiler
        ShaderCompiler::Init();

        EShaderPlatform platform = GetShaderPlatformByRHIType(g_rhi->GetType());
        ShaderResourceManager::Init(platform);
        ShaderResourceManager::GetInstance().PrepareGlobalShaderResources();

        ShaderResourceManager::GetInstance().GetShaderTypeMap(platform);
        int i = 1;
    }

}// namespace Moer