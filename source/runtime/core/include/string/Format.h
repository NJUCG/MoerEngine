#pragma once

#include "string/String.h"
#include "string/StringConvert.h"

#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <tuple>
#include <utility>

namespace Moer {

namespace Detail {

#if defined(_WIN32) || defined(_WIN64)
inline PlatformString::storage_type ToPlatformStorage(Utf8StringView text) {
    PlatformString platform_text = Utf8ToPlatform(text);
    return std::move(platform_text.Native());
}

inline PlatformString::storage_type ToPlatformFormatArg(const char* text) {
    return ToPlatformStorage(Utf8StringView(text ? text : ""));
}

inline PlatformString::storage_type ToPlatformFormatArg(char* text) {
    return ToPlatformFormatArg(static_cast<const char*>(text));
}

template<std::size_t Size>
inline PlatformString::storage_type ToPlatformFormatArg(const char (&text)[Size]) {
    return ToPlatformStorage(Utf8StringView(text, std::char_traits<char>::length(text)));
}

template<std::size_t Size>
inline PlatformString::storage_type ToPlatformFormatArg(char (&text)[Size]) {
    return ToPlatformStorage(Utf8StringView(text, std::char_traits<char>::length(text)));
}

inline PlatformString::storage_type ToPlatformFormatArg(std::string_view&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(std::string_view& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(const std::string_view& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(const std::string& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(std::string& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(std::string&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(const std::string&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

template<typename Traits, typename Allocator>
inline PlatformString::storage_type ToPlatformFormatArg(
    const std::basic_string<Char8, Traits, Allocator>& text
) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

template<typename Traits, typename Allocator>
inline PlatformString::storage_type ToPlatformFormatArg(std::basic_string<Char8, Traits, Allocator>& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

template<typename Traits, typename Allocator>
inline PlatformString::storage_type ToPlatformFormatArg(std::basic_string<Char8, Traits, Allocator>&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

template<typename Traits>
inline PlatformString::storage_type ToPlatformFormatArg(std::basic_string_view<Char8, Traits>&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

template<typename Traits>
inline PlatformString::storage_type ToPlatformFormatArg(const std::basic_string_view<Char8, Traits>&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

template<typename Traits>
inline PlatformString::storage_type ToPlatformFormatArg(std::basic_string_view<Char8, Traits>& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

template<typename Traits>
inline PlatformString::storage_type ToPlatformFormatArg(const std::basic_string_view<Char8, Traits>& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(Utf8StringView&& text) {
    return ToPlatformStorage(text);
}

inline PlatformString::storage_type ToPlatformFormatArg(const Utf8StringView&& text) {
    return ToPlatformStorage(text);
}

inline PlatformString::storage_type ToPlatformFormatArg(Utf8StringView& text) {
    return ToPlatformStorage(text);
}

inline PlatformString::storage_type ToPlatformFormatArg(const Utf8StringView& text) {
    return ToPlatformStorage(text);
}

inline PlatformString::storage_type ToPlatformFormatArg(const Utf8String& text) {
    return ToPlatformStorage(Utf8StringView(text));
}

inline PlatformString::storage_type ToPlatformFormatArg(Utf8String& text) {
    return ToPlatformStorage(Utf8StringView(text));
}

inline PlatformString::storage_type ToPlatformFormatArg(Utf8String&& text) {
    return ToPlatformStorage(Utf8StringView(text));
}

inline PlatformString::storage_type ToPlatformFormatArg(AsciiStringView&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(const AsciiStringView&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(AsciiStringView& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(const AsciiStringView& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(const AsciiString& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(AsciiString& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::storage_type ToPlatformFormatArg(AsciiString&& text) {
    return ToPlatformStorage(Utf8StringView(text.data(), text.size()));
}

inline PlatformString::native_view_type ToPlatformFormatArg(PlatformStringView text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(PlatformStringView& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(const PlatformStringView& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(const PlatformString& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(PlatformString& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(PlatformString&& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(WideStringView text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(WideStringView& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(const WideStringView& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(const WideString& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(WideString& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(WideString&& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}
#else
inline PlatformString::native_view_type ToPlatformFormatArg(PlatformStringView text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(const PlatformString& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(PlatformString&& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(Utf8StringView text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(const Utf8String& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(Utf8String&& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(AsciiStringView text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(const AsciiString& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}

inline PlatformString::native_view_type ToPlatformFormatArg(AsciiString&& text) {
    return PlatformString::native_view_type(text.data(), text.size());
}
#endif

template<typename T>
inline decltype(auto) ToPlatformFormatArg(T&& value) {
    return std::forward<T>(value);
}

template<typename T>
using PlatformFormatArgType = std::remove_cvref_t<decltype(ToPlatformFormatArg(std::declval<T>()))>;

#if defined(_MSC_VER) && _MSC_VER < 1930
template<typename CharT, typename... Args>
using BasicFormatString = std::_Basic_format_string<CharT, Args...>;
#else
template<typename CharT, typename... Args>
using BasicFormatString = std::basic_format_string<CharT, Args...>;
#endif

} // namespace Detail

template<typename... Args>
using FormatString = Detail::BasicFormatString<PlatformChar, Detail::PlatformFormatArgType<Args>...>;

template<typename... Args>
using Utf8FormatString = Detail::BasicFormatString<Char8, std::type_identity_t<Args>...>;

template<typename... Args>
String Printf(FormatString<Args...> fmt, Args&&... args) {
    return String(std::apply(
        [&](auto&&... converted_args) {
            return std::format(fmt, std::forward<decltype(converted_args)>(converted_args)...);
        },
        std::make_tuple(Detail::ToPlatformFormatArg(std::forward<Args>(args))...)
    ));
}

template<typename... Args>
Utf8String Utf8Printf(Utf8FormatString<Args...> fmt, Args&&... args) {
    return Utf8String(std::format(fmt, std::forward<Args>(args)...));
}

} // namespace Moer
