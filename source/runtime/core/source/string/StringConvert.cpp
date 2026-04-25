#include "string/StringConvert.h"

#include <limits>
#include <stdexcept>

namespace Moer {
namespace {

constexpr char32_t kMaxCodePoint = 0x10FFFF;
constexpr char32_t kHighSurrogateStart = 0xD800;
constexpr char32_t kHighSurrogateEnd = 0xDBFF;
constexpr char32_t kLowSurrogateStart = 0xDC00;
constexpr char32_t kLowSurrogateEnd = 0xDFFF;

bool IsSurrogate(char32_t value) {
    return value >= kHighSurrogateStart && value <= kLowSurrogateEnd;
}

[[noreturn]] void RejectInvalidText() {
    throw std::runtime_error("Invalid Unicode text.");
}

char32_t DecodeUtf8CodePoint(StringView text, std::size_t& index) {
    const auto read_byte = [&](std::size_t offset) -> unsigned char {
        if (index + offset >= text.size()) {
            RejectInvalidText();
        }
        return static_cast<unsigned char>(text[index + offset]);
    };

    const unsigned char first = read_byte(0);
    char32_t            code_point = 0;
    std::size_t         length = 0;

    if (first <= 0x7F) {
        code_point = first;
        length = 1;
    } else if (first >= 0xC2 && first <= 0xDF) {
        code_point = first & 0x1F;
        length = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        code_point = first & 0x0F;
        length = 3;
    } else if (first >= 0xF0 && first <= 0xF4) {
        code_point = first & 0x07;
        length = 4;
    } else {
        RejectInvalidText();
    }

    for (std::size_t i = 1; i < length; ++i) {
        const unsigned char next = read_byte(i);
        if ((next & 0xC0) != 0x80) {
            RejectInvalidText();
        }
        code_point = (code_point << 6) | (next & 0x3F);
    }

    if ((length == 3 && code_point < 0x800) || (length == 4 && code_point < 0x10000)) {
        RejectInvalidText();
    }
    if (code_point > kMaxCodePoint || IsSurrogate(code_point)) {
        RejectInvalidText();
    }

    index += length;
    return code_point;
}

void AppendUtf8CodePoint(String& output, char32_t code_point) {
    if (code_point > kMaxCodePoint || IsSurrogate(code_point)) {
        RejectInvalidText();
    }

    if (code_point <= 0x7F) {
        output.push_back(static_cast<Char8>(code_point));
    } else if (code_point <= 0x7FF) {
        output.push_back(static_cast<Char8>(0xC0 | (code_point >> 6)));
        output.push_back(static_cast<Char8>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        output.push_back(static_cast<Char8>(0xE0 | (code_point >> 12)));
        output.push_back(static_cast<Char8>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<Char8>(0x80 | (code_point & 0x3F)));
    } else {
        output.push_back(static_cast<Char8>(0xF0 | (code_point >> 18)));
        output.push_back(static_cast<Char8>(0x80 | ((code_point >> 12) & 0x3F)));
        output.push_back(static_cast<Char8>(0x80 | ((code_point >> 6) & 0x3F)));
        output.push_back(static_cast<Char8>(0x80 | (code_point & 0x3F)));
    }
}

void AppendWideCodePoint(WideString& output, char32_t code_point) {
    if constexpr (sizeof(WideChar) == 2) {
        if (code_point <= 0xFFFF) {
            output.push_back(static_cast<WideChar>(code_point));
            return;
        }

        code_point -= 0x10000;
        output.push_back(static_cast<WideChar>(kHighSurrogateStart + (code_point >> 10)));
        output.push_back(static_cast<WideChar>(kLowSurrogateStart + (code_point & 0x3FF)));
    } else {
        if (code_point > static_cast<char32_t>(std::numeric_limits<WideChar>::max())) {
            RejectInvalidText();
        }
        output.push_back(static_cast<WideChar>(code_point));
    }
}

char32_t DecodeWideCodePoint(WideStringView text, std::size_t& index) {
    const char32_t first = static_cast<char32_t>(text[index++]);

    if constexpr (sizeof(WideChar) == 2) {
        if (first >= kHighSurrogateStart && first <= kHighSurrogateEnd) {
            if (index >= text.size()) {
                RejectInvalidText();
            }
            const char32_t second = static_cast<char32_t>(text[index++]);
            if (second < kLowSurrogateStart || second > kLowSurrogateEnd) {
                RejectInvalidText();
            }
            return 0x10000 + (((first - kHighSurrogateStart) << 10) | (second - kLowSurrogateStart));
        }
        if (first >= kLowSurrogateStart && first <= kLowSurrogateEnd) {
            RejectInvalidText();
        }
    }

    if (first > kMaxCodePoint || IsSurrogate(first)) {
        RejectInvalidText();
    }
    return first;
}

} // namespace

PlatformString Utf8ToPlatform(StringView text) {
#if defined(_WIN32) || defined(_WIN64)
    return Utf8ToWide(text);
#else
    return PlatformString(text);
#endif
}

String PlatformToUtf8(PlatformStringView text) {
#if defined(_WIN32) || defined(_WIN64)
    return WideToUtf8(text);
#else
    return String(text);
#endif
}

WideString Utf8ToWide(StringView text) {
    WideString output;
    output.reserve(text.size());

    std::size_t index = 0;
    while (index < text.size()) {
        AppendWideCodePoint(output, DecodeUtf8CodePoint(text, index));
    }

    return output;
}

String WideToUtf8(WideStringView text) {
    String output;
    output.reserve(text.size());

    std::size_t index = 0;
    while (index < text.size()) {
        AppendUtf8CodePoint(output, DecodeWideCodePoint(text, index));
    }

    return output;
}

} // namespace Moer
