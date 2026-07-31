#include "rhi/RHIThreadHeartbeat.h"

#include "log/LogSystem.h"
#include "platform/Platform.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace Moer::Render {
namespace {

constexpr std::size_t   kMaxHeartbeatSlots = 128;
constexpr std::uint64_t kWritingBit        = 1;

thread_local RHIThreadHeartbeatHandle g_current_heartbeat{};

template<typename F>
class ScopeExit final {
public:
    explicit ScopeExit(F&& _callback) noexcept : callback(std::forward<F>(_callback)) {}

    ~ScopeExit() noexcept {
        callback();
    }

    ScopeExit(const ScopeExit&)            = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F callback;
};

[[nodiscard]] std::uint64_t ActiveToken(std::uint64_t _generation) noexcept {
    return _generation << 1;
}

[[nodiscard]] std::uint64_t TokenGeneration(std::uint64_t _token) noexcept {
    return _token >> 1;
}

[[nodiscard]] std::uint64_t ProductionNow(void*) noexcept {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch()
    )
                                          .count());
}

void ProductionSink(const RHIThreadHeartbeatEvent& _event, void*) noexcept {
    try {
        if (_event.kind == ERHIHeartbeatEventKind::Stalled) {
            LOG_WARNING(
                "[RHIHeartbeat] stalled role={} domain={} thread={} slot={} "
                "generation={} stale_ms={} stage={} progress={}",
                RHIThreadRoleName(_event.role),
                RHIHeartbeatDomainName(_event.domain),
                _event.thread_id,
                _event.slot_index,
                _event.generation,
                _event.stale_ms,
                RHIHeartbeatStageName(_event.stage),
                _event.progress_sequence
            );
        } else {
            LOG_INFO(
                "[RHIHeartbeat] recovered role={} domain={} thread={} slot={} "
                "generation={} previous_stale_ms={} stage={} progress={}",
                RHIThreadRoleName(_event.role),
                RHIHeartbeatDomainName(_event.domain),
                _event.thread_id,
                _event.slot_index,
                _event.generation,
                _event.stale_ms,
                RHIHeartbeatStageName(_event.stage),
                _event.progress_sequence
            );
        }
    } catch (...) {
        // Diagnostics must never destabilize the ownership pipeline.
    }
}

} // namespace

struct RHIThreadHeartbeat::Impl {
    struct Slot {
        // 0 = inactive, generation<<1 = active, generation<<1|1 = writer.
        std::atomic<std::uint64_t> state{0};
        std::atomic<std::uint64_t> last_pulse_ns{0};
        std::atomic<std::uint64_t> progress_sequence{0};
        std::atomic<std::uint32_t> thread_id{0};
        std::atomic<std::uint8_t>  role{static_cast<std::uint8_t>(ERHIThreadRole::Unknown)};
        std::atomic<std::uint8_t>  domain{static_cast<std::uint8_t>(ERHIHeartbeatDomain::General)};
        std::atomic<std::uint8_t>  stage{static_cast<std::uint8_t>(ERHIHeartbeatStage::Registered)};
    };

    struct MonitorSlot {
        std::uint64_t generation{0};
        std::uint64_t progress_sequence{0};
        std::uint64_t reported_stale_ms{0};
        bool          stalled{false};
    };

    RHIThreadHeartbeatConfig                    config{};
    RHIThreadHeartbeatTestingHooks              hooks{};
    std::array<Slot, kMaxHeartbeatSlots>        slots{};
    std::array<MonitorSlot, kMaxHeartbeatSlots> monitor_slots{};
    mutable std::mutex                          registry_mutex{};
    std::mutex                                  wait_mutex{};
    std::condition_variable                     wait_cv{};
    bool                                        monitor_running{false};
    std::thread                                 monitor_thread{};

    [[nodiscard]] std::uint64_t Now() const noexcept {
        const RHIHeartbeatNowFunction now = hooks.now != nullptr ? hooks.now : ProductionNow;
        return now(hooks.context);
    }

    void Emit(const RHIThreadHeartbeatEvent& _event) const noexcept {
        const RHIHeartbeatEventSink sink = hooks.sink != nullptr ? hooks.sink : ProductionSink;
        sink(_event, hooks.context);
    }
};

const char* RHIHeartbeatDomainName(ERHIHeartbeatDomain _domain) noexcept {
    switch (_domain) {
        case ERHIHeartbeatDomain::General:
            return "General";
        case ERHIHeartbeatDomain::Graphics:
            return "Graphics";
        case ERHIHeartbeatDomain::Compute:
            return "Compute";
        case ERHIHeartbeatDomain::Copy:
            return "Copy";
        case ERHIHeartbeatDomain::Present:
            return "Present";
    }
    return "Invalid";
}

