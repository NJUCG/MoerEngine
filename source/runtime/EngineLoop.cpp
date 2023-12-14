// FILEPATH: /e:/GitHub/MoerEngine/source/runtime/EngineLoop.cpp

#include "EngineLoop.h"
#include "PixelFormat.h"
#include "RendererManager.h"
#include "renderer/UIRenderer.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "window/WindowContext.h"
#include "rhi/RHI.h"
#include "rhi/RHICommandQueue.h"
#include "rhi/RHICommandList.h"

#include "RenderThread.h"

#define ENGINE_LOOP_NEW(target)    new target()
#define ENGINE_LOOP_DELETE(target) delete target
namespace Moer {

    FrameEndSync::FrameEndSync() : event_index(0) {
    }
    FrameEndSync::~FrameEndSync() {
        Cleanup();
    }

    void FrameEndSync::Sync(bool b_allow_one_frame_lag) {
        fence[event_index].BeginFence();
        if (b_allow_one_frame_lag) {
            event_index = (event_index + 1) % 2;
        }
        fence[event_index].Wait();
    }

    void FrameEndSync::Cleanup() {
        fence[0].Wait();
        fence[1].Wait();
    }
    struct EngineLoopData {

        uint64_t FrameIndex() const { return frame_index; }
        uint64_t IncrementFrameIndex() { return ++frame_index; }

    private:
        uint64_t frame_index = 0;
    };

    void EngineLoop::Run() {
        RendererManager::GetInstance().DrawFrame();

        ProcessInputEvents();
        UIRenderer::GetRenderer()->BeginRenderFrame();

        for (auto& func : on_draw_ui_funcs) {
            func();
        }

        //record and present gui render command on main thread
        UIRenderer::GetRenderer()->EndRenderFrame();

        frame_end_sync.Sync(true);
    }
    void EngineLoop::Init() {
        UIRenderer::GetRenderer()->Init();
        RendererManager::GetInstance().Init();
    }

    void EngineLoop::ProcessInputEvents() {
        //window io tick
        WindowContext::Tick();
    }

    bool EngineLoop::ShouldEndLoop() {
        // implementation of ShouldQuit method
        return WindowContext::ShouldClose(WindowContext::GetMainWindow());
    }

    void EngineLoop::Quit() {
        // implementation of AfterLoop method
        UIRenderer::GetRenderer()->ShutDown();

        ENGINE_LOOP_DELETE(data);
    }

    void EngineLoop::RegisterOnDrawUI(std::function<void()> _func) {
        // implementation of RegisterOnDrawUI method
        on_draw_ui_funcs.emplace_back(_func);
    }
}// namespace Moer