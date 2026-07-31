#include "rhi/RHIThreadHeartbeat.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using namespace Moer::Render;

struct TestContext {
    std::uint64_t                        now_ns{0};
    std::uint32_t                        clock_queries{0};
    std::vector<RHIThreadHeartbeatEvent> events{};
};

struct SnapshotRaceContext {
    std::atomic<std::uint64_t> now_ns{0};
    std::atomic<std::uint32_t> event_count{0};
    std::atomic_bool           snapshot_observed{false};
    std::atomic_bool           pulse_completed{false};
};

struct StopRaceContext {
    std::atomic<std::uint64_t> now_ns{0};
    std::atomic_bool           block_next_clock{false};
    std::atomic_bool           pulse_entered{false};
    std::atomic_bool           release_pulse{false};
};

struct PreGateRaceContext {
    std::atomic_bool block_next_acquire{false};
    std::atomic_bool acquire_entered{false};
    std::atomic_bool release_acquire{false};
};

PreGateRaceContext g_pre_gate_race{};

std::uint64_t ReadFakeClock(void* _context) noexcept {
    auto& context = *static_cast<TestContext*>(_context);
    ++context.clock_queries;
    return context.now_ns;
}

void CaptureEvent(const RHIThreadHeartbeatEvent& _event, void* _context) noexcept {
    static_cast<TestContext*>(_context)->events.emplace_back(_event);
}

std::uint64_t ReadRaceClock(void* _context) noexcept {
    return static_cast<SnapshotRaceContext*>(_context)->now_ns.load(std::memory_order_acquire);
}

void CountRaceEvent(const RHIThreadHeartbeatEvent&, void* _context) noexcept {
    static_cast<SnapshotRaceContext*>(_context)->event_count.fetch_add(1, std::memory_order_relaxed);
}

