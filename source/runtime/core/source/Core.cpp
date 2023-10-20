#include "Core.h"
#include "taskgraph/ThreadManager.h"
namespace Moer {
    bool IsCurrentlyGameThread() {
        return GetGameThreadId() == Platform::GetCurrentThreadID();
    }

    bool IsCurrentlyRenderThread() {
        return GetRenderThreadId() == Platform::GetCurrentThreadID();
    }

    bool IsGameThreadInitialized() {
        return ThreadManager::GetGameThreadID() != 0;
    }

    bool IsRenderThreadInitialized() {
        return GetRenderThreadId() != 0;
    }
    uint32_t GetRenderThreadId() {
        return ThreadManager::GetRenderThreadID();
    }

    uint32_t GetGameThreadId() {
        return ThreadManager::GetGameThreadID();
    }
}// namespace Moer
