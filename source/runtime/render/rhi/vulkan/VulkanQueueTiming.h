#pragma once

#include <chrono>

namespace Moer::Render {

inline double RhiThreadProfileMilliseconds(
    std::chrono::steady_clock::time_point _begin,
    std::chrono::steady_clock::time_point _end
) {
    return std::chrono::duration<double, std::milli>(_end - _begin).count();
}

} // namespace Moer::Render
