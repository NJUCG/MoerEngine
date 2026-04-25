#pragma once

#include "string/String.h"

#include <format>
#include <type_traits>
#include <utility>

namespace Moer {

template<typename... Args>
using FormatString = std::basic_format_string<PlatformChar, std::type_identity_t<Args>...>;

template<typename... Args>
using Utf8FormatString = std::basic_format_string<Utf8Char, std::type_identity_t<Args>...>;

template<typename... Args>
String Printf(FormatString<Args...> fmt, Args&&... args) {
    return String(std::format(fmt, std::forward<Args>(args)...));
}

template<typename... Args>
Utf8String Utf8Printf(Utf8FormatString<Args...> fmt, Args&&... args) {
    return Utf8String(std::format(fmt, std::forward<Args>(args)...));
}

} // namespace Moer
