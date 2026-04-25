#pragma once

#include "API_Macro.h"
#include "string/String.h"

namespace Moer {

CORE_API PlatformString Utf8ToPlatform(Utf8StringView text);
CORE_API Utf8String     PlatformToUtf8(PlatformStringView text);
CORE_API WideString     Utf8ToWide(Utf8StringView text);
CORE_API Utf8String     WideToUtf8(WideStringView text);

} // namespace Moer
