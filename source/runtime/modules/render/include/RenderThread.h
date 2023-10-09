#ifndef MOERENGINE_RENDER_THREAD_H
#define MOERENGINE_RENDER_THREAD_H
#include "API_Macro.h"
#include <cstdint>
class Runnable;
class RunnableThread;
namespace Moer {
    class RenderThread;
    extern Runnable*            g_render_thread_runnable;
    extern RunnableThread*      g_render_thread;
    extern void RENDER_CORE_API StartRenderThread();

    extern void RENDER_CORE_API StopRenderThread();

    extern void RENDER_CORE_API ShutDownRenderThread();

    extern void RENDER_CORE_API RestartRenderThread();

    //
    extern void RENDER_CORE_API SuspendRenderThread(bool _restart_later = true);

    extern void RENDER_CORE_API ResumeRenderThread(bool _promised_restart_before = true);

    class RENDER_CORE_API ScopedResumeRenderThread {
    public:
        ScopedResumeRenderThread();
        ~ScopedResumeRenderThread();
    };

}// namespace Moer

#endif