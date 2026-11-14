#ifndef MOER_ENGINE_PROFILE_DUMP_TESTING_H
#define MOER_ENGINE_PROFILE_DUMP_TESTING_H

#include "API_Macro.h"

#include "misc/Types.h"
#include <cstddef>

namespace Moer::ProfileDump::Testing {

enum class FaultPoint : uint8 {
    None = 0,
    StartAllocation,
    BeforeThreadCreate,
    ShutdownFinalizeAllocation,
    OpenTempFile,
    WritePacket,
    FlushFile,
    CloseFile,
    RenameFinal,
    WriterException,
};

// Hooks may only be changed while the runtime is stopped. The matching fault
// is injected once, on the requested one-based hit.
CORE_API bool ConfigureFault(FaultPoint _point, uint64 _trigger_hit = 1) noexcept;

CORE_API bool ConfigureWriterPauseBeforeTempOpen(bool _enabled) noexcept;
CORE_API bool ConfigureWriterPauseAfterStart(bool _enabled) noexcept;
CORE_API bool ConfigureWriterPauseBeforeIdleWait(bool _enabled) noexcept;
CORE_API bool ConfigureCrashFlushPauseBeforePublish(bool _enabled) noexcept;
CORE_API bool ConfigureCrashFlushPauseAfterCompletionObserved(bool _enabled) noexcept;
CORE_API bool ConfigureEmitterPauseAfterAdmission(bool _enabled) noexcept;
CORE_API bool WaitForWriterPaused(uint32 _timeout_ms) noexcept;
CORE_API bool WaitForWriterIdleWaitPaused(uint32 _timeout_ms) noexcept;
CORE_API bool WaitForCrashFlushBeforePublishPaused(uint32 _timeout_ms) noexcept;
CORE_API bool WaitForCrashFlushAfterCompletionObservedPaused(
    uint32 _timeout_ms
) noexcept;
CORE_API bool WaitForCrashFlushRequestPending(uint32 _timeout_ms) noexcept;
CORE_API bool WaitForEmitterPaused(uint32 _timeout_ms) noexcept;
CORE_API bool CreateActiveTempCollision(const uint8* _bytes, std::size_t _byte_count) noexcept;
CORE_API void ResumeWriter() noexcept;
CORE_API void ResumeWriterIdleWait() noexcept;
CORE_API void ResumeCrashFlushBeforePublish() noexcept;
CORE_API void ResumeCrashFlushAfterCompletionObserved() noexcept;
CORE_API void ResumeEmitter() noexcept;
CORE_API void ClearHooks() noexcept;

} // namespace Moer::ProfileDump::Testing

#endif // MOER_ENGINE_PROFILE_DUMP_TESTING_H
