#ifndef MOER_WINDOWS_CRASH_DIAGNOSTICS_TESTING_H
#define MOER_WINDOWS_CRASH_DIAGNOSTICS_TESTING_H

#include "API_Macro.h"

namespace Moer::PlatformTesting {

CORE_API bool ConfigureCrashWorkerPauseBeforeDump(bool _enabled) noexcept;

} // namespace Moer::PlatformTesting

#endif // MOER_WINDOWS_CRASH_DIAGNOSTICS_TESTING_H
