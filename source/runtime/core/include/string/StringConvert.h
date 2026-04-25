#pragma once

#include "API_Macro.h"
#include "string/String.h"

namespace Moer {

CORE_API PlatformString Utf8ToPlatform(StringView text);
CORE_API String         PlatformToUtf8(PlatformStringView text);
CORE_API WideString     Utf8ToWide(StringView text);
CORE_API String         WideToUtf8(WideStringView text);

} // namespace Moer
