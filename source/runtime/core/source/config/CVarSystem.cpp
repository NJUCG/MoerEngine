#include "config/CVarSystem.h"

#include <mutex>

namespace Moer::CVar {

namespace {
struct RegistryData {
    std::mutex                 mutex;
    UnorderedMap<std::string, ICVar*> cvars;
};

RegistryData& GetRegistryData() {
    static RegistryData data;
    return data;
}
} // namespace

bool Register(ICVar* cvar) {
    if (!cvar) {
        return false;
    }

    auto& data = GetRegistryData();
    std::lock_guard lock(data.mutex);

    auto [iter, inserted] = data.cvars.insert_or_assign(cvar->GetName(), cvar);
    (void)iter;
    return inserted;
}

ICVar* Find(std::string_view name) {
    auto& data = GetRegistryData();
    std::lock_guard lock(data.mutex);

    auto iter = data.cvars.find(std::string(name));
    return iter != data.cvars.end() ? iter->second : nullptr;
}

Array<ICVar*> GetAll() {
    auto& data = GetRegistryData();
    std::lock_guard lock(data.mutex);

    Array<ICVar*> out;
    out.reserve(data.cvars.size());
    for (auto& [name, cvar] : data.cvars) {
        out.push_back(cvar);
    }

    std::sort(out.begin(), out.end(), [](const ICVar* lhs, const ICVar* rhs) {
        return lhs && rhs ? lhs->GetName() < rhs->GetName() : lhs < rhs;
    });
    return out;
}

} // namespace Moer::CVar
