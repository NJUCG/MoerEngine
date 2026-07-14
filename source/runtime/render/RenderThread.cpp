#include "RenderThread.h"
#include "Core.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
#include <atomic>
#include <cstddef>
namespace Moer {

Runnable*            g_render_thread_runnable   = nullptr;
RunnableThread*      g_render_thread            = nullptr;
Event*               g_render_suspend_end_event = nullptr;
std::atomic_uint32_t g_render_thread_suspending;

void RenderThreadMain(Event* _is_bound_to_taskgraph_event) {
    TaskGraph::GetInterface().AttachToNameThread(EThread::ERenderThread);

    //barrier

    if (_is_bound_to_taskgraph_event) {
        _is_bound_to_taskgraph_event->Trigger();
    }
    assert(IsRenderThreadInitialized() && "render thread not set");
    LOG_INFO("[RENDER THREAD] thread id:{} executing", GetRenderThreadId());
    TaskGraph::GetInterface().ProcessThreadUntilReturn(EThread::ERenderThread);
    LOG_INFO("[RENDER THREAD] thread id:{} end", GetRenderThreadId());
}
class RenderThread : public Runnable {
public:
    Event* is_bound_to_taskgraph_event;

    RenderThread() {
        is_bound_to_taskgraph_event = EventPool::Get()->GetEvent(false);
    }

    virtual ~RenderThread() {
        EventPool::Get()->ReleaseEvent(is_bound_to_taskgraph_event);
    }

