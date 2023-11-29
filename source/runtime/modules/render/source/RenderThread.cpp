#include "RenderThread.h"
#include "Core.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/GraphTask.h"
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
            int i                       = 1;
            //todo: flush
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
        virtual void Stop() override{};
        virtual void Exit() override {
            ThreadManager::SetRenderThreadID(0);
        };
        virtual ThreadIndex GetIndex() override { return EThread::ERenderThread; };

    private:
    };

    void StartRenderThread() {
        g_render_thread_runnable = new RenderThread();

        g_render_thread = RunnableThread::Create(g_render_thread_runnable, "RenderingThread", 0xFFFFFFFFFFFFFFFF);

        static_cast<RenderThread*>(g_render_thread_runnable)->is_bound_to_taskgraph_event->Wait();
    };

    void StopRenderThread() {

        LOG_INFO("Wait for Rendering Thread To Stop.");
        //wait for thread to finish executing
        GraphEventRef return_task = GraphTask<ReturnGraphTask>::CreateTask(nullptr, EThread::EMainThread)
                                        .ConstructAndDispatchWhenReady(EThread::ERenderThread);

        //not sure, game thread tasks should not be executing, because we are currently on Game Thread
        assert(!TaskGraph::GetInterface().IsThreadProcessingTask(EThread::EMainThread) && "On Game Thread while Game Thread Tasks are being executing");
        TaskGraph::GetInterface().WaitUntilTaskComplete(return_task, EThread::EGameThread_local);

        delete g_render_thread;
        g_render_thread = nullptr;
        delete (RenderThread*)g_render_thread_runnable;
        g_render_thread_runnable = nullptr;

        LOG_INFO("Rendering Thread Stopped.");
    };

    void ShutDownRenderThread() {
    }

    void RestartRenderThread() {
        LOG_INFO("Restarting Render Thread.");
        assert(IsGameThreadInitialized() && IsCurrentlyGameThread() && "Render Thread Control is only allowed in Game Thread.");

        bool b_render_thread_not_shut_down = g_render_thread && g_render_thread_runnable;
        if (b_render_thread_not_shut_down) {
        }
    }

    void SuspendRenderThread(bool _restart_later) {
        assert(IsGameThreadInitialized() && IsCurrentlyGameThread() && "Render Thread Control is only allowed in Game Thread.");
        LOG_INFO("Try Suspending Render Thread.");
        if (_restart_later) {
            StopRenderThread();
            g_render_thread_suspending++;

        } else {
            if (g_render_thread_suspending.load(std::memory_order_relaxed)) {
                GraphEventRef suspend_complete_event = FunctionGraphTask::ConstructAndDispatchWhenReady(
                    []() {
                        g_render_thread_suspending++;
                    },
                    nullptr,
                    EThread::ERenderThread);
                TaskGraph::GetInterface().WaitUntilTaskComplete(suspend_complete_event, EThread::EMainThread);
                //now all works on task render thread has finished
                //start a waiting task on render thread
                g_render_suspend_end_event = EventPool::Get()->GetEvent();
                FunctionGraphTask::ConstructAndDispatchWhenReady(
                    []() {
                        LOG_INFO("Render Thread Suspending.");
                        g_render_suspend_end_event->Wait();
                        LOG_INFO("Render Thread End Suspending.");
                    },
                    nullptr,
                    EThread::ERenderThread);
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

                g_rhi->RHIFlushPendingDeletes();
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
                Event* event = EventPool::Get()->GetEvent();

                static uint32_t wait_recursive_counter = 0;

                wait_recursive_counter++;
                //already called
                if (wait_recursive_counter > 1) {
                    //TODO: messages
                }
                GraphEventArray temp_events;
                temp_events.push_back(complete_event);

                GraphTask<TriggerEventGraphTask>::CreateTask(&temp_events, EThread::EMainThread)
                    .ConstructAndDispatchWhenReady(event, EThread::AnyThread_NormalPri);

                event->Wait();
            }
        }
    }

}// namespace Moer
