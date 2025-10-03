#pragma once

#include "Traits.h"
#include <type_traits>

namespace Moer {

/**
    * Aligns a value to the nearest higher multiple of 'Alignment', which must be a power of two.
    *
    * @param  _value      The value to align.
    * @param  _alignment  The alignment value, must be a power of two.
    *
    * @return The value aligned up to the specified alignment.
    */
template<typename T>
inline constexpr T AlignUp(T _value, uint64 _alignment) {
    static_assert(std::is_integral_v<T> || std::is_pointer_v<T>, "Align expects an integer or pointer type");

    return (T)((uint64)_value + _alignment - 1) & ~(_alignment - 1);
}

/**
    * Aligns a value to the nearest lower multiple of 'Alignment', which must be a power of two.
    *
    * @param  _value      The value to align.
    * @param  _alignment  The alignment value, must be a power of two.
    *
    * @return The value aligned down to the specified alignment.
    */
template<typename T>
inline constexpr T AlignDown(T _value, uint64 _alignment) {
    static_assert(
        std::is_integral_v<T> || std::is_pointer_v<T>, "AlignDown expects an integer or pointer type"
    );

    return (T)(((uint64)_value) & ~(_alignment - 1));
}

/**
    * Checks if a pointer is aligned to the specified alignment.
    *
    * @param  _value      The value to align.
    * @param  _alignment  The alignment value, must be a power of two.
    *
    * @return true if the pointer is aligned to the specified alignment, false otherwise.
    */
template<typename T>
inline constexpr bool IsAligned(T _value, uint64 _alignment) {
    static_assert(
        std::is_integral_v<T> || std::is_pointer_v<T>, "IsAligned expects an integer or pointer type"
    );

    return !((uint64)_value & (_alignment - 1));
}

/**
    * Aligns a value to the nearest higher multiple of 'Alignment'.
    *
    * @param  _value      The value to align.
    * @param  _alignment  The alignment value, can be any arbitrary value.
    *
    * @return The value aligned up to the specified alignment.
    */
template<typename T>
inline constexpr T AlignArbitrary(T _value, uint64 _alignment) {
    static_assert(
        std::is_integral_v<T> || std::is_pointer_v<T>, "AlignArbitrary expects an integer or pointer type"
    );

    return (T)((((uint64)_value + _alignment - 1) / _alignment) * _alignment);
}

} // namespace Moer