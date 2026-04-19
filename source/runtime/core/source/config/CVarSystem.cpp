#include "config/CVarSystem.h"

#include "log/LogSystem.h"

#include <fstream>
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

std::string TrimIniText(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }

    size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    return std::string(text.substr(begin, end - begin));
}

std::string StripInlineComment(std::string_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if ((c == ';' || c == '#') && (i == 0 || std::isspace(static_cast<unsigned char>(text[i - 1])))) {
            return TrimIniText(text.substr(0, i));
        }
    }
    return TrimIniText(text);
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

bool ApplyIniFile(const std::filesystem::path& file_path) {
    if (!std::filesystem::exists(file_path)) {
        return false;
    }

    std::ifstream file(file_path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open cvar override file `{}`.", file_path.generic_string());
        return false;
    }

    bool        in_console_section = false;
    bool        saw_console_section = false;
    bool        applied_any = false;
    std::string raw_line;
    size_t      line_number = 0;

    while (std::getline(file, raw_line)) {
        ++line_number;
        const std::string trimmed = TrimIniText(raw_line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            const std::string section_name = TrimIniText(
                std::string_view(trimmed).substr(1, trimmed.size() - 2)
            );
            in_console_section = section_name == "ConsoleVariables";
            saw_console_section = saw_console_section || in_console_section;
            continue;
        }

        if (!in_console_section) {
            continue;
        }

        const size_t equal_pos = trimmed.find('=');
        if (equal_pos == std::string::npos) {
            LOG_WARNING(
                "Ignore malformed cvar override line {} in `{}`: {}",
                line_number,
                file_path.generic_string(),
                trimmed
            );
            continue;
        }

        const std::string key = TrimIniText(std::string_view(trimmed).substr(0, equal_pos));
        const std::string value = StripInlineComment(std::string_view(trimmed).substr(equal_pos + 1));
        if (key.empty()) {
            LOG_WARNING(
                "Ignore empty cvar name on line {} in `{}`.",
                line_number,
                file_path.generic_string()
            );
            continue;
        }

        ICVar* cvar = Find(key);
        if (!cvar) {
            LOG_WARNING("Ignore unknown cvar override `{}` from `{}`.", key, file_path.generic_string());
            continue;
        }

        if (const char* error = cvar->SetValueFromString(value)) {
            LOG_WARNING(
                "Failed to apply cvar override `{}` = `{}` from `{}`: {}",
                key,
                value,
                file_path.generic_string(),
                error
            );
            continue;
        }

        char value_buffer[256]{};
        cvar->CopyValueString(value_buffer, sizeof(value_buffer));
        LOG_INFO("Applied cvar override: {} = {}", key, value_buffer);
        applied_any = true;
    }

    if (!saw_console_section) {
        LOG_WARNING(
            "Console variable ini `{}` does not contain [ConsoleVariables].",
            file_path.generic_string()
        );
        return false;
    }

    return applied_any;
}

} // namespace Moer::CVar
