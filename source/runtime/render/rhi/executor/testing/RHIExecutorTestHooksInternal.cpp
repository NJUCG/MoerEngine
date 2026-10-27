#include "RHIExecutorTestHooksInternal.h"

#include "misc/RAII.h"
#include <atomic>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace Moer::Render {
namespace {

std::atomic<
    const RHIDirectCommandListMaterializationOverrideForTesting*>
           g_direct_command_list_materialization_override{nullptr};
std::mutex g_direct_command_list_materialization_override_mutex{};
std::condition_variable
       g_direct_command_list_materialization_override_cv{};
size_t g_direct_command_list_materialization_active_callbacks{0};
bool   g_direct_command_list_materialization_override_retiring{false};

} // namespace

void InvokeDirectCommandListMaterializationOverrideForTesting(
    size_t _source_index
) {
    const auto* override =
        g_direct_command_list_materialization_override.load(
            std::memory_order_acquire
        );
    if (override == nullptr) {
        return;
    }
    {
        std::lock_guard lock(
            g_direct_command_list_materialization_override_mutex
        );
        if (g_direct_command_list_materialization_override_retiring ||
            g_direct_command_list_materialization_override.load(
                std::memory_order_relaxed
            ) != override) {
            return;
        }
        ++g_direct_command_list_materialization_active_callbacks;
    }
    auto release_reader = OnScopeExit([&]() noexcept {
        std::lock_guard lock(
            g_direct_command_list_materialization_override_mutex
        );
        assert(
            g_direct_command_list_materialization_active_callbacks != 0
        );
        --g_direct_command_list_materialization_active_callbacks;
        if (g_direct_command_list_materialization_active_callbacks == 0) {
            g_direct_command_list_materialization_override_cv.notify_all();
        }
    });
    override->before_source(override->context, _source_index);
}

bool TryInstallRHIDirectCommandListMaterializationOverrideForTesting(
    const RHIDirectCommandListMaterializationOverrideForTesting* _override
) noexcept {
    if (_override == nullptr || _override->before_source == nullptr) {
        return false;
    }
    std::lock_guard lock(
        g_direct_command_list_materialization_override_mutex
    );
    if (g_direct_command_list_materialization_override_retiring ||
        g_direct_command_list_materialization_override.load(
            std::memory_order_relaxed
        ) != nullptr) {
        return false;
    }
    g_direct_command_list_materialization_override.store(
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
    std::unique_lock lock(
        g_direct_command_list_materialization_override_mutex
    );
    if (g_direct_command_list_materialization_override_retiring ||
        g_direct_command_list_materialization_override.load(
            std::memory_order_relaxed
        ) != _override) {
        return false;
    }
    g_direct_command_list_materialization_override_retiring = true;
    g_direct_command_list_materialization_override.store(
        nullptr, std::memory_order_release
    );
    g_direct_command_list_materialization_override_cv.wait(
        lock,
        [] {
            return g_direct_command_list_materialization_active_callbacks ==
                   0;
        }
    );
    g_direct_command_list_materialization_override_retiring = false;
    return true;
}

} // namespace Moer::Render