const char* RHIHeartbeatStageName(ERHIHeartbeatStage _stage) noexcept {
    switch (_stage) {
        case ERHIHeartbeatStage::Registered:
            return "Registered";
        case ERHIHeartbeatStage::WaitingForWork:
            return "WaitingForWork";
        case ERHIHeartbeatStage::Dispatch:
            return "Dispatch";
        case ERHIHeartbeatStage::WaitForDependency:
            return "WaitForDependency";
        case ERHIHeartbeatStage::RecordCommands:
            return "RecordCommands";
        case ERHIHeartbeatStage::Translate:
            return "Translate";
        case ERHIHeartbeatStage::WaitForPipelineCapacity:
            return "WaitForPipelineCapacity";
        case ERHIHeartbeatStage::Submit:
            return "Submit";
        case ERHIHeartbeatStage::NativeSubmit:
            return "NativeSubmit";
        case ERHIHeartbeatStage::Present:
            return "Present";
        case ERHIHeartbeatStage::PollCompletion:
            return "PollCompletion";
        case ERHIHeartbeatStage::AwaitCompletion:
            return "AwaitCompletion";
        case ERHIHeartbeatStage::Shutdown:
            return "Shutdown";
    }
    return "Invalid";
}

bool IsRHIHeartbeatParkedStage(ERHIHeartbeatStage _stage) noexcept {
    return _stage == ERHIHeartbeatStage::WaitingForWork || _stage == ERHIHeartbeatStage::Shutdown;
}

RHIThreadHeartbeat& RHIThreadHeartbeat::Get() {
    static RHIThreadHeartbeat heartbeat{};
    return heartbeat;
}

RHIThreadHeartbeat::~RHIThreadHeartbeat() {
    Stop();
}

void RHIThreadHeartbeat::Start(RHIThreadHeartbeatConfig _config) noexcept {
    StartInternal(_config, {}, true);
}

void RHIThreadHeartbeat::StartForTesting(
    RHIThreadHeartbeatConfig       _config,
    RHIThreadHeartbeatTestingHooks _hooks
) {
    StartInternal(_config, _hooks, false);
}

