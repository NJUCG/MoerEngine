#include "RHIExecutorTestHooksInternal.h"

#include "misc/RAII.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace Moer::Render {
namespace {

// Installation, callback acquisition and retirement share one lifetime.
struct MaterializationOverrideState {
    std::atomic<const RHIDirectCommandListMaterializationOverrideForTesting*> current{nullptr};
    std::mutex                                                                mutex;
    std::condition_variable                                                   cv;
    size_t                                                                    active_callbacks{0};
    bool                                                                      retiring{false};
};

MaterializationOverrideState g_materialization_override;

} // namespace

void InvokeDirectCommandListMaterializationOverrideForTesting(
    size_t _source_index
) {
    const auto* hook = g_materialization_override.current.load(std::memory_order_acquire);
    if (hook == nullptr) {
        return;
    }
    {
        std::lock_guard lock(g_materialization_override.mutex);
        if (g_materialization_override.retiring ||
            g_materialization_override.current.load(std::memory_order_relaxed) != hook) {
            return;
        }
        ++g_materialization_override.active_callbacks;
    }
    auto release_reader = OnScopeExit([&]() noexcept {
        std::lock_guard lock(g_materialization_override.mutex);
        assert(g_materialization_override.active_callbacks != 0);
        --g_materialization_override.active_callbacks;
        if (g_materialization_override.active_callbacks == 0) {
            g_materialization_override.cv.notify_all();
        }
    });
    hook->before_source(hook->context, _source_index);
}

bool TryInstallRHIDirectCommandListMaterializationOverrideForTesting(
    const RHIDirectCommandListMaterializationOverrideForTesting* _override
) noexcept {
    if (_override == nullptr || _override->before_source == nullptr) {
        return false;
    }
    std::lock_guard lock(g_materialization_override.mutex);
    if (g_materialization_override.retiring ||
        g_materialization_override.current.load(std::memory_order_relaxed) != nullptr) {
        return false;
    }
    g_materialization_override.current.store(
        _override, std::memory_order_release
    );
    return true;
}

bool RemoveRHIDirectCommandListMaterializationOverrideForTesting(
    const RHIDirectCommandListMaterializationOverrideForTesting* _override
) noexcept {
    if (_override == nullptr) {
        return false;
    }
    std::unique_lock lock(g_materialization_override.mutex);
    if (g_materialization_override.retiring ||
        g_materialization_override.current.load(std::memory_order_relaxed) != _override) {
        return false;
    }
    g_materialization_override.retiring = true;
    g_materialization_override.current.store(
        nullptr, std::memory_order_release
    );
    g_materialization_override.cv.wait(lock, [] {
        return g_materialization_override.active_callbacks == 0;
    });
    g_materialization_override.retiring = false;
    return true;
}

} // namespace Moer::Render
