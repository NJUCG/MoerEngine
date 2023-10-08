#include "RenderThread.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "taskgraph/Event.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/ThreadManager.h"
namespace Moer {

    uint32_t        g_render_thread_id       = 0;
    Runnable*       g_render_thread_runnable = nullptr;
    RunnableThread* g_render_thread          = nullptr;
    void            RenderThreadMain(Event* _is_bound_to_taskgraph_event) {
        TaskGraph::GetInterface().AttachToNameThread(EThread::ERenderThread);

        //barrier

        if (_is_bound_to_taskgraph_event) {
            _is_bound_to_taskgraph_event->Trigger();
        }
        assert(g_render_thread_id != 0 && "render thread not set");
        LOG_INFO("render thread {} executing", g_render_thread_id);
        TaskGraph::GetInterface().ProcessThreadUntilReturn(EThread::ERenderThread);
        LOG_INFO("render thread {} end", g_render_thread_id);
    }
    class RenderThread : public Runnable {
    public:
        Event* is_bound_to_taskgraph_event;

        RenderThread() {
            is_bound_to_taskgraph_event = EventPool::Get()->GetEvent(false);
            //todo: flush
        }

        virtual ~RenderThread() {
            EventPool::Get()->ReleaseEvent(is_bound_to_taskgraph_event);
        }

        virtual void Init() override {
            g_render_thread_id = Platform::GetCurrentThreadID();
        }
        virtual uint32_t Run() override {

            RenderThreadMain(is_bound_to_taskgraph_event);

            return 0;
        }
        virtual void Stop() override{};
        virtual void Exit() override {
            g_render_thread_id = 0;
        };
        virtual ThreadIndex GetIndex() override { return EThread::ERenderThread; };

    private:
    };

    void StartRenderingThread() {
        g_render_thread_runnable = new RenderThread();

        g_render_thread = RunnableThread::Create(g_render_thread_runnable, "RenderingThread", 0xFFFFFFFFFFFFFFFF);

        static_cast<RenderThread*>(g_render_thread_runnable)->is_bound_to_taskgraph_event->Wait();
    };

    void StopRenderingThread() {

        LOG_INFO("Wait for Rendering Thread To Stop.");
        //wait for thread to finish executing
        GraphEventRef return_task = GraphTask<ReturnGraphTask>::CreateTask(nullptr, EThread::EGameThread)
                                        .ConstructAndDispatchWhenReady(EThread::ERenderThread);

        //not sure, game thread tasks should not be executing, because we are currently on Game Thread
        assert(!TaskGraph::GetInterface().IsThreadProcessingTask(EThread::EGameThread) && "On Game Thread while Game Thread Tasks are being executing");
        TaskGraph::GetInterface().WaitUntilTaskComplete(return_task, EThread::EGameThread_local);
        LOG_INFO("Rendering Thread Stopped.");
    };

    void ShutDownRenderThread() {
        delete g_render_thread;
        g_render_thread = nullptr;
        delete (RenderThread*)g_render_thread_runnable;
        g_render_thread_runnable = nullptr;
    }
}// namespace Moer
