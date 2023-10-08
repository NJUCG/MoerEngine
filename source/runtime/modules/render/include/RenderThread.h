#ifndef MOERENGINE_RENDER_THREAD_H
#define MOERENGINE_RENDER_THREAD_H
#include "API_Macro.h"
#include <cstdint>
class Runnable;
class RunnableThread;
namespace Moer {
    class RenderThread;

    extern uint32_t             g_render_thread_id;
    extern Runnable*            g_render_thread_runnable;
    extern RunnableThread*      g_render_thread;
    extern void RENDER_CORE_API StartRenderingThread();

    extern void RENDER_CORE_API StopRenderingThread();

    extern void RENDER_CORE_API ShutDownRenderThread();
}// namespace Moer

#endif