#include "Core.h"
#include "Engine.h"
#include "RenderThread.h"
#include "log/LogSystem.h"
#include "math/Matrix.h"
#include "rhi/RHIResource.h"
#include "shader/Shader.h"
#include "shader/ShaderResourceManager.h"
#include "taskgraph/GraphTask.h"
#include <filesystem>
#include <vcruntime_string.h>
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
        LambdaTask::Dispatch([&engine] {
            for (uint32_t i = 0; i < 300 && !engine.IsRequestQuiting(); i++) {

                // FunctionGraphTask::ConstructAndDispatchWhenReady(
                //     []() {
                //         std::this_thread::sleep_for(std::chrono::milliseconds(100));
                //         LOG_WARNING("Render Thread Ticking");
                //     },
                //     nullptr,
                //     EThread::ERenderThread);

                LambdaTask::Dispatch(
                    []() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        LOG_WARNING("Render Thread Ticking");
                    },
                    EThread::ERenderThread
                );
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
        // FunctionGraphTask::ConstructAndDispatchWhenReady(
        //     [&engine]() {
        //         for (uint32_t i = 0; i < 300 && !engine.IsRequestQuiting(); i++) {

        //             FunctionGraphTask::ConstructAndDispatchWhenReady(
        //                 []() {
        //                     std::this_thread::sleep_for(std::chrono::milliseconds(100));
        //                     LOG_WARNING("Render Thread Ticking");
        //                 },
        //                 nullptr,
        //                 EThread::ERenderThread);
        //             std::this_thread::sleep_for(std::chrono::milliseconds(200));
        //         }
        //     },
        //     nullptr,
        //     EThread::AnyThread_HighPri);

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

// void ShaderParameterSetTest() {
//     BEGIN_TEST(ShaderParameterSetTest)
//     TestReflectionShader::Parameters params{};
//     Shader*                          shader = ShaderResourceManager::GetShader<TestReflectionShader>();
//     RHIBatchedShaderParameters       batched_params;
//     auto                             view_info = RHIViewInfo::CreateBufferSRVInfo();
//     params.bar                                 = new RHIShaderResourceView(nullptr, view_info);
//     params.ubo.viewMatrix                      = Moer::Matrix4x4f::Identity();
//     Moer::Matrix4x4f param_test;
//
//     batched_params.SetParameters(shader, params);
//
//     END_TEST(ShaderParameterSetTest)
// }
//todo config this
std::filesystem::path GetValidEnginePath(const std::filesystem::path& path) {
    if (path.filename().string().ends_with("exe"))
        return path.parent_path();
    return path;
}

int main(int argc, const char** argv) {

    Moer::Engine         engine;
    Moer::EngineInitInfo info{GetValidEnginePath(argv[0])};

    engine.Init(info);
    engine.PostInit();
    //ShaderParameterSetTest();
    engine.Run();

    engine.Quit();
}