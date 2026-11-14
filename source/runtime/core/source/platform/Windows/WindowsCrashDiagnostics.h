#ifndef MOER_WINDOWS_CRASH_DIAGNOSTICS_H
#define MOER_WINDOWS_CRASH_DIAGNOSTICS_H

#include "platform/Platform.h"

#include "misc/Types.h"

bool InitializeWindowsCrashDiagnostics() noexcept;

PlatformCrashArtifactResult
SubmitWindowsCrashArtifacts(const PlatformCrashArtifactRequest& _request, Moer::uint32 _timeout_ms) noexcept;

#endif // MOER_WINDOWS_CRASH_DIAGNOSTICS_H
