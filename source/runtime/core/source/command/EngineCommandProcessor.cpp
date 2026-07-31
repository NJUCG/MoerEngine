#include "command/EngineCommandProcessor.h"

#include "config/CVarSystem.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <mutex>
#include <utility>

namespace Moer::Command {

namespace {

struct BuiltinCommand {
    std::string_view name;
    std::string_view helper;
    std::string_view usage;
};

constexpr BuiltinCommand builtin_commands[] = {
    {
        "help",
        "Show engine commands and the default cvar syntax.",
        "/help",
    },
    {
        "cvar.list",
        "List registered cvars, optionally filtered by prefix.",
        "/cvar.list [prefix]",
    },
};

std::string_view TrimView(std::string_view _text) noexcept {
    while (!_text.empty() && std::isspace(static_cast<unsigned char>(_text.front()))) {
        _text.remove_prefix(1);
    }
    while (!_text.empty() && std::isspace(static_cast<unsigned char>(_text.back()))) {
        _text.remove_suffix(1);
    }
    return _text;
}

constexpr char ToLowerAscii(char _character) noexcept {
    return _character >= 'A' && _character <= 'Z' ? static_cast<char>(_character + ('a' - 'A')) : _character;
}

std::string Normalize(std::string_view _text) {
    std::string normalized;
    normalized.reserve(_text.size());
    for (const char character : _text) {
        normalized.push_back(ToLowerAscii(character));
    }
    return normalized;
}

bool StartsWithInsensitive(std::string_view _text, std::string_view _prefix) noexcept {
    if (_prefix.size() > _text.size()) {
        return false;
    }
    for (std::size_t index = 0; index < _prefix.size(); ++index) {
        if (ToLowerAscii(_text[index]) != ToLowerAscii(_prefix[index])) {
            return false;
        }
    }
    return true;
}

const BuiltinCommand* FindBuiltin(std::string_view _name) {
    for (const BuiltinCommand& command : builtin_commands) {
        if (Normalize(_name) == command.name) {
            return &command;
        }
    }
    return nullptr;
}

std::string TypeName(CVar::EType _type) {
    switch (_type) {
        case CVar::EType::Bool:
            return "bool";
        case CVar::EType::Int:
            return "int";
        case CVar::EType::Float:
            return "float";
        case CVar::EType::String:
            return "string";
    }
    return "unknown";
}

std::string FlagsText(CVar::EFlags _flags) {
    std::string flags;
    if (CVar::HasFlag(_flags, CVar::EFlags::ReadOnly)) {
        flags = "read-only";
    }
    if (CVar::HasFlag(_flags, CVar::EFlags::StartupOnly)) {
        if (!flags.empty()) {
            flags += ", ";
        }
        flags += "startup-only";
    }
    return flags;
}

std::string SetErrorText(const CVar::CVarSetResult& _result) {
    if (_result.detail && _result.detail[0] != '\0') {
        return _result.detail;
    }
    switch (_result.status) {
        case CVar::ESetStatus::NotFound:
            return "unknown cvar";
        case CVar::ESetStatus::TypeMismatch:
            return "value type does not match";
        case CVar::ESetStatus::OutOfRange:
            return "value is outside the allowed range";
        case CVar::ESetStatus::ReadOnly:
            return "cvar is read-only";
        case CVar::ESetStatus::StartupSealed:
            return "cvar is sealed after startup";
        case CVar::ESetStatus::InvalidRegistration:
            return "cvar registration is no longer active";
        case CVar::ESetStatus::Changed:
        case CVar::ESetStatus::Unchanged:
            return {};
    }
    return "cvar update failed";
}

} // namespace

struct EngineCommandProcessor::Impl {
    explicit Impl(EngineCommandProcessorLimits _limits) :
        pending_capacity((std::max)(std::size_t{1}, _limits.pending_capacity)),
        output_capacity((std::max)(std::size_t{1}, _limits.output_capacity)) {}

    const std::size_t pending_capacity;
    const std::size_t output_capacity;

