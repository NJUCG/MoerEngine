#ifndef MOERENGINE_RENDER_THREAD_H
#define MOERENGINE_RENDER_THREAD_H
#include "API_Macro.h"
#include "Core.h"
#include "RenderAPI.h"
#include <cstdint>
#include <functional>
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

class RENDER_API RenderThreadService {
public:
    ~RenderThreadService();

    void Start();
    void Stop();

    GraphEventRef Enqueue(std::function<void()> task);
    void          Wait(const GraphEventRef& event);
    void          RunAndWait(std::function<void()> task);
    void          Flush();

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
