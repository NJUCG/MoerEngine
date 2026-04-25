#include "config/CVarSystem.h"

#include "log/LogSystem.h"

#include <algorithm>
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

    auto [iter, inserted] = data.cvars.insert_or_assign(std::string(cvar->GetName()), cvar);
    (void)iter;
    return inserted;
}

ICVar* Find(std::string_view name) {
    auto& data = GetRegistryData();
    std::lock_guard lock(data.mutex);

    auto iter = data.cvars.find(std::string(name));
    return iter != data.cvars.end() ? iter->second : nullptr;
}

void VisitAll(ICVarVisitor visitor, void* user_data) {
    if (!visitor) {
        return;
    }

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

    for (ICVar* cvar : out) {
        visitor(cvar, user_data);
    }
}

bool ApplyValueMap(const UnorderedMap<std::string, std::string>& values, std::string_view source_name) {
    if (values.empty()) {
        return false;
    }

    const std::string source_label = source_name.empty() ? "config" : std::string(source_name);

    Array<std::string> ordered_keys;
    ordered_keys.reserve(values.size());
    for (const auto& [key, value] : values) {
        ordered_keys.push_back(key);
    }
    std::sort(ordered_keys.begin(), ordered_keys.end());

    bool applied_any = false;
    for (const std::string& key : ordered_keys) {
        const auto value_iter = values.find(key);
        if (value_iter == values.end()) {
            continue;
        }

        ICVar* cvar = Find(key);
        if (!cvar) {
            LOG_WARNING(MOER_TEXT("Ignore unknown cvar override `{}` from {}."), key, source_label);
            continue;
        }

        if (const char* error = cvar->SetValueFromString(value_iter->second, ESetSource::StartupConfig)) {
            LOG_WARNING(
                MOER_TEXT("Failed to apply cvar override `{}` = `{}` from {}: {}"),
                key,
                value_iter->second,
                source_label,
                error
            );
            continue;
        }

        char value_buffer[256]{};
        cvar->CopyValueString(value_buffer, sizeof(value_buffer));
        LOG_INFO(MOER_TEXT("Applied cvar override from {}: {} = {}"), source_label, key, value_buffer);
        applied_any = true;
    }

    return applied_any;
}

void SealStartupConfigReadOnlyCVars() {
    auto& data = GetRegistryData();
    std::lock_guard lock(data.mutex);

    for (auto& [name, cvar] : data.cvars) {
        (void)name;
        if (cvar) {
            cvar->SealStartupConfig();
        }
    }
}

} // namespace Moer::CVar
