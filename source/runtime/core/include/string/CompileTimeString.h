#pragma once

#include <cstddef>

namespace Moer {

template<typename CharT, std::size_t N>
struct CompileTimeString {
    using CharType = CharT;

    static constexpr std::size_t size = N;
    CharT                        value[N]{};

    constexpr CompileTimeString(const CharT (&str)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }
};

template<typename CharT, std::size_t N>
CompileTimeString(const CharT (&)[N]) -> CompileTimeString<CharT, N>;

} // namespace Moer
