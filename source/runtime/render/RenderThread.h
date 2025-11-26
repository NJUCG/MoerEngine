#ifndef MOERENGINE_RENDER_THREAD_H
#define MOERENGINE_RENDER_THREAD_H
#include "API_Macro.h"
#include "Core.h"
#include "RenderAPI.h"
#include <cstdint>
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