void RHIThreadHeartbeat::StartInternal(
    RHIThreadHeartbeatConfig       _config,
    RHIThreadHeartbeatTestingHooks _hooks,
    bool                           _spawn_monitor
) noexcept {
    std::lock_guard lifecycle_lock(lifecycle_mutex);
    if (enabled.load(std::memory_order_acquire) || impl.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    if (!_config.enabled) {
        return;
    }

    _config.stall_timeout_ms = std::max<std::uint32_t>(1000, _config.stall_timeout_ms);
    _config.poll_interval_ms =
        std::clamp<std::uint32_t>(_config.poll_interval_ms, 50, _config.stall_timeout_ms);

    Impl* created = new (std::nothrow) Impl{};
    if (created == nullptr) {
        return;
    }
    created->config = _config;
    created->hooks  = _hooks;
    before_reader_gate_hook.store(_hooks.before_reader_gate, std::memory_order_release);
    impl.store(created, std::memory_order_release);
    impl_reader_state.store(0, std::memory_order_release);
    enabled.store(true, std::memory_order_release);

    if (_spawn_monitor) {
        try {
            {
                std::lock_guard lock(created->wait_mutex);
                created->monitor_running = true;
            }
            created->monitor_thread = std::thread([this] {
                MonitorLoop();
            });
        } catch (...) {
            enabled.store(false, std::memory_order_release);
            impl_reader_state.fetch_or(kImplReadersStoppingBit, std::memory_order_acq_rel);
            impl.store(nullptr, std::memory_order_release);
            WaitForImplReaders();
            delete created;
            return;
        }
        try {
            LOG_INFO(
                "[RHIHeartbeat] enabled timeout_ms={} poll_ms={} slots={}",
                _config.stall_timeout_ms,
                _config.poll_interval_ms,
                kMaxHeartbeatSlots
            );
        } catch (...) {
        }
    }
}

void RHIThreadHeartbeat::Stop() noexcept {
    std::lock_guard lifecycle_lock(lifecycle_mutex);
    enabled.store(false, std::memory_order_release);
    impl_reader_state.fetch_or(kImplReadersStoppingBit, std::memory_order_acq_rel);
    Impl* stopping = impl.exchange(nullptr, std::memory_order_acq_rel);
    if (stopping == nullptr) {
        WaitForImplReaders();
        g_current_heartbeat = {};
        return;
    }

    // Stop new registrations/pulses before waking the monitor. RenderDevice
    // calls this only after backend-owned threads have joined.
    {
        std::lock_guard lock(stopping->wait_mutex);
        stopping->monitor_running = false;
    }
    stopping->wait_cv.notify_all();
    if (stopping->monitor_thread.joinable()) {
        stopping->monitor_thread.join();
    }

    WaitForImplReaders();
    delete stopping;
    g_current_heartbeat = {};
}

bool RHIThreadHeartbeat::IsEnabled() const noexcept {
    return enabled.load(std::memory_order_acquire);
}

RHIThreadHeartbeat::Impl* RHIThreadHeartbeat::AcquireImplRead() const noexcept {
    if (!enabled.load(std::memory_order_acquire)) {
        return nullptr;
    }

    const RHIHeartbeatReaderAcquireHook before_gate = before_reader_gate_hook.load(std::memory_order_acquire);
    if (before_gate != nullptr) {
        before_gate();
    }

    std::uint64_t state = impl_reader_state.load(std::memory_order_acquire);
    for (;;) {
        if ((state & kImplReadersStoppingBit) != 0) {
            return nullptr;
        }
        if ((state & kImplReaderCountMask) == kImplReaderCountMask) {
            return nullptr;
        }
        if (impl_reader_state.compare_exchange_weak(
                state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire
            )) {
            break;
        }
    }

    Impl* active = impl.load(std::memory_order_acquire);
    if (active == nullptr) {
        ReleaseImplRead();
        return nullptr;
    }
    return active;
}

void RHIThreadHeartbeat::ReleaseImplRead() const noexcept {
    const std::uint64_t previous = impl_reader_state.fetch_sub(1, std::memory_order_acq_rel);
    if ((previous & kImplReaderCountMask) == 1) {
        impl_reader_state.notify_all();
    }
}

void RHIThreadHeartbeat::WaitForImplReaders() noexcept {
    std::uint64_t state = impl_reader_state.load(std::memory_order_acquire);
    while ((state & kImplReaderCountMask) != 0) {
        impl_reader_state.wait(state, std::memory_order_acquire);
        state = impl_reader_state.load(std::memory_order_acquire);
    }
}

RHIThreadHeartbeatHandle RHIThreadHeartbeat::Register(
    ERHIThreadRole      _role,
    ERHIHeartbeatDomain _domain,
    ERHIHeartbeatStage  _stage
) noexcept {
    Impl* active = AcquireImplRead();
    if (active == nullptr) {
        return {};
    }
    ScopeExit release_reader([this]() noexcept {
        ReleaseImplRead();
    });

    try {
        std::lock_guard lock(active->registry_mutex);
        if (!enabled.load(std::memory_order_acquire) || impl.load(std::memory_order_acquire) != active) {
            return {};
        }

        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(active->slots.size()); ++index) {
            Impl::Slot& slot = active->slots[index];
            if (slot.state.load(std::memory_order_acquire) != 0) {
                continue;
            }

            std::uint64_t generation = next_generation.fetch_add(1, std::memory_order_relaxed);
            if (generation == 0) {
                generation = next_generation.fetch_add(1, std::memory_order_relaxed);
            }

            slot.thread_id.store(Platform::GetCurrentThreadID(), std::memory_order_relaxed);
            slot.role.store(static_cast<std::uint8_t>(_role), std::memory_order_relaxed);
            slot.domain.store(static_cast<std::uint8_t>(_domain), std::memory_order_relaxed);
            slot.stage.store(static_cast<std::uint8_t>(_stage), std::memory_order_relaxed);
            slot.last_pulse_ns.store(active->Now(), std::memory_order_relaxed);
            slot.progress_sequence.store(1, std::memory_order_relaxed);
            slot.state.store(ActiveToken(generation), std::memory_order_release);

            g_current_heartbeat = {
                .slot_index = index,
                .generation = generation,
            };
            return g_current_heartbeat;
        }
    } catch (...) {
        // Registration is diagnostics-only and must not escape into an owner
        // thread, including allocation or platform-query failures.
    }
    return {};
}

