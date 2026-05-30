#include "VulkanThreadHeartbeat.h"

#include "VulkanRHITrace.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace Moer::Render {

namespace {

thread_local VulkanThreadHeartbeat::Handle g_current_heartbeat_handle{};

}

struct VulkanThreadHeartbeat::Impl {
    using Clock = std::chrono::steady_clock;

    struct Slot {
        bool             active = false;
        uint32_t         thread_id = 0;
        String           label{};
        String           stage{};
        Clock::time_point last_pulse{};
        Clock::time_point last_report{};
    };

    bool                     enabled = false;
    uint64_t                 timeout_ms = 5000;
    uint64_t                 poll_ms = 1000;
    std::mutex               mutex{};
    std::condition_variable  cv{};
    std::vector<Slot>        slots{};
    bool                     running = false;
    std::thread              monitor_thread{};
};

VulkanThreadHeartbeat& VulkanThreadHeartbeat::Get() {
    static VulkanThreadHeartbeat watchdog{};
    return watchdog;
}

bool VulkanThreadHeartbeat::Enabled() const {
    return m_impl->enabled;
}

VulkanThreadHeartbeat::Handle VulkanThreadHeartbeat::Register(StringView label, StringView stage) {
    if (!Enabled()) {
        return {};
    }

    EnsureStarted();

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const auto                  now = Impl::Clock::now();
    const uint32_t              thread_id = Platform::GetCurrentThreadID();
    for (uint32_t index = 0; index < m_impl->slots.size(); ++index) {
        auto& slot = m_impl->slots[index];
        if (slot.active) {
            continue;
        }

        slot.active = true;
        slot.thread_id = thread_id;
        slot.label = String(label);
        slot.stage = String(stage);
        slot.last_pulse = now;
        slot.last_report = now;
        g_current_heartbeat_handle = Handle{.slot_index = index};
        return Handle{.slot_index = index};
    }

    Impl::Slot slot{};
    slot.active = true;
    slot.thread_id = thread_id;
    slot.label = String(label);
    slot.stage = String(stage);
    slot.last_pulse = now;
    slot.last_report = now;
    m_impl->slots.emplace_back(std::move(slot));
    g_current_heartbeat_handle = Handle{.slot_index = static_cast<uint32_t>(m_impl->slots.size() - 1)};
    return g_current_heartbeat_handle;
}

void VulkanThreadHeartbeat::Pulse(Handle handle, StringView stage) {
    if (!Enabled() || !handle.IsValid()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (handle.slot_index >= m_impl->slots.size()) {
        return;
    }

    auto& slot = m_impl->slots[handle.slot_index];
    if (!slot.active) {
        return;
    }

    slot.thread_id = Platform::GetCurrentThreadID();
    slot.stage = String(stage);
    slot.last_pulse = Impl::Clock::now();
}

void VulkanThreadHeartbeat::PulseCurrent(StringView stage) {
    Pulse(g_current_heartbeat_handle, stage);
}

void VulkanThreadHeartbeat::Unregister(Handle& handle) {
    if (!Enabled() || !handle.IsValid()) {
        handle = {};
        return;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (handle.slot_index < m_impl->slots.size()) {
        auto& slot = m_impl->slots[handle.slot_index];
        slot.active = false;
        slot.thread_id = 0;
        slot.label.clear();
        slot.stage.clear();
        slot.last_pulse = Impl::Clock::time_point{};
        slot.last_report = Impl::Clock::time_point{};
    }
    if (g_current_heartbeat_handle.slot_index == handle.slot_index) {
        g_current_heartbeat_handle = {};
    }
    handle = {};
}

VulkanThreadHeartbeat::VulkanThreadHeartbeat() : m_impl(new Impl()) {
    m_impl->enabled = !ReadRHITraceEnvValue("MOER_THREAD_HEARTBEAT").empty();
    m_impl->timeout_ms = std::max<uint64_t>(
        1000,
        ParseRHITraceEnvUInt64("MOER_THREAD_HEARTBEAT_TIMEOUT_MS", m_impl->timeout_ms)
    );
    m_impl->poll_ms = std::max<uint64_t>(
        250,
        ParseRHITraceEnvUInt64("MOER_THREAD_HEARTBEAT_POLL_MS", m_impl->poll_ms)
    );
}

VulkanThreadHeartbeat::~VulkanThreadHeartbeat() {
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->running = false;
    }
    m_impl->cv.notify_all();
    if (m_impl->monitor_thread.joinable()) {
        m_impl->monitor_thread.join();
    }
    delete m_impl;
    m_impl = nullptr;
}

void VulkanThreadHeartbeat::EnsureStarted() {
    if (!Enabled()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (m_impl->running) {
        return;
    }

    m_impl->running = true;
    m_impl->monitor_thread = std::thread([this]() { MonitorLoop(); });
    LOG_INFO(
        MOER_TEXT("[ThreadHeartbeat] enabled timeout_ms={} poll_ms={}"),
        m_impl->timeout_ms,
        m_impl->poll_ms
    );
}

void VulkanThreadHeartbeat::MonitorLoop() {
    Platform::SetCurrentThreadName(MOER_ASCII_TEXT("HeartbeatWatchdog"));

    struct Snapshot {
        uint32_t thread_id = 0;
        uint64_t stale_ms = 0;
        String   label{};
        String   stage{};
    };

    while (true) {
        std::vector<Snapshot> stale_slots{};
        {
            std::unique_lock<std::mutex> lock(m_impl->mutex);
            m_impl->cv.wait_for(lock, std::chrono::milliseconds(m_impl->poll_ms), [this]() {
                return !m_impl->running;
            });
            if (!m_impl->running) {
                return;
            }

            const auto now = Impl::Clock::now();
            for (auto& slot : m_impl->slots) {
                if (!slot.active) {
                    continue;
                }

                const auto stale_duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - slot.last_pulse).count();
                const auto report_duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - slot.last_report).count();
                if (stale_duration < static_cast<int64_t>(m_impl->timeout_ms) ||
                    report_duration < static_cast<int64_t>(m_impl->timeout_ms)) {
                    continue;
                }

                slot.last_report = now;
                stale_slots.emplace_back(Snapshot{
                    .thread_id = slot.thread_id,
                    .stale_ms = static_cast<uint64_t>(stale_duration),
                    .label = slot.label,
                    .stage = slot.stage,
                });
            }
        }

        for (const auto& snapshot : stale_slots) {
            LOG_WARNING(
                MOER_TEXT("[ThreadHeartbeat] stalled thread={} id={} stale_ms={} last_stage={}"),
                snapshot.label,
                snapshot.thread_id,
                snapshot.stale_ms,
                snapshot.stage
            );
        }
    }
}

} // namespace Moer::Render