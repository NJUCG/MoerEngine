#pragma once

#include "misc/STL.h"

#include <string>

namespace Moer::Render {

struct ProfileResultEntry {
    std::string name;
    double      time;
};

struct ProfileData {
    Array<ProfileResultEntry> gpu_entries;
    Array<ProfileResultEntry> cpu_entries;
};

} // namespace Moer::Render