void RHIThreadHeartbeat::Pulse(RHIThreadHeartbeatHandle _handle, ERHIHeartbeatStage _stage) noexcept {
    if (!_handle.IsValid()) {
        return;
    }

    Impl* active = AcquireImplRead();
    if (active == nullptr) {
        return;
    }
    ScopeExit release_reader([this]() noexcept {
        ReleaseImplRead();
    });
    if (_handle.slot_index >= active->slots.size()) {
        return;
    }

    Impl::Slot&   slot     = active->slots[_handle.slot_index];
    std::uint64_t expected = ActiveToken(_handle.generation);
    if (!slot.state.compare_exchange_strong(
            expected, expected | kWritingBit, std::memory_order_acquire, std::memory_order_relaxed
        )) {
        return;
    }

    try {
        slot.thread_id.store(Platform::GetCurrentThreadID(), std::memory_order_relaxed);
        slot.stage.store(static_cast<std::uint8_t>(_stage), std::memory_order_relaxed);
        slot.last_pulse_ns.store(active->Now(), std::memory_order_relaxed);
        slot.progress_sequence.store(
            slot.progress_sequence.load(std::memory_order_relaxed) + 1, std::memory_order_release
        );
    } catch (...) {
        // Restore a readable token even if a platform diagnostic query fails.
        // Heartbeat instrumentation must not terminate an owner thread.
    }
    slot.state.store(ActiveToken(_handle.generation), std::memory_order_release);
}

void RHIThreadHeartbeat::PulseCurrent(ERHIHeartbeatStage _stage) noexcept {
    Pulse(g_current_heartbeat, _stage);
}

void RHIThreadHeartbeat::Unregister(RHIThreadHeartbeatHandle& _handle) noexcept {
    if (!_handle.IsValid()) {
        return;
    }

    const RHIThreadHeartbeatHandle retiring = _handle;
    _handle                                 = {};
    if (g_current_heartbeat.slot_index == retiring.slot_index &&
        g_current_heartbeat.generation == retiring.generation) {
        g_current_heartbeat = {};
    }

    Impl* active = AcquireImplRead();
    if (active == nullptr) {
        return;
    }
    ScopeExit release_reader([this]() noexcept {
        ReleaseImplRead();
    });
    if (retiring.slot_index >= active->slots.size()) {
        return;
    }

    try {
        std::lock_guard     lock(active->registry_mutex);
        Impl::Slot&         slot  = active->slots[retiring.slot_index];
        const std::uint64_t token = ActiveToken(retiring.generation);
        for (;;) {
            std::uint64_t expected = slot.state.load(std::memory_order_acquire);
            if (expected == 0 || TokenGeneration(expected) != retiring.generation) {
                return;
            }
            if ((expected & kWritingBit) != 0) {
                std::this_thread::yield();
                continue;
            }
            if (!slot.state.compare_exchange_weak(
                    expected, token | kWritingBit, std::memory_order_acquire, std::memory_order_relaxed
                )) {
                continue;
            }
            slot.state.store(0, std::memory_order_release);
            return;
        }
    } catch (...) {
    }
}

void RHIThreadHeartbeat::MonitorLoop() noexcept {
    try {
        Platform::SetCurrentThreadName("Moer RHI Heartbeat");
    } catch (...) {
    }

    Impl* active = impl.load(std::memory_order_acquire);
    if (active == nullptr) {
        return;
    }

    for (;;) {
        {
            std::unique_lock lock(active->wait_mutex);
            active->wait_cv
                .wait_for(lock, std::chrono::milliseconds(active->config.poll_interval_ms), [active] {
                    return !active->monitor_running;
                });
            if (!active->monitor_running) {
                return;
            }
        }
        PollOnce();
    }
}

void RHIThreadHeartbeat::PollOnceForTesting() noexcept {
    if (!IsMonitorRunningForTesting()) {
        PollOnce();
    }
}

bool RHIThreadHeartbeat::IsMonitorRunningForTesting() const noexcept {
    Impl* active = AcquireImplRead();
    if (active == nullptr) {
        return false;
    }
    ScopeExit       release_reader([this]() noexcept {
        ReleaseImplRead();
    });
    std::lock_guard lock(active->wait_mutex);
    return active->monitor_running;
}

std::uint32_t RHIThreadHeartbeat::ActiveSlotCountForTesting() const noexcept {
    Impl* active = AcquireImplRead();
    if (active == nullptr) {
        return 0;
    }
    ScopeExit     release_reader([this]() noexcept {
        ReleaseImplRead();
    });
    std::uint32_t count = 0;
    for (const Impl::Slot& slot : active->slots) {
        const std::uint64_t token = slot.state.load(std::memory_order_acquire);
        if (token != 0 && (token & kWritingBit) == 0) {
            ++count;
        }
    }
    return count;
}

