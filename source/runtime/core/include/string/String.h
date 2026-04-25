#pragma once

#include "misc/MMemory.h"
#include "string/CharTypes.h"

#include <string>
#include <string_view>

namespace Moer {

template<
    typename CharT,
    typename Traits    = std::char_traits<CharT>,
    typename Allocator = MoerStlAllocator<CharT>>
using BasicString = std::basic_string<CharT, Traits, Allocator>;

template<typename CharT, typename Traits = std::char_traits<CharT>>
using BasicStringView = std::basic_string_view<CharT, Traits>;

using String       = BasicString<Char8>;
using StringView   = BasicStringView<Char8>;
using NarrowString = BasicString<NarrowChar>;
using NarrowStringView = BasicStringView<NarrowChar>;
using Utf8String   = BasicString<Utf8Char>;
using Utf8StringView = BasicStringView<Utf8Char>;
using Utf16String  = BasicString<Utf16Char>;
using Utf16StringView = BasicStringView<Utf16Char>;
using Utf32String  = BasicString<Utf32Char>;
using Utf32StringView = BasicStringView<Utf32Char>;
using WideString   = BasicString<WideChar>;
using WideStringView = BasicStringView<WideChar>;
using PlatformString = BasicString<PlatformChar>;
using PlatformStringView = BasicStringView<PlatformChar>;

} // namespace Moer
