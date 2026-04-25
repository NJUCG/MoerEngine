#include "string/CompileTimeString.h"
#include "string/StringConvert.h"
#include "misc/STL.h"

#include <cassert>
#include <iostream>
#include <type_traits>

using namespace Moer;

namespace {

static_assert(std::is_same_v<Char8, char>);
static_assert(std::is_same_v<Char16, char16_t>);
static_assert(std::is_same_v<Char32, char32_t>);
static_assert(std::is_same_v<WideChar, wchar_t>);

#if defined(_WIN32) || defined(_WIN64)
static_assert(std::is_same_v<PlatformChar, WideChar>);
#else
static_assert(std::is_same_v<PlatformChar, Char8>);
#endif

static_assert(std::is_same_v<String::allocator_type, MoerStlAllocator<Char8>>);
static_assert(std::is_same_v<WideString::allocator_type, MoerStlAllocator<WideChar>>);
static_assert(std::is_same_v<StringView, std::basic_string_view<Char8>>);

constexpr CompileTimeString narrow_literal("narrow");
constexpr CompileTimeString wide_literal(L"wide");
constexpr CompileTimeString platform_literal(MOER_PLATFORM_TEXT_LITERAL("platform"));

static_assert(std::is_same_v<typename decltype(narrow_literal)::CharType, Char8>);
static_assert(std::is_same_v<typename decltype(wide_literal)::CharType, WideChar>);
static_assert(std::is_same_v<typename decltype(platform_literal)::CharType, PlatformChar>);

void TestStringAliases() {
    String text = "Moer";
    text += "Engine";

    StringView view = text;
    assert(view == "MoerEngine");

    PlatformString platform_text = Utf8ToPlatform(view);
    String         utf8_text = PlatformToUtf8(platform_text);
    assert(utf8_text == view);

    std::cout << "[TESTCASE][PASS] StringAliases\n";
}

void TestUnicodeConversion() {
    const String utf8_text = "A\xE4\xB8\xAD\xF0\x9F\x8E\xAE";
    const WideString wide_text = Utf8ToWide(utf8_text);
    const String roundtrip = WideToUtf8(wide_text);

    assert(roundtrip == utf8_text);
    std::cout << "[TESTCASE][PASS] UnicodeConversion\n";
}

void TestInvalidUtf8Rejected() {
    const String invalid_text = "\xC0\xAF";

    bool rejected = false;
    try {
        (void)Utf8ToWide(invalid_text);
    } catch (const std::runtime_error&) {
        rejected = true;
    }

    assert(rejected);
    std::cout << "[TESTCASE][PASS] InvalidUtf8Rejected\n";
}

} // namespace

int main() {
    TestStringAliases();
    TestUnicodeConversion();
    TestInvalidUtf8Rejected();
    return 0;
}
