#pragma once

#include <utility>

namespace Moer {

template<typename CharT, std::size_t N>
struct CompileTimeString {
    static constexpr std::size_t size = N;
    CharT                        value[N]{};

    constexpr CompileTimeString(const CharT (&str)[N]) {
        for (std::size_t i = 0; i < N; ++i) {
            value[i] = str[i];
        }
    }
};

} // namespace Moer