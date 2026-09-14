#pragma once

#include "API_Macro.h"
#include "config/CVarSystem.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace Moer::CVar {

struct OverrideIssueView {
    std::string_view name;
    std::string_view detail;
    ESetStatus status = ESetStatus::NotFound;
};

struct OverrideReport {
    std::size_t applied_count = 0;
    std::size_t issue_count   = 0;

    [[nodiscard]] bool Succeeded() const noexcept {
        return issue_count == 0;
    }
};

using OverrideIssueCallback = void (*)(const OverrideIssueView& _issue, void* _context);

// Applies [cvars] first and then [profiles.<profile>.cvars]. Values must be
// scalar TOML values. Dotted cvar names are written as quoted TOML keys.
CORE_API OverrideReport
ApplyOverridesFromTomlFile(
    std::string_view      _path,
    std::string_view      _profile = {},
    OverrideIssueCallback _on_issue = nullptr,
    void*                 _context = nullptr
);
CORE_API OverrideReport
ApplyOverridesFromToml(
    std::string_view      _toml,
    std::string_view      _profile = {},
    OverrideIssueCallback _on_issue = nullptr,
    void*                 _context = nullptr
);

// Each command-line assignment uses Name=Value syntax.
CORE_API OverrideReport ApplyCommandLineOverrides(
    std::span<const std::string> _assignments,
    OverrideIssueCallback        _on_issue = nullptr,
    void*                        _context = nullptr
);

} // namespace Moer::CVar
