#include "misc/Assert.h"

#include "log/LogSystem.h"
#include "profile/ProfileDump.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace Moer::Diagnostics {

namespace {

enum class EFatalProgress : std::uint8_t {
    Idle,
    OwnerElected,
    PrefixFlushed,
    ArtifactAttemptFinished,
};

std::atomic<std::uint32_t>  g_fatal_owner_thread_id{0};
std::atomic<EFatalProgress> g_fatal_progress{EFatalProgress::Idle};
std::atomic_bool            g_has_ensure_failures{false};

constexpr std::uint32_t kCrashFlushTimeoutMs   = 200;
constexpr std::uint32_t kConcurrentFatalWaitMs = 1500;

std::string_view FailureKindName(EFailureKind _kind) noexcept {
    switch (_kind) {
        case EFailureKind::Assert:
            return "assert";
        case EFailureKind::Ensure:
            return "ensure";
    }
    return "unknown";
}

std::string_view CrashFlushResultName(ProfileDump::CrashFlushResult _result) noexcept {
    switch (_result) {
        case ProfileDump::CrashFlushResult::Completed:
            return "completed";
        case ProfileDump::CrashFlushResult::NotRunning:
            return "not_running";
        case ProfileDump::CrashFlushResult::Faulted:
            return "faulted";
        case ProfileDump::CrashFlushResult::Busy:
            return "busy";
        case ProfileDump::CrashFlushResult::TimedOut:
            return "timed_out";
    }
    return "unknown";
}

} // namespace

namespace Detail {

void ClaimAssertFailureOwnership(std::uint32_t _thread_id) noexcept {
    std::uint32_t expected = 0;
    if (g_fatal_owner_thread_id.compare_exchange_strong(
            expected, _thread_id, std::memory_order_acq_rel, std::memory_order_acquire
        )) {
        g_fatal_progress.store(EFatalProgress::OwnerElected, std::memory_order_release);
        return;
    }

    if (expected == _thread_id) {
        Platform::FailFast("recursive controlled fatal");
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kConcurrentFatalWaitMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_fatal_progress.load(std::memory_order_acquire) >= EFatalProgress::ArtifactAttemptFinished) {
            break;
        }
        std::this_thread::yield();
    }
    Platform::FailFast("concurrent controlled fatal");
}

} // namespace Detail

bool HasEnsureFailures() noexcept {
    return g_has_ensure_failures.load(std::memory_order_acquire);
}

void ResetEnsureFailures() noexcept {
    g_has_ensure_failures.store(false, std::memory_order_release);
}

[[noreturn]] void HandleAssertFailure(const FailureInfo& _info) noexcept {
    const std::uint32_t current_thread_id = Platform::GetCurrentThreadID();
    if (g_fatal_owner_thread_id.load(std::memory_order_acquire) != current_thread_id) {
        Detail::ClaimAssertFailureOwnership(current_thread_id);
    }

    const ProfileDump::CrashFlushResult profile_flush =
        ProfileDump::FlushCrashPublishedPrefix(kCrashFlushTimeoutMs);
    g_fatal_progress.store(EFatalProgress::PrefixFlushed, std::memory_order_release);
    const PlatformStackTrace stack = Platform::CaptureStackTrace(2);
    static_cast<void>(Platform::WriteCrashArtifacts(PlatformCrashArtifactRequest{
        .failure_kind         = FailureKindName(_info.kind),
        .expression           = _info.expression ? _info.expression : "",
        .file                 = _info.file ? _info.file : "",
        .function             = _info.function ? _info.function : "",
        .message              = _info.message.View(),
        .profile_flush_status = CrashFlushResultName(profile_flush),
        .line                 = _info.line,
        .thread_id            = _info.thread_id,
        .message_truncated    = _info.message.truncated,
        .stack                = stack,
    }));
    g_fatal_progress.store(EFatalProgress::ArtifactAttemptFinished, std::memory_order_release);

    const std::string_view reason =
        _info.message.View().empty() ? std::string_view("controlled fatal") : _info.message.View();
    Platform::FailFast(reason);
}

bool HandleEnsureFailure(const FailureInfo& _info) noexcept {
    g_has_ensure_failures.store(true, std::memory_order_release);
    try {
        LOG_WARNING(
            "[MOER_ENSURE] expr=`{}` file={} line={} function={} thread={} "
            "message={}",
            _info.expression ? _info.expression : "",
            _info.file ? _info.file : "",
            _info.line,
            _info.function ? _info.function : "",
            _info.thread_id,
            _info.message.View()
        );
    } catch (...) {
        // Ensure remains non-fatal even if the configured logger rejects the
        // diagnostic or is already tearing down.
    }
    return false;
}

} // namespace Moer::Diagnostics
