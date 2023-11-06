#include "RenderSystem.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"

#include "rhi/RHIResource.h"
#include "rhi/vulkan/VulkanRHI.h"
#include "shader/Shader.h"
#include "shader/ShaderCompiler.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include "RenderThread.h"

#include "rhi/RHI.h"
#include "rhi/vulkan/IVulkanRHI.h"
namespace Moer {
    RenderSystem::~RenderSystem() {
    }

    void RenderSystem::Init() {
        //init rhi first
        InitRHI();

        InitShaderResources();

        StartRenderThread();
    }
    void RenderSystem::PostInit() {
        PostInitRHI();
    }
    //use in single thread mode
    void RenderSystem::Tick() {
    }
    void RenderSystem::ShutDown() {

        StopRenderThread();

        FreeShaderResources();

        ShutDownRHI();

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
        g_rhi = new VulkanRHIImpl();
    }

    void RenderSystem::PostInitRHI() {
        RHIInitInfo info;
        info.max_frame_in_flight = 3;
        g_rhi->Initialize(info);
        g_rhi->PostInit();
    }
    void RenderSystem::InitShaderResources() {
        //init shader compiler
        ShaderCompiler::Init();

        EShaderPlatform platform = GetShaderPlatformByRHIType(g_rhi->GetType());
        ShaderResourceManager::Init(platform);
        ShaderResourceManager::GetInstance().PrepareGlobalShaderResources();

        Shader* shader = ShaderResourceManager::GetShader<TestReflectionShader>();

        int i = 1;
    }

    void RenderSystem::ShutDownRHI() {
        g_rhi->ShutDown();
    }

    void RenderSystem::FreeShaderResources() {
        ShaderResourceManager::ShutDown();
    }

}// namespace Moer