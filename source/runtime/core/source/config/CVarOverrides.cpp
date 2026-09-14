#include "config/CVarOverrides.h"

#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <toml++/toml.hpp>

namespace Moer::CVar {

namespace {

std::string_view Trim(std::string_view _text) {
    const std::size_t first = _text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    return _text.substr(first, _text.find_last_not_of(" \t\r\n") - first + 1);
}

std::optional<std::string> ScalarText(const toml::node& _node) {
    if (const auto* value = _node.as_string()) {
        return value->get();
    }
    if (const auto* value = _node.as_boolean()) {
        return value->get() ? "true" : "false";
    }
    if (const auto* value = _node.as_integer()) {
        return std::to_string(value->get());
    }
    if (const auto* value = _node.as_floating_point()) {
        std::ostringstream text;
        text << std::setprecision(std::numeric_limits<double>::max_digits10) << value->get();
        return text.str();
    }
    return std::nullopt;
}

void ReportIssue(
    OverrideReport&       _report,
    OverrideIssueCallback _on_issue,
    void*                 _context,
    std::string_view      _name,
    std::string_view      _detail,
    ESetStatus            _status
) {
    ++_report.issue_count;
    if (_on_issue) {
        _on_issue({_name, _detail, _status}, _context);
    }
}

void ApplyTable(
    const toml::table&    _table,
    ESetSource            _source,
    OverrideReport&       _report,
    OverrideIssueCallback _on_issue,
    void*                 _context
) {
    for (const auto& [key, node] : _table) {
        const std::optional<std::string> value = ScalarText(node);
        if (!value) {
            ReportIssue(
                _report,
                _on_issue,
                _context,
                key.str(),
                "cvar override must be a scalar TOML value",
                ESetStatus::TypeMismatch
            );
            continue;
        }

        const CVarSetResult result = SetValueFromString(key.str(), *value, _source);
        if (result.Succeeded()) {
            ++_report.applied_count;
        } else {
            ReportIssue(
                _report,
                _on_issue,
                _context,
                key.str(),
                result.detail ? result.detail : "cvar override failed",
                result.status
            );
        }
    }
}

OverrideReport ApplyTomlTable(
    const toml::table&    _root,
    std::string_view      _profile,
    OverrideIssueCallback _on_issue,
    void*                 _context
) {
    OverrideReport report;
    if (const toml::table* cvars = _root["cvars"].as_table()) {
        ApplyTable(*cvars, ESetSource::StartupConfig, report, _on_issue, _context);
    }

    if (!_profile.empty()) {
        const toml::table* profiles = _root["profiles"].as_table();
        const toml::node*  profile_node = profiles ? profiles->get(_profile) : nullptr;
        const toml::table* profile      = profile_node ? profile_node->as_table() : nullptr;
        const toml::table* cvars    = profile ? (*profile)["cvars"].as_table() : nullptr;
        if (!cvars) {
            ReportIssue(
                report,
                _on_issue,
                _context,
                _profile,
                "named cvar profile was not found",
                ESetStatus::NotFound
            );
        } else {
            ApplyTable(*cvars, ESetSource::Profile, report, _on_issue, _context);
        }
    }
    return report;
}

} // namespace

OverrideReport ApplyOverridesFromTomlFile(
    std::string_view      _path,
    std::string_view      _profile,
    OverrideIssueCallback _on_issue,
    void*                 _context
) {
    return ApplyTomlTable(toml::parse_file(std::string(_path)), _profile, _on_issue, _context);
}

OverrideReport ApplyOverridesFromToml(
    std::string_view      _toml,
    std::string_view      _profile,
    OverrideIssueCallback _on_issue,
    void*                 _context
) {
    return ApplyTomlTable(toml::parse(_toml), _profile, _on_issue, _context);
}

OverrideReport ApplyCommandLineOverrides(
    std::span<const std::string> _assignments,
    OverrideIssueCallback        _on_issue,
    void*                        _context
) {
    OverrideReport report;
    for (const std::string& assignment : _assignments) {
        const std::size_t separator = assignment.find('=');
        const std::string_view name = Trim(std::string_view(assignment).substr(0, separator));
        const std::string_view value = separator == std::string::npos ? std::string_view{} :
                                                                       Trim(std::string_view(assignment).substr(separator + 1));
        if (separator == std::string::npos || name.empty()) {
            ReportIssue(
                report,
                _on_issue,
                _context,
                assignment,
                "command-line cvar override must use Name=Value",
                ESetStatus::TypeMismatch
            );
            continue;
        }

        const CVarSetResult result = SetValueFromString(name, value, ESetSource::CommandLine);
        if (result.Succeeded()) {
            ++report.applied_count;
        } else {
            ReportIssue(
                report,
                _on_issue,
                _context,
                name,
                result.detail ? result.detail : "command-line cvar override failed",
                result.status
            );
        }
    }
    return report;
}

} // namespace Moer::CVar