void PauseAfterSnapshotPayload(std::uint32_t, void* _context) noexcept {
    auto& context = *static_cast<SnapshotRaceContext*>(_context);
    context.snapshot_observed.store(true, std::memory_order_release);
    while (!context.pulse_completed.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

std::uint64_t ReadStopRaceClock(void* _context) noexcept {
    auto& context = *static_cast<StopRaceContext*>(_context);
    if (context.block_next_clock.exchange(false, std::memory_order_acq_rel)) {
        context.pulse_entered.store(true, std::memory_order_release);
        while (!context.release_pulse.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    return context.now_ns.load(std::memory_order_acquire);
}

void PauseBeforeReaderGate() noexcept {
    if (!g_pre_gate_race.block_next_acquire.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    g_pre_gate_race.acquire_entered.store(true, std::memory_order_release);
    while (!g_pre_gate_race.release_acquire.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

bool WaitForTrue(const std::atomic_bool& _value) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (!_value.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

bool Expect(bool _condition, const char* _message) {
    if (_condition) {
        return true;
    }
    std::cerr << "RHIThreadHeartbeatContract: " << _message << '\n';
    return false;
}

RHIThreadHeartbeatConfig EnabledConfig() {
    return {
        .enabled          = true,
        .stall_timeout_ms = 1000,
        .poll_interval_ms = 100,
    };
}

RHIThreadHeartbeatTestingHooks Hooks(TestContext& _context) {
    return {
        .now     = ReadFakeClock,
        .sink    = CaptureEvent,
        .context = &_context,
    };
}

bool DisabledIsAZeroServiceFastPath() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    TestContext context{};
    heartbeat.StartForTesting(RHIThreadHeartbeatConfig{.enabled = false}, Hooks(context));
    RHIThreadHeartbeatHandle handle = heartbeat.Register(
        ERHIThreadRole::Executor, ERHIHeartbeatDomain::General, ERHIHeartbeatStage::Dispatch
    );
    heartbeat.Pulse(handle, ERHIHeartbeatStage::NativeSubmit);
    heartbeat.PollOnceForTesting();

    const bool okay = Expect(!heartbeat.IsEnabled(), "disabled config started the service") &&
                      Expect(!heartbeat.IsMonitorRunningForTesting(), "disabled config started a monitor") &&
                      Expect(!handle.IsValid(), "disabled Register returned a live handle") &&
                      Expect(context.clock_queries == 0, "disabled hot path queried the clock") &&
                      Expect(context.events.empty(), "disabled service emitted an event");
    heartbeat.Stop();
    return okay;
}

bool ReportsOneStallThenOneRecovery() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    TestContext context{.now_ns = 10'000'000'000ull};
    heartbeat.StartForTesting(EnabledConfig(), Hooks(context));
    RHIThreadHeartbeatHandle handle = heartbeat.Register(
        ERHIThreadRole::Translate, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::Translate
    );

    context.now_ns += 999'000'000ull;
    heartbeat.PollOnceForTesting();
    bool okay = Expect(context.events.empty(), "stall reported before timeout");

    context.now_ns += 1'000'000ull;
    heartbeat.PollOnceForTesting();
    okay &= Expect(context.events.size() == 1, "first stall was not reported once");
    if (context.events.size() == 1) {
        const auto& event = context.events.front();
        okay &= Expect(event.kind == ERHIHeartbeatEventKind::Stalled, "first event was not a stall");
        okay &= Expect(
            event.role == ERHIThreadRole::Translate && event.domain == ERHIHeartbeatDomain::Graphics &&
                event.stage == ERHIHeartbeatStage::Translate,
            "stall identity did not preserve role/domain/stage"
        );
        okay &= Expect(event.stale_ms == 1000, "stall duration was not deterministic");
    }

    context.now_ns += 5'000'000'000ull;
    heartbeat.PollOnceForTesting();
    okay &= Expect(context.events.size() == 1, "unchanged stall was reported repeatedly");

    heartbeat.Pulse(handle, ERHIHeartbeatStage::NativeSubmit);
    heartbeat.PollOnceForTesting();
    okay &= Expect(context.events.size() == 2, "progress did not emit recovery");
    if (context.events.size() == 2) {
        const auto& event = context.events.back();
        okay &= Expect(event.kind == ERHIHeartbeatEventKind::Recovered, "progress event was not recovery");
        okay &= Expect(
            event.stage == ERHIHeartbeatStage::NativeSubmit && event.stale_ms == 1000,
            "recovery did not preserve the new stage and reported stall age"
        );
    }

    heartbeat.Unregister(handle);
    okay &= Expect(heartbeat.ActiveSlotCountForTesting() == 0, "Unregister left a live slot");
    heartbeat.Stop();
    return okay;
}

bool ParkedStagesDoNotReportFalseStalls() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    TestContext context{.now_ns = 20'000'000'000ull};
    heartbeat.StartForTesting(EnabledConfig(), Hooks(context));
    RHIThreadHeartbeatHandle handle = heartbeat.Register(
        ERHIThreadRole::Completion, ERHIHeartbeatDomain::Copy, ERHIHeartbeatStage::WaitingForWork
    );

    context.now_ns += 60'000'000'000ull;
    heartbeat.PollOnceForTesting();
    bool okay = Expect(context.events.empty(), "idle service thread was reported as stalled");

    heartbeat.Pulse(handle, ERHIHeartbeatStage::Shutdown);
    context.now_ns += 60'000'000'000ull;
    heartbeat.PollOnceForTesting();
    okay &= Expect(context.events.empty(), "shutdown parked state was reported as stalled");

    heartbeat.Unregister(handle);
    heartbeat.Stop();
    return okay;
}

bool GenerationRejectsStaleHandlesAfterReuse() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    TestContext context{.now_ns = 30'000'000'000ull};
    heartbeat.StartForTesting(EnabledConfig(), Hooks(context));

    RHIThreadHeartbeatHandle first = heartbeat.Register(
        ERHIThreadRole::Executor, ERHIHeartbeatDomain::General, ERHIHeartbeatStage::Dispatch
    );
    const RHIThreadHeartbeatHandle stale = first;
    heartbeat.Unregister(first);
    RHIThreadHeartbeatHandle second = heartbeat.Register(
        ERHIThreadRole::Submission, ERHIHeartbeatDomain::Compute, ERHIHeartbeatStage::Submit
    );

    bool okay = Expect(second.slot_index == stale.slot_index, "freed slot was not reused") &&
                Expect(second.generation != stale.generation, "slot generation did not advance");

    heartbeat.Pulse(stale, ERHIHeartbeatStage::Present);
    context.now_ns += 1'000'000'000ull;
    heartbeat.PollOnceForTesting();
    okay &= Expect(context.events.size() == 1, "reused live slot did not stall");
    if (context.events.size() == 1) {
        const auto& event = context.events.front();
        okay &= Expect(
            event.generation == second.generation && event.role == ERHIThreadRole::Submission &&
                event.domain == ERHIHeartbeatDomain::Compute && event.stage == ERHIHeartbeatStage::Submit,
            "stale handle mutated the reused slot"
        );
    }

    heartbeat.Unregister(second);
    heartbeat.PollOnceForTesting();
    okay &= Expect(context.events.size() == 1, "Unregister synthesized a recovery event");
    heartbeat.Stop();
    return okay;
}

bool GenerationRejectsStaleHandlesAfterRestart() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    TestContext first_context{.now_ns = 35'000'000'000ull};
    heartbeat.StartForTesting(EnabledConfig(), Hooks(first_context));
    RHIThreadHeartbeatHandle first = heartbeat.Register(
        ERHIThreadRole::Executor, ERHIHeartbeatDomain::General, ERHIHeartbeatStage::Dispatch
    );
    const RHIThreadHeartbeatHandle stale = first;
    heartbeat.Stop();

    TestContext second_context{.now_ns = 40'000'000'000ull};
    heartbeat.StartForTesting(EnabledConfig(), Hooks(second_context));
    RHIThreadHeartbeatHandle second = heartbeat.Register(
        ERHIThreadRole::Submission, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::Submit
    );

    bool okay = Expect(second.slot_index == stale.slot_index, "restart did not reuse the first slot") &&
                Expect(second.generation != stale.generation, "restart reset the slot generation");

    heartbeat.Pulse(stale, ERHIHeartbeatStage::Present);
    RHIThreadHeartbeatHandle retiring_stale = stale;
    heartbeat.Unregister(retiring_stale);
    okay &= Expect(
        heartbeat.ActiveSlotCountForTesting() == 1, "stale pre-restart handle retired the new participant"
    );

    second_context.now_ns += 1'000'000'000ull;
    heartbeat.PollOnceForTesting();
    okay &= Expect(second_context.events.size() == 1, "restarted live slot did not stall");
    if (second_context.events.size() == 1) {
        const auto& event = second_context.events.front();
        okay &= Expect(
            event.generation == second.generation && event.role == ERHIThreadRole::Submission &&
                event.domain == ERHIHeartbeatDomain::Graphics && event.stage == ERHIHeartbeatStage::Submit,
            "stale pre-restart handle mutated the new participant"
        );
    }

    heartbeat.Unregister(second);
    heartbeat.Stop();
    return okay;
}

bool ScopeOwnsRegistrationLifetime() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    TestContext context{.now_ns = 40'000'000'000ull};
    heartbeat.StartForTesting(EnabledConfig(), Hooks(context));
    {
        RHIThreadHeartbeatScope outer(
            ERHIThreadRole::RecordWorker, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::RecordCommands
        );
        if (!Expect(heartbeat.ActiveSlotCountForTesting() == 1, "scope did not register its participant")) {
            heartbeat.Stop();
            return false;
        }
        {
            RHIThreadHeartbeatScope inner(
                ERHIThreadRole::Submission, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::Submit
            );
            if (!Expect(
                    heartbeat.ActiveSlotCountForTesting() == 2, "nested scope did not register independently"
                )) {
                heartbeat.Stop();
                return false;
            }
            heartbeat.PulseCurrent(ERHIHeartbeatStage::NativeSubmit);
        }
        if (!Expect(
                heartbeat.ActiveSlotCountForTesting() == 1, "nested scope did not retire only its own slot"
            )) {
            heartbeat.Stop();
            return false;
        }
        heartbeat.PulseCurrent(ERHIHeartbeatStage::Dispatch);
        context.now_ns += 1'000'000'000ull;
        heartbeat.PollOnceForTesting();
        if (!Expect(
                context.events.size() == 1 && context.events.front().role == ERHIThreadRole::RecordWorker &&
                    context.events.front().stage == ERHIHeartbeatStage::Dispatch,
                "nested scope teardown did not restore PulseCurrent"
            )) {
            heartbeat.Stop();
            return false;
        }
    }

    const bool okay = Expect(
        heartbeat.ActiveSlotCountForTesting() == 0, "scope destructor did not unregister its participant"
    );
    heartbeat.Stop();
    return okay;
}

bool MonitorThreadStopsWithoutWaitingForPollTimeout() {
    using namespace std::chrono;

    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();
    heartbeat.Start(RHIThreadHeartbeatConfig{
        .enabled          = true,
        .stall_timeout_ms = 5000,
        .poll_interval_ms = 5000,
    });
    bool okay =
        Expect(heartbeat.IsMonitorRunningForTesting(), "production Start did not create the monitor thread");
    RHIThreadHeartbeatHandle handle = heartbeat.Register(
        ERHIThreadRole::Completion, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::WaitingForWork
    );
    const auto begin = steady_clock::now();
    heartbeat.Stop();
    const auto elapsed = duration_cast<milliseconds>(steady_clock::now() - begin);
    okay &= Expect(elapsed < milliseconds{1000}, "Stop waited for the configured poll timeout");
    heartbeat.Unregister(handle);
    return okay;
}

bool ConcurrentRegisterPulseAndReuseLeavesNoSlots() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();
    heartbeat.StartForTesting(EnabledConfig(), {});

    std::atomic<std::uint32_t> failures{0};
    std::vector<std::jthread>  workers{};
    workers.reserve(24);
    for (std::uint32_t worker_index = 0; worker_index < 24; ++worker_index) {
        workers.emplace_back([&heartbeat, &failures, worker_index] {
            for (std::uint32_t iteration = 0; iteration < 200; ++iteration) {
                RHIThreadHeartbeatHandle handle = heartbeat.Register(
                    ERHIThreadRole::Translate,
                    (worker_index & 1u) == 0u ? ERHIHeartbeatDomain::Graphics : ERHIHeartbeatDomain::Compute,
                    ERHIHeartbeatStage::Translate
                );
                if (!handle.IsValid()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                heartbeat.Pulse(handle, ERHIHeartbeatStage::Dispatch);
                heartbeat.Pulse(handle, ERHIHeartbeatStage::Translate);
                heartbeat.Unregister(handle);
            }
        });
    }
    workers.clear();

    const bool okay =
        Expect(
            failures.load(std::memory_order_acquire) == 0, "concurrent registration exhausted or lost a slot"
        ) &&
        Expect(heartbeat.ActiveSlotCountForTesting() == 0, "concurrent reuse left an active slot");
    heartbeat.Stop();
    return okay;
}

bool ConcurrentStopWaitsForActivePulseReader() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    StopRaceContext context{};
    context.now_ns.store(50'000'000'000ull, std::memory_order_release);
    heartbeat.StartForTesting(
        EnabledConfig(),
        RHIThreadHeartbeatTestingHooks{
            .now     = ReadStopRaceClock,
            .context = &context,
        }
    );
    RHIThreadHeartbeatHandle handle = heartbeat.Register(
        ERHIThreadRole::Submission, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::Submit
    );

    context.block_next_clock.store(true, std::memory_order_release);
    std::jthread pulser([&] {
        heartbeat.Pulse(handle, ERHIHeartbeatStage::NativeSubmit);
    });
    bool         okay =
        Expect(WaitForTrue(context.pulse_entered), "pulse did not enter the blocked diagnostic clock");

    std::atomic_bool stop_returned{false};
    std::jthread     stopper([&] {
        heartbeat.Stop();
        stop_returned.store(true, std::memory_order_release);
    });
    const auto       disable_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (heartbeat.IsEnabled() && std::chrono::steady_clock::now() < disable_deadline) {
        std::this_thread::yield();
    }
    okay &= Expect(!heartbeat.IsEnabled(), "concurrent Stop did not disable new readers");
    okay &= Expect(
        !stop_returned.load(std::memory_order_acquire),
        "Stop deleted the service while Pulse still held a reader"
    );

    context.release_pulse.store(true, std::memory_order_release);
    pulser.join();
    stopper.join();
    okay &= Expect(
        stop_returned.load(std::memory_order_acquire),
        "Stop did not finish after the active Pulse reader drained"
    );
    heartbeat.Stop();
    return okay;
}

bool StopClosesReaderGateAgainstLateAcquire() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    g_pre_gate_race.block_next_acquire.store(false, std::memory_order_release);
    g_pre_gate_race.acquire_entered.store(false, std::memory_order_release);
    g_pre_gate_race.release_acquire.store(false, std::memory_order_release);

    TestContext context{.now_ns = 60'000'000'000ull};
    heartbeat.StartForTesting(
        EnabledConfig(),
        RHIThreadHeartbeatTestingHooks{
            .now                = ReadFakeClock,
            .sink               = CaptureEvent,
            .before_reader_gate = PauseBeforeReaderGate,
            .context            = &context,
        }
    );
    RHIThreadHeartbeatHandle handle = heartbeat.Register(
        ERHIThreadRole::Submission, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::Submit
    );

    g_pre_gate_race.block_next_acquire.store(true, std::memory_order_release);
    std::jthread pulser([&] {
        heartbeat.Pulse(handle, ERHIHeartbeatStage::NativeSubmit);
    });
    bool         okay =
        Expect(WaitForTrue(g_pre_gate_race.acquire_entered), "pulse did not pause before the reader gate");

    std::atomic_bool stop_returned{false};
    std::jthread     stopper([&] {
        heartbeat.Stop();
        stop_returned.store(true, std::memory_order_release);
    });
    okay &= Expect(WaitForTrue(stop_returned), "Stop treated a pre-gate caller as an acquired reader");
    okay &= Expect(!heartbeat.IsEnabled(), "Stop left the reader gate enabled");

    g_pre_gate_race.release_acquire.store(true, std::memory_order_release);
    pulser.join();
    stopper.join();
    heartbeat.Stop();
    return okay;
}

bool MonitorSnapshotDoesNotMixPulseGenerations() {
    RHIThreadHeartbeat& heartbeat = RHIThreadHeartbeat::Get();
    heartbeat.Stop();

    SnapshotRaceContext context{};
    context.now_ns.store(10'000'000'000ull, std::memory_order_release);
    heartbeat.StartForTesting(
        EnabledConfig(),
        RHIThreadHeartbeatTestingHooks{
            .now                    = ReadRaceClock,
            .sink                   = CountRaceEvent,
            .after_snapshot_payload = PauseAfterSnapshotPayload,
            .context                = &context,
        }
    );
    RHIThreadHeartbeatHandle handle = heartbeat.Register(
        ERHIThreadRole::Submission, ERHIHeartbeatDomain::Graphics, ERHIHeartbeatStage::Submit
    );

    context.now_ns.store(20'000'000'000ull, std::memory_order_release);
    std::jthread pulser([&] {
        while (!context.snapshot_observed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        heartbeat.Pulse(handle, ERHIHeartbeatStage::NativeSubmit);
        context.pulse_completed.store(true, std::memory_order_release);
    });
    heartbeat.PollOnceForTesting();
    pulser.join();

    const bool okay =
        Expect(
            context.snapshot_observed.load(std::memory_order_acquire), "snapshot race hook was not reached"
        ) &&
        Expect(
            context.pulse_completed.load(std::memory_order_acquire), "snapshot race pulse did not complete"
        ) &&
        Expect(
            context.event_count.load(std::memory_order_acquire) == 0,
            "monitor mixed old and new pulse payloads into a false stall"
        );
    heartbeat.Unregister(handle);
    heartbeat.Stop();
    return okay;
}

} // namespace

int main() {
    if (!DisabledIsAZeroServiceFastPath() || !ReportsOneStallThenOneRecovery() ||
        !ParkedStagesDoNotReportFalseStalls() || !GenerationRejectsStaleHandlesAfterReuse() ||
        !GenerationRejectsStaleHandlesAfterRestart() || !ScopeOwnsRegistrationLifetime() ||
        !MonitorThreadStopsWithoutWaitingForPollTimeout() ||
        !ConcurrentRegisterPulseAndReuseLeavesNoSlots() || !ConcurrentStopWaitsForActivePulseReader() ||
        !StopClosesReaderGateAgainstLateAcquire() || !MonitorSnapshotDoesNotMixPulseGenerations()) {
        return EXIT_FAILURE;
    }

    std::cout << "RHI thread heartbeat contract passed\n";
    return EXIT_SUCCESS;
}
