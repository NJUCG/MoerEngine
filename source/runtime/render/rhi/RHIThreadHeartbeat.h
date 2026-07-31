#pragma once

#include "RenderAPI.h"
#include "rhi/RHIThreadOwnership.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace Moer::Render {

// Backend-neutral queue/lifecycle domain attached to a heartbeat participant.
// Keep this set fixed: worker hot paths publish compact IDs and the monitor
// resolves them to text only when it emits a diagnostic.
enum class ERHIHeartbeatDomain : std::uint8_t {
    General,
    Graphics,
    Compute,
    Copy,
    Present,
};

// A stage describes the last boundary crossed by an RHI owner. WaitingForWork
// and Shutdown are deliberately parked states: an idle service thread is
// healthy even when it does not pulse for longer than the stall timeout.
enum class ERHIHeartbeatStage : std::uint8_t {
    Registered,
    WaitingForWork,
    Dispatch,
    WaitForDependency,
    RecordCommands,
    Translate,
    WaitForPipelineCapacity,
    Submit,
    NativeSubmit,
    Present,
    PollCompletion,
    AwaitCompletion,
    Shutdown,
};

enum class ERHIHeartbeatEventKind : std::uint8_t {
    Stalled,
    Recovered,
};

struct RHIThreadHeartbeatConfig {
    bool          enabled{false};
    std::uint32_t stall_timeout_ms{5000};
    std::uint32_t poll_interval_ms{1000};
};

struct RHIThreadHeartbeatHandle {
    std::uint32_t slot_index{UINT32_MAX};
    std::uint64_t generation{0};

    [[nodiscard]] bool IsValid() const noexcept {
        return slot_index != UINT32_MAX && generation != 0;
    }
};

struct RHIThreadHeartbeatEvent {
    ERHIHeartbeatEventKind kind{ERHIHeartbeatEventKind::Stalled};
    ERHIThreadRole         role{ERHIThreadRole::Unknown};
    ERHIHeartbeatDomain    domain{ERHIHeartbeatDomain::General};
    ERHIHeartbeatStage     stage{ERHIHeartbeatStage::Registered};
    std::uint32_t          thread_id{0};
    std::uint32_t          slot_index{UINT32_MAX};
    std::uint64_t          generation{0};
    std::uint64_t          progress_sequence{0};
    std::uint64_t          stale_ms{0};
};

using RHIHeartbeatNowFunction = std::uint64_t (*)(void* _context) noexcept;
using RHIHeartbeatEventSink   = void (*)(const RHIThreadHeartbeatEvent& _event, void* _context) noexcept;
using RHIHeartbeatReaderAcquireHook = void (*)() noexcept;
using RHIHeartbeatSnapshotHook      = void (*)(std::uint32_t _slot_index, void* _context) noexcept;

struct RHIThreadHeartbeatTestingHooks {
    RHIHeartbeatNowFunction       now{nullptr};
    RHIHeartbeatEventSink         sink{nullptr};
    RHIHeartbeatReaderAcquireHook before_reader_gate{nullptr};
    RHIHeartbeatSnapshotHook      after_snapshot_payload{nullptr};
    void*                         context{nullptr};
};

RENDER_API const char* RHIHeartbeatDomainName(ERHIHeartbeatDomain _domain) noexcept;
RENDER_API const char* RHIHeartbeatStageName(ERHIHeartbeatStage _stage) noexcept;
RENDER_API bool        IsRHIHeartbeatParkedStage(ERHIHeartbeatStage _stage) noexcept;

// Fixed-capacity RHI watchdog. Disabled is the production default: no Impl is
// allocated, no monitor thread is created, and Register/Pulse return before
// taking a lock or querying the clock. Enabled Pulse uses a per-slot atomic
// writer token; registration/reuse alone takes the registry mutex.
class RENDER_API RHIThreadHeartbeat final {
public:
    static RHIThreadHeartbeat& Get();

    void Start(RHIThreadHeartbeatConfig _config) noexcept;
    void Stop() noexcept;

    [[nodiscard]] bool IsEnabled() const noexcept;

    [[nodiscard]] RHIThreadHeartbeatHandle Register(
        ERHIThreadRole      _role,
        ERHIHeartbeatDomain _domain,
        ERHIHeartbeatStage  _stage = ERHIHeartbeatStage::Registered
    ) noexcept;
    void Pulse(RHIThreadHeartbeatHandle _handle, ERHIHeartbeatStage _stage) noexcept;
    void PulseCurrent(ERHIHeartbeatStage _stage) noexcept;
    void Unregister(RHIThreadHeartbeatHandle& _handle) noexcept;

    // Deterministic CPU-only contract seam. This starts the same reducer and
    // slot implementation without a monitor thread; the caller advances its
    // injected clock and invokes PollOnceForTesting explicitly.
    void StartForTesting(RHIThreadHeartbeatConfig _config, RHIThreadHeartbeatTestingHooks _hooks);
    void PollOnceForTesting() noexcept;
    [[nodiscard]] bool          IsMonitorRunningForTesting() const noexcept;
    [[nodiscard]] std::uint32_t ActiveSlotCountForTesting() const noexcept;

private:
    RHIThreadHeartbeat() = default;
    ~RHIThreadHeartbeat();

    RHIThreadHeartbeat(const RHIThreadHeartbeat&)            = delete;
    RHIThreadHeartbeat& operator=(const RHIThreadHeartbeat&) = delete;

    void StartInternal(
        RHIThreadHeartbeatConfig       _config,
        RHIThreadHeartbeatTestingHooks _hooks,
        bool                           _spawn_monitor
    ) noexcept;
    void MonitorLoop() noexcept;
    void PollOnce() noexcept;

    struct Impl;
    [[nodiscard]] Impl* AcquireImplRead() const noexcept;
    void                ReleaseImplRead() const noexcept;
    void                WaitForImplReaders() noexcept;

    static constexpr std::uint64_t kImplReadersStoppingBit = std::uint64_t{1} << 63;
    static constexpr std::uint64_t kImplReaderCountMask    = ~kImplReadersStoppingBit;

    std::atomic<Impl*>                         impl{nullptr};
    std::atomic_bool                           enabled{false};
    mutable std::atomic<std::uint64_t>         impl_reader_state{kImplReadersStoppingBit};
    std::atomic<RHIHeartbeatReaderAcquireHook> before_reader_gate_hook{nullptr};
    std::atomic<std::uint64_t>                 next_generation{1};
    std::mutex                                 lifecycle_mutex{};
};

class RENDER_API RHIThreadHeartbeatScope final {
public:
    explicit RHIThreadHeartbeatScope(
        ERHIThreadRole      _role,
        ERHIHeartbeatDomain _domain = ERHIHeartbeatDomain::General,
        ERHIHeartbeatStage  _stage  = ERHIHeartbeatStage::Registered
    ) noexcept;
    ~RHIThreadHeartbeatScope();

    RHIThreadHeartbeatScope(const RHIThreadHeartbeatScope&)            = delete;
    RHIThreadHeartbeatScope& operator=(const RHIThreadHeartbeatScope&) = delete;
    RHIThreadHeartbeatScope(RHIThreadHeartbeatScope&&)                 = delete;
    RHIThreadHeartbeatScope& operator=(RHIThreadHeartbeatScope&&)      = delete;

    void Pulse(ERHIHeartbeatStage _stage) noexcept;

private:
    RHIThreadHeartbeatHandle previous_handle{};
    RHIThreadHeartbeatHandle handle{};
};

} // namespace Moer::Render
