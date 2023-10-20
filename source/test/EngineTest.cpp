#include "Engine.h"
#include <filesystem>
#include "Core.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/GraphTask.h"
#include "log/LogSystem.h"
#include "RenderThread.h"
#define BEGIN_TEST(TestName)                          \
    LOG_INFO("===================================="); \
    LOG_INFO("Begin Test: {}", #TestName);            \
    LOG_INFO("====================================");

#define END_TEST(TestName)                            \
    LOG_INFO("===================================="); \
    LOG_INFO("End Test: {}", #TestName);              \
    LOG_INFO("====================================");

void RenderThreadSuspendTest(const Moer::Engine& engine) {
    BEGIN_TEST(RenderThreadSuspendTest)

    {
        FunctionGraphTask::ConstructAndDispatchWhenReady(
            [&engine]() {
                for (uint32_t i = 0; i < 300 && !engine.IsRequestQuiting(); i++) {

                    FunctionGraphTask::ConstructAndDispatchWhenReady(
                        []() {
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            LOG_WARNING("Render Thread Ticking");
                        },
                        nullptr,
                        EThread::ERenderThread);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            },
            nullptr,
            EThread::AnyThread_HighPri);

        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        Moer::ScopedResumeRenderThread scope_suspend;
        for (uint32_t i = 0; i < 5; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            LOG_WARNING("Main Thread Ticking");
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    END_TEST(RenderThreadSuspendTest)
}

void ShaderParameterSetTest() {
    BEGIN_TEST(ShaderParameterSetTest)
    TestReflectionShader::Parameters params{};
    Shader*                          shader = ShaderResourceManager::GetShader<TestReflectionShader>();
    RHIBatchedShaderParameters       batched_params;
    auto                             view_info = RHIViewInfo::CreateBufferSRVInfo();
    params.bar                                 = new RHIShaderResourceView(nullptr, view_info);
    batched_params.SetParameters(shader, params);
    END_TEST(ShaderParameterSetTest)
}
int main(int argc, const char** argv) {

    Moer::Engine         engine;
    Moer::EngineInitInfo info{std::filesystem::path(argv[0])};

    engine.Init(info);
    engine.PostInit();
    ShaderParameterSetTest();
    engine.Run();

    engine.Quit();
}