    virtual void Init() override {
        ThreadManager::SetRenderThreadID(Platform::GetCurrentThreadID());
    }
    virtual uint32_t Run() override {

        RenderThreadMain(is_bound_to_taskgraph_event);

        return 0;
    }
    virtual void Stop() override {};
    virtual void Exit() override {
        ThreadManager::SetRenderThreadID(0);
    };
    virtual ThreadIndex GetIndex() override {
        return EThread::ERenderThread;
    };

private:
};

void StartRenderThread() {
    assert(IsCurrentlyGameThread() && "Render thread must be started from the game thread.");
    if (IsRenderThreadRunning()) {
        LOG_WARNING("[Threading] Render thread is already running.");
        return;
    }

    g_render_thread_runnable = MoerNew(RenderThread)();

    g_render_thread = RunnableThread::Create(
        g_render_thread_runnable,
        ThreadAttributes{
            .affinity = Affinity::AnyOf(EThread::GetThreadIndex(EThread::ERenderThread), Affinity::All()),
            .name     = "RenderThread"
        }
    );

    static_cast<RenderThread*>(g_render_thread_runnable)->is_bound_to_taskgraph_event->Wait();
};

void StopRenderThread() {

    assert(IsCurrentlyGameThread() && "Render thread must be stopped from the game thread.");
    if (!IsRenderThreadRunning()) {
        LOG_WARNING("[Threading] Render thread is already stopped.");
        return;
    }

    LOG_INFO("Wait for Rendering Thread To Stop.");
    //wait for thread to finish executing
    // GraphEventRef return_task = GraphTask<ReturnGraphTask>::CreateTask(nullptr, EThread::EMainThread)
    //                                 .ConstructAndDispatchWhenReady(EThread::ERenderThread);
    auto return_event = GraphTask<ReturnGraphTask>::Create(EThread::ERenderThread).Dispatch();
    //not sure, game thread tasks should not be executing, because we are currently on Game Thread
    assert(
        !TaskGraph::GetInterface().IsThreadProcessingTask(EThread::EMainThread) &&
        "On Game Thread while Game Thread Tasks are being executing"
    );
    TaskGraph::GetInterface().WaitUntilTaskComplete(return_event, EThread::EMainThread);

    MoerDelete(g_render_thread);
    g_render_thread = nullptr;
    MoerDelete((RenderThread*)g_render_thread_runnable);
    g_render_thread_runnable = nullptr;

    LOG_INFO("Rendering Thread Stopped.");
};

void ShutDownRenderThread() {}

void RestartRenderThread() {
    LOG_INFO("Restarting Render Thread.");
    assert(
        IsGameThreadInitialized() && IsCurrentlyGameThread() &&
        "Render Thread Control is only allowed in Game Thread."
    );

    bool b_render_thread_not_shut_down = g_render_thread && g_render_thread_runnable;
    if (b_render_thread_not_shut_down) {
    }
}

void SuspendRenderThread(bool _restart_later) {
    assert(
        IsGameThreadInitialized() && IsCurrentlyGameThread() &&
        "Render Thread Control is only allowed in Game Thread."
    );
    LOG_INFO("Try Suspending Render Thread.");
    if (_restart_later) {
        StopRenderThread();
        g_render_thread_suspending++;

    } else {
        if (g_render_thread_suspending.load(std::memory_order_relaxed)) {
            GraphEventRef suspend_complete_event = LambdaTask::Dispatch(
                []() {
                    g_render_thread_suspending++;
                },
                EThread::ERenderThread
            );
            TaskGraph::GetInterface().WaitUntilTaskComplete(suspend_complete_event, EThread::EMainThread);
            //now all works on task render thread has finished
            //start a waiting task on render thread
            if (g_render_suspend_end_event == nullptr)
                g_render_suspend_end_event = EventPool::Get()->GetEvent();

            LambdaTask::Dispatch(
                []() {
                    LOG_INFO("Render Thread Suspending.");
                    g_render_suspend_end_event->Wait();
                    LOG_INFO("Render Thread End Suspending.");
                },
                EThread::ERenderThread
            );

        } else {
            //render thread already suspended
            g_render_thread_suspending++;
        }
    }
}

void ResumeRenderThread(bool _promised_restart_before) {
    if (_promised_restart_before) {
        LOG_INFO("Restarting Render Thread.");
        g_render_thread_suspending--;
        StartRenderThread();

    } else {
        if (g_render_thread_suspending.fetch_add(-1) == 1) {
            LOG_INFO("Trigger Render Thread End Suspending.");
            g_render_suspend_end_event->Trigger();
        }
    }
}

bool IsRenderThreadRunning() {
    return g_render_thread != nullptr && g_render_thread_runnable != nullptr;
}

RenderThreadService::~RenderThreadService() {
    assert(!running && "RenderThreadService::Stop() must be called before destruction.");
}

void RenderThreadService::Start() {
    if (running) {
        return;
    }

    StartRenderThread();
    running = IsRenderThreadRunning();
}

void RenderThreadService::Stop() {
    if (!running) {
        return;
    }

    Flush();
    StopRenderThread();
    running = false;
}

GraphEventRef RenderThreadService::Enqueue(std::function<void()> task) {
    assert(running && "Cannot enqueue work before the render thread starts.");
    assert(IsCurrentlyGameThread() && "RenderThreadService is submitted by the game thread.");
    assert(task && "Cannot enqueue an empty render task.");
    return LambdaTask::Dispatch(std::move(task), EThread::ERenderThread);
}

void RenderThreadService::Wait(const GraphEventRef& event) {
    assert(running && "Cannot wait after the render thread stops.");
    assert(IsCurrentlyGameThread() && "RenderThreadService is waited by the game thread.");
    TaskGraph::GetInterface().WaitUntilTaskComplete(event, EThread::EMainThread);
}

void RenderThreadService::RunAndWait(std::function<void()> task) {
    auto event = Enqueue(std::move(task));
    Wait(event);
}

void RenderThreadService::Flush() {
    if (!running) {
        return;
    }

    RunAndWait([]() {});
}

ScopedResumeRenderThread::ScopedResumeRenderThread() {
    SuspendRenderThread(true);
}

ScopedResumeRenderThread::~ScopedResumeRenderThread() {
    ResumeRenderThread(true);
}

void RenderThreadFence::BeginFence() {
    complete_event = GraphEvent::CreateGraphEvent();

    EnqueueRenderTask([temp_complete_event = complete_event]() {
        if (IsCurrentlyRenderThread()) {
            temp_complete_event->TryUnlockSubsequents();
        }
    });
}

bool RenderThreadFence::IsFenceComplete() {
    assert(Moer::IsCurrentlyGameThread());
    if (!complete_event.Get() || complete_event->IsComplete()) {
        complete_event = nullptr;
        return true;
    }
    return false;
}
void RenderThreadFence::Wait() {

    assert(Moer::IsCurrentlyGameThread());

    if (!IsFenceComplete()) {
        {
            EventRef event{};

            static uint32_t wait_recursive_counter = 0;

            wait_recursive_counter++;
            //already called
            if (wait_recursive_counter > 1) {
                //TODO: messages
            }
            GraphEventArray temp_events;
            temp_events.push_back(complete_event);

            // GraphTask<TriggerEventGraphTask>::CreateTask(&temp_events, EThread::EMainThread)
            //     .ConstructAndDispatchWhenReady(event, EThread::AnyThread_NormalPri);

            GraphTask<TriggerEventGraphTask>::Create(event, EThread::AnyThread_NormalPri)
                .Wait(std::move(temp_events))
                .Dispatch();
            event.Wait();
        }
    }
}

} // namespace Moer
