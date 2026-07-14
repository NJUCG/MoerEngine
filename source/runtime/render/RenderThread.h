#ifndef MOERENGINE_RENDER_THREAD_H
#define MOERENGINE_RENDER_THREAD_H
#include "API_Macro.h"
#include "Core.h"
#include "RenderAPI.h"
#include <cstdint>
#include <type_traits>
#include <utility>
class Runnable;
class RunnableThread;
namespace Moer {
class RenderThread;
extern Runnable*       g_render_thread_runnable;
extern RunnableThread* g_render_thread;
extern void RENDER_API StartRenderThread();

extern void RENDER_API StopRenderThread();

extern void RENDER_API ShutDownRenderThread();

extern void RENDER_API RestartRenderThread();

//
extern void RENDER_API SuspendRenderThread(bool _restart_later = true);

extern void RENDER_API ResumeRenderThread(bool _promised_restart_before = true);

extern bool RENDER_API IsRenderThreadRunning();

template<typename Function>
class RenderThreadServiceTask {
public:
    explicit RenderThreadServiceTask(Function&& function) : m_function(std::move(function)) {}

    static EThread::Type GetPreferredThread() {
        return EThread::ERenderThread;
    }

    void Fire(EThread::Type, const GraphEventRef&) {
        m_function();
    }

private:
    Function m_function;
};

class RENDER_API RenderThreadService {
public:
    ~RenderThreadService();

    void Start();
    void Stop();

    template<typename Function>
        requires std::is_invocable_v<std::decay_t<Function>&>
    GraphEventRef Enqueue(Function&& task) {
        assert(running && "Cannot enqueue work before the render thread starts.");
        assert(IsCurrentlyGameThread() && "RenderThreadService is submitted by the game thread.");

        using StoredFunction = std::decay_t<Function>;
        using TaskType       = RenderThreadServiceTask<StoredFunction>;
        return GraphTask<TaskType>::Create(StoredFunction(std::forward<Function>(task)))
            .Dispatch(EThread::ERenderThread);
    }

    void Wait(const GraphEventRef& event);

    template<typename Function>
        requires std::is_invocable_v<std::decay_t<Function>&>
    void RunAndWait(Function&& task) {
        auto event = Enqueue(std::forward<Function>(task));
        Wait(event);
    }

    void Flush();

    bool IsRunning() const {
        return running;
    }

private:
    bool running = false;
};

class RENDER_API ScopedResumeRenderThread {
public:
    ScopedResumeRenderThread();
    ~ScopedResumeRenderThread();
};

class RENDER_API RenderThreadFence {
public:
    RenderThreadFence() {}

    void BeginFence();

    bool IsFenceComplete();

    void Wait();

private:
    GraphEventRef complete_event;
};

} // namespace Moer

#endif
