#pragma once

#include <limits>

namespace Moer {

// Numeric upper bound; callers give sentinel values their domain-specific names.
template<typename T>
    requires std::numeric_limits<T>::is_specialized
inline constexpr T MaxValue = (std::numeric_limits<T>::max)();

} // namespace Moer
