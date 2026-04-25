#include "misc/Assert.h"

#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "profile/ProfileDump.h"

#include <atomic>

namespace Moer::Diagnostics {

namespace {

std::atomic<CrashFlushHook> g_crash_flush_hook{nullptr};
std::atomic<bool>           g_ensure_failure_escalation{false};
std::atomic<bool>           g_has_ensure_failures{false};
std::atomic<bool>           g_handling_fatal{false};

const char* ToString(EFailureKind kind) {
    switch (kind) {
        case EFailureKind::Assert:
            return "assert";
        case EFailureKind::Ensure:
            return "ensure";
    }
    return "unknown";
}

void FlushCrashState() {
    LogSystem::Flush();
    if (CrashFlushHook hook = g_crash_flush_hook.load(std::memory_order_acquire); hook != nullptr) {
        hook();
    }
    ProfileDump::FlushThreadLocal();
    LogSystem::Flush();
}

} // namespace

void SetCrashFlushHook(CrashFlushHook hook) {
    g_crash_flush_hook.store(hook, std::memory_order_release);
}

void SetEnsureFailureEscalation(bool enabled) {
    g_ensure_failure_escalation.store(enabled, std::memory_order_release);
}

bool HasEnsureFailures() {
    return g_has_ensure_failures.load(std::memory_order_acquire);
}

void ResetEnsureFailures() {
    g_has_ensure_failures.store(false, std::memory_order_release);
}

[[noreturn]] void HandleAssertFailure(FailureInfo&& info) noexcept {
    if (g_handling_fatal.exchange(true, std::memory_order_acq_rel)) {
        Platform::FailFast("recursive fatal failure");
    }

    LOG_CRITICAL(
        MOER_TEXT("[MOER_ASSERT] expr=`{}` file={} line={} function={} thread={} message={}"),
        info.expression ? info.expression : "",
        info.file ? info.file : "",
        info.line,
        info.function ? info.function : "",
        info.thread_id,
        info.message
    );
    FlushCrashState();

    const PlatformStackTrace stack = Platform::CaptureStackTrace(2);
    const std::string        stack_text = Platform::FormatStackTrace(stack);
    const auto dump = Platform::WriteCrashDump(PlatformCrashDumpRequest{
        .failure_kind = ToString(info.kind),
        .message = info.message,
        .thread_id = info.thread_id,
    });

    if (dump.written) {
        LOG_CRITICAL(MOER_TEXT("[MOER_ASSERT] dump={}"), dump.path.generic_string());
    } else if (!dump.error_message.empty()) {
        LOG_CRITICAL(MOER_TEXT("[MOER_ASSERT] dump_write_failed={}"), dump.error_message);
    }

    if (!stack_text.empty()) {
        LOG_CRITICAL(MOER_TEXT("[MOER_ASSERT] stack:\n{}"), stack_text);
    }
    FlushCrashState();

    Platform::FailFast(info.message);
}

bool HandleEnsureFailure(FailureInfo&& info) noexcept {
    g_has_ensure_failures.store(true, std::memory_order_release);

    LOG_WARNING(
        MOER_TEXT("[MOER_ENSURE] expr=`{}` file={} line={} function={} thread={} message={}"),
        info.expression ? info.expression : "",
        info.file ? info.file : "",
        info.line,
        info.function ? info.function : "",
        info.thread_id,
        info.message
    );

    if (g_ensure_failure_escalation.load(std::memory_order_acquire)) {
        LOG_ERROR(
            MOER_TEXT("[MOER_ENSURE_ESCALATED] expr=`{}` file={} line={} function={} thread={} message={}"),
            info.expression ? info.expression : "",
            info.file ? info.file : "",
            info.line,
            info.function ? info.function : "",
            info.thread_id,
            info.message
        );
        LogSystem::Flush();
    }

    return false;
}

} // namespace Moer::Diagnostics