#ifndef MOER_WINDOWS_CRASH_DIAGNOSTICS_H
#define MOER_WINDOWS_CRASH_DIAGNOSTICS_H

#include "platform/Platform.h"

#include <cstdint>

bool InitializeWindowsCrashDiagnostics() noexcept;

PlatformCrashArtifactResult
SubmitWindowsCrashArtifacts(const PlatformCrashArtifactRequest& _request, std::uint32_t _timeout_ms) noexcept;

#endif // MOER_WINDOWS_CRASH_DIAGNOSTICS_H
