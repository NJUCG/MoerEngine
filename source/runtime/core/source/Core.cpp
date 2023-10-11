#include "Core.h"
#include "taskgraph/ThreadManager.h"
namespace Moer {
    bool IsCurrentlyGameThread() {
        return ThreadManager::g_game_thread_id == Platform::GetCurrentThreadID();
    }

    bool IsCurrentlyRenderThread() {
        return ThreadManager::g_render_thread_id == Platform::GetCurrentThreadID();
    }

    bool IsGameThreadInitialized() {
        return ThreadManager::g_game_thread_id != 0;
    }

    bool IsRenderThreadInitialized() {
        return ThreadManager::g_render_thread_id != 0;
    }
}// namespace Moer