void RHIThreadHeartbeat::PollOnce() noexcept {
    Impl* active = AcquireImplRead();
    if (active == nullptr) {
        return;
    }
    ScopeExit release_reader([this]() noexcept {
        ReleaseImplRead();
    });

    std::array<RHIThreadHeartbeatEvent, kMaxHeartbeatSlots * 2> events{};
    std::size_t                                                 event_count = 0;
    const std::uint64_t                                         now         = active->Now();
    const std::uint64_t                                         timeout_ns =
        static_cast<std::uint64_t>(active->config.stall_timeout_ms) * 1'000'000ull;

    for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(active->slots.size()); ++index) {
        Impl::Slot&        slot    = active->slots[index];
        Impl::MonitorSlot& monitor = active->monitor_slots[index];

        const std::uint64_t state_before = slot.state.load(std::memory_order_acquire);
        if (state_before == 0) {
            monitor = {};
            continue;
        }
        if ((state_before & kWritingBit) != 0) {
            continue;
        }

        const std::uint64_t generation = TokenGeneration(state_before);
        // Progress is also the snapshot sequence. Reading it on both sides of
        // the payload closes the active->writing->active ABA window where the
        // state token intentionally returns to the same generation.
        const std::uint64_t progress_before = slot.progress_sequence.load(std::memory_order_acquire);
        const std::uint64_t last_pulse_ns   = slot.last_pulse_ns.load(std::memory_order_relaxed);
        const std::uint32_t thread_id       = slot.thread_id.load(std::memory_order_relaxed);
        const auto          role = static_cast<ERHIThreadRole>(slot.role.load(std::memory_order_relaxed));
        const auto domain = static_cast<ERHIHeartbeatDomain>(slot.domain.load(std::memory_order_relaxed));
        const auto stage  = static_cast<ERHIHeartbeatStage>(slot.stage.load(std::memory_order_relaxed));
        if (active->hooks.after_snapshot_payload != nullptr) {
            active->hooks.after_snapshot_payload(index, active->hooks.context);
        }

        const std::uint64_t state_after    = slot.state.load(std::memory_order_acquire);
        const std::uint64_t progress_after = slot.progress_sequence.load(std::memory_order_acquire);
        if (state_before != state_after || progress_before != progress_after) {
            continue;
        }
        const std::uint64_t progress = progress_after;

        if (monitor.generation != generation) {
            monitor = {
                .generation        = generation,
                .progress_sequence = progress,
            };
        } else if (monitor.progress_sequence != progress) {
            if (monitor.stalled && event_count < events.size()) {
                events[event_count++] = {
                    .kind              = ERHIHeartbeatEventKind::Recovered,
                    .role              = role,
                    .domain            = domain,
                    .stage             = stage,
                    .thread_id         = thread_id,
                    .slot_index        = index,
                    .generation        = generation,
                    .progress_sequence = progress,
                    .stale_ms          = monitor.reported_stale_ms,
                };
            }
            monitor.progress_sequence = progress;
            monitor.reported_stale_ms = 0;
            monitor.stalled           = false;
        }

        if (IsRHIHeartbeatParkedStage(stage)) {
            continue;
        }

        const std::uint64_t stale_ns = now >= last_pulse_ns ? now - last_pulse_ns : 0;
        if (stale_ns < timeout_ns || monitor.stalled) {
            continue;
        }

        const std::uint64_t stale_ms = stale_ns / 1'000'000ull;
        monitor.stalled              = true;
        monitor.reported_stale_ms    = stale_ms;
        if (event_count < events.size()) {
            events[event_count++] = {
                .kind              = ERHIHeartbeatEventKind::Stalled,
                .role              = role,
                .domain            = domain,
                .stage             = stage,
                .thread_id         = thread_id,
                .slot_index        = index,
                .generation        = generation,
                .progress_sequence = progress,
                .stale_ms          = stale_ms,
            };
        }
    }

    for (std::size_t index = 0; index < event_count; ++index) {
        active->Emit(events[index]);
    }
}

RHIThreadHeartbeatScope::RHIThreadHeartbeatScope(
    ERHIThreadRole      _role,
    ERHIHeartbeatDomain _domain,
    ERHIHeartbeatStage  _stage
) noexcept :
    previous_handle(g_current_heartbeat),
    handle(RHIThreadHeartbeat::Get().Register(_role, _domain, _stage)) {}

RHIThreadHeartbeatScope::~RHIThreadHeartbeatScope() {
    RHIThreadHeartbeat::Get().Unregister(handle);
    g_current_heartbeat = previous_handle;
}

void RHIThreadHeartbeatScope::Pulse(ERHIHeartbeatStage _stage) noexcept {
    RHIThreadHeartbeat::Get().Pulse(handle, _stage);
}

} // namespace Moer::Render
