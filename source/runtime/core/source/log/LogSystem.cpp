#include "log/LogSystem.h"

namespace Moer::LogSystem {

void Init() {

#if !defined(NDEBUG)
    spdlog::set_level(spdlog::level::trace);
#endif
}

} // namespace Moer::LogSystem