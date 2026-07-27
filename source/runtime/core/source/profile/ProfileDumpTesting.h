#ifndef MOER_ENGINE_PROFILE_DUMP_TESTING_H
#define MOER_ENGINE_PROFILE_DUMP_TESTING_H

#include "API_Macro.h"

#include <cstddef>
#include <cstdint>

namespace Moer::ProfileDump::Testing {

enum class FaultPoint : std::uint8_t {
    None = 0,
    StartAllocation,
    BeforeThreadCreate,
    OpenTempFile,
    WritePacket,
    FlushFile,
    CloseFile,
    RenameFinal,
    WriterException,
};

// Hooks may only be changed while the runtime is stopped. The matching fault
// is injected once, on the requested one-based hit.
CORE_API bool ConfigureFault(FaultPoint _point, std::uint64_t _trigger_hit = 1) noexcept;

CORE_API bool ConfigureWriterPauseBeforeTempOpen(bool _enabled) noexcept;
CORE_API bool ConfigureWriterPauseAfterStart(bool _enabled) noexcept;
CORE_API bool WaitForWriterPaused(std::uint32_t _timeout_ms) noexcept;
CORE_API bool CreateActiveTempCollision(const std::uint8_t* _bytes, std::size_t _byte_count) noexcept;
CORE_API void ResumeWriter() noexcept;
CORE_API void ClearHooks() noexcept;

} // namespace Moer::ProfileDump::Testing

#endif // MOER_ENGINE_PROFILE_DUMP_TESTING_H
