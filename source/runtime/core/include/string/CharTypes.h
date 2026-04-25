#pragma once

namespace Moer {

using Char8     = char;
using Char16    = char16_t;
using Char32    = char32_t;
using WideChar  = wchar_t;
using AsciiChar = char;
using Utf8Char  = Char8;
using Utf16Char = Char16;
using Utf32Char = Char32;

#if defined(_WIN32) || defined(_WIN64)
using PlatformChar = WideChar;
#else
using PlatformChar = Utf8Char;
#endif

} // namespace Moer

#if defined(_WIN32) || defined(_WIN64)
#define MOER_PLATFORM_TEXT_LITERAL(literal) L##literal
#else
#define MOER_PLATFORM_TEXT_LITERAL(literal) literal
#endif

#define MOER_TEXT(literal)       MOER_PLATFORM_TEXT_LITERAL(literal)
#define MOER_ASCII_TEXT(literal) literal