    mutable std::mutex      pending_mutex;
    std::deque<std::string> pending_commands;
    std::mutex              process_mutex;

    mutable std::mutex            output_mutex;
    std::deque<CommandOutputLine> output_lines;
    std::uint64_t                 next_output_sequence = 1;
};

EngineCommandProcessor::EngineCommandProcessor(EngineCommandProcessorLimits _limits) :
    impl(std::make_unique<Impl>(_limits)) {}

EngineCommandProcessor::~EngineCommandProcessor() = default;

ESubmitStatus EngineCommandProcessor::SubmitText(std::string_view _text) {
    _text = TrimView(_text);
    if (_text.empty()) {
        return ESubmitStatus::Empty;
    }

    std::lock_guard lock(impl->pending_mutex);
    if (impl->pending_commands.size() >= impl->pending_capacity) {
        return ESubmitStatus::QueueFull;
    }
    impl->pending_commands.emplace_back(_text);
    return ESubmitStatus::Accepted;
}

ProcessPendingResult EngineCommandProcessor::ProcessPending(std::size_t _max_commands) {
    if (_max_commands == 0) {
        return {.status = EProcessStatus::MaxCommandsZero};
    }

    std::unique_lock process_lock(impl->process_mutex, std::try_to_lock);
    if (!process_lock.owns_lock()) {
        return {.status = EProcessStatus::Busy};
    }

    std::size_t command_budget = 0;
    {
        std::lock_guard pending_lock(impl->pending_mutex);
        command_budget = (std::min)(_max_commands, impl->pending_commands.size());
    }
    if (command_budget == 0) {
        return {.status = EProcessStatus::Empty};
    }

    std::size_t processed_count = 0;
    while (processed_count < command_budget) {
        std::string command;
        {
            std::lock_guard pending_lock(impl->pending_mutex);
            if (impl->pending_commands.empty()) {
                break;
            }
            command = std::move(impl->pending_commands.front());
            impl->pending_commands.pop_front();
        }
        ProcessCommand(command);
        ++processed_count;
    }
    return {
        .status    = processed_count == 0 ? EProcessStatus::Empty : EProcessStatus::Processed,
        .processed = processed_count,
    };
}

CommandOutputPollResult EngineCommandProcessor::VisitOutput(
    std::uint64_t        _next_sequence,
    std::size_t          _max_count,
    CommandOutputVisitor _visitor,
    void*                _context
) const {
    CommandOutputPollResult result;
    result.next_sequence = _next_sequence == 0 ? 1 : _next_sequence;
    std::vector<CommandOutputLine> copied_lines;
    {
        std::lock_guard lock(impl->output_mutex);
        if (impl->output_lines.empty()) {
            return result;
        }

        const std::uint64_t first_sequence = impl->output_lines.front().sequence;
        if (result.next_sequence < first_sequence) {
            result.dropped_count = first_sequence - result.next_sequence;
            result.next_sequence = first_sequence;
        }
        if (_max_count == 0 || !_visitor) {
            return result;
        }

        copied_lines.reserve((std::min)(_max_count, impl->output_lines.size()));
        for (const CommandOutputLine& line : impl->output_lines) {
            if (line.sequence < result.next_sequence) {
                continue;
            }
            copied_lines.push_back(line);
            result.next_sequence = line.sequence + 1;
            if (copied_lines.size() == _max_count) {
                break;
            }
        }
    }

    for (const CommandOutputLine& line : copied_lines) {
        _visitor(
            {
                .sequence = line.sequence,
                .text     = line.text,
            },
            _context
        );
    }
    result.visited_count = copied_lines.size();
    return result;
}

std::size_t EngineCommandProcessor::VisitCandidates(
    std::string_view        _input,
    std::size_t             _max_count,
    CommandCandidateVisitor _visitor,
    void*                   _context
) const {
    std::vector<CommandCandidate> candidates;
    if (_max_count == 0 || !_visitor) {
        return 0;
    }

    _input = TrimView(_input);
    if (_input.empty()) {
        return 0;
    }

    if (_input.front() == '/') {
        std::string_view  token     = TrimView(_input.substr(1));
        const std::size_t token_end = token.find_first_of(" \t?");
        if (token_end != std::string_view::npos) {
            token = token.substr(0, token_end);
        }
        for (const BuiltinCommand& command : builtin_commands) {
            if (!token.empty() && !StartsWithInsensitive(command.name, token)) {
                continue;
            }
            candidates.push_back({
                .text       = "/" + std::string(command.name),
                .helper     = std::string(command.helper),
                .is_command = true,
            });
            if (candidates.size() == _max_count) {
                break;
            }
        }
    } else {
        std::string_view  token     = _input;
        const std::size_t token_end = token.find_first_of(" \t=?");
        if (token_end != std::string_view::npos) {
            token = token.substr(0, token_end);
        }
        if (token.empty()) {
            return 0;
        }

        std::vector<CVar::CVarSnapshot> snapshots = CVar::List(token);
        candidates.reserve((std::min)(_max_count, snapshots.size()));
        for (const CVar::CVarSnapshot& snapshot : snapshots) {
            candidates.push_back({
                .text       = snapshot.name,
                .helper     = snapshot.helper,
                .is_command = false,
            });
            if (candidates.size() == _max_count) {
                break;
            }
        }
    }

    for (const CommandCandidate& candidate : candidates) {
        _visitor(
            {
                .text       = candidate.text,
                .helper     = candidate.helper,
                .is_command = candidate.is_command,
            },
            _context
        );
    }
    return candidates.size();
}

void EngineCommandProcessor::ClearOutput() {
    std::lock_guard lock(impl->output_mutex);
    impl->output_lines.clear();
}

void EngineCommandProcessor::ProcessCommand(std::string_view _text) {
    _text = TrimView(_text);
    if (_text.empty()) {
        return;
    }
    if (_text.front() == '/') {
        ProcessSlashCommand(_text);
    } else {
        ProcessDefaultCVar(_text);
    }
}

void EngineCommandProcessor::ProcessSlashCommand(std::string_view _text) {
    std::string_view body       = TrimView(_text.substr(1));
    bool             wants_help = false;
    if (!body.empty() && body.back() == '?') {
        wants_help = true;
        body.remove_suffix(1);
        body = TrimView(body);
    }
    if (body.empty()) {
        AppendOutput("Error: missing command after '/'. Try /help");
        return;
    }

    const std::size_t      name_end = body.find_first_of(" \t");
    const std::string_view command_name =
        name_end == std::string_view::npos ? body : body.substr(0, name_end);
    const std::string_view arguments =
        name_end == std::string_view::npos ? std::string_view{} : TrimView(body.substr(name_end + 1));
    const BuiltinCommand* command = FindBuiltin(command_name);
    if (!command) {
        AppendOutput("Error: unknown command: /" + std::string(command_name));
        return;
    }

    if (wants_help) {
        AppendOutput("/" + std::string(command->name));
        AppendOutput("  help: " + std::string(command->helper));
        AppendOutput("  usage: " + std::string(command->usage));
        return;
    }

    if (command->name == "help") {
        AppendOutput("Commands:");
        for (const BuiltinCommand& builtin : builtin_commands) {
            AppendOutput("  /" + std::string(builtin.name) + " - " + std::string(builtin.helper));
        }
        AppendOutput("CVar syntax:");
        AppendOutput("  <cvar>            show current value");
        AppendOutput("  <cvar> <value>    set value");
        AppendOutput("  <cvar> = <value>  set value");
        AppendOutput("  <cvar> ?          show details");
        return;
    }

    if (command->name == "cvar.list") {
        const std::vector<CVar::CVarSnapshot> snapshots = CVar::List(arguments);
        if (snapshots.empty()) {
            AppendOutput(
                arguments.empty() ? "No cvars registered." :
                                    "No cvar matches prefix: " + std::string(arguments)
            );
            return;
        }
        for (const CVar::CVarSnapshot& snapshot : snapshots) {
            std::string       line = snapshot.name + " (" + TypeName(snapshot.type) + ") = " + snapshot.value;
            const std::string flags = FlagsText(snapshot.flags);
            if (!flags.empty()) {
                line += " [" + flags + "]";
            }
            AppendOutput(line);
        }
    }
}

void EngineCommandProcessor::ProcessDefaultCVar(std::string_view _text) {
    _text                              = TrimView(_text);
    const std::size_t initial_path_end = _text.find_first_of(" \t=");
    bool wants_help = initial_path_end == std::string_view::npos && !_text.empty() && _text.back() == '?';
    if (wants_help) {
        _text.remove_suffix(1);
        _text = TrimView(_text);
    }

    const std::size_t      path_end = _text.find_first_of(" \t=");
    const std::string_view path     = path_end == std::string_view::npos ? _text : _text.substr(0, path_end);
    if (path.empty()) {
        AppendOutput("Error: missing cvar name.");
        return;
    }

    if (!wants_help && path_end != std::string_view::npos) {
        wants_help = TrimView(_text.substr(path_end)) == "?";
    }

    const std::optional<CVar::CVarSnapshot> current = CVar::Find(path);
    if (!current) {
        AppendOutput("Error: unknown cvar: " + std::string(path));
        return;
    }
    if (wants_help) {
        AppendOutput(current->name + " = " + current->value);
        AppendOutput("  type: " + TypeName(current->type));
        if (!current->helper.empty()) {
            AppendOutput("  help: " + current->helper);
        }
        if (current->type == CVar::EType::Bool) {
            if (!current->true_helper.empty()) {
                AppendOutput("  true: " + current->true_helper);
            }
            if (!current->false_helper.empty()) {
                AppendOutput("  false: " + current->false_helper);
            }
        }
        if (current->min_value) {
            AppendOutput("  minimum: " + std::to_string(*current->min_value));
        }
        if (current->max_value) {
            AppendOutput("  maximum: " + std::to_string(*current->max_value));
        }
        const std::string flags = FlagsText(current->flags);
        if (!flags.empty()) {
            AppendOutput("  flags: " + flags);
        }
        return;
    }
    if (path_end == std::string_view::npos) {
        AppendOutput(current->name + " = " + current->value);
        return;
    }

    std::string_view argument        = TrimView(_text.substr(path_end));
    bool             explicit_equals = false;
    if (!argument.empty() && argument.front() == '=') {
        explicit_equals = true;
        argument.remove_prefix(1);
        argument = TrimView(argument);
    }
    if (argument.empty() && !explicit_equals) {
        AppendOutput(current->name + " = " + current->value);
        return;
    }

    const CVar::CVarSetResult result = CVar::SetValueFromString(path, argument);
    if (!result.Succeeded()) {
        AppendOutput("Error: " + current->name + ": " + SetErrorText(result));
        return;
    }

    const std::optional<CVar::CVarSnapshot> updated = CVar::Find(path);
    if (updated) {
        AppendOutput(updated->name + " = " + updated->value);
    } else {
        AppendOutput(current->name + " updated");
    }
    if (result.callback_failed) {
        AppendOutput("Warning: " + current->name + ": on-change callback failed");
    } else if (result.callback_canceled) {
        AppendOutput("Notice: " + current->name + ": on-change callback canceled by unregistration");
    } else if (result.callback_deferred) {
        AppendOutput("Notice: " + current->name + ": on-change callback queued behind an active callback");
    }
}

void EngineCommandProcessor::AppendOutput(std::string_view _text) {
    while (!_text.empty() && (_text.back() == '\r' || _text.back() == '\n')) {
        _text.remove_suffix(1);
    }
    if (_text.empty()) {
        return;
    }

    std::lock_guard lock(impl->output_mutex);
    impl->output_lines.push_back({
        .sequence = impl->next_output_sequence++,
        .text     = std::string(_text),
    });
    while (impl->output_lines.size() > impl->output_capacity) {
        impl->output_lines.pop_front();
    }
}

} // namespace Moer::Command
