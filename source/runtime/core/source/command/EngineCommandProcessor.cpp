#include "command/EngineCommandProcessor.h"

#include <algorithm>
#include <cctype>
#include <mutex>

#include "config/CVarSystem.h"

namespace Moer::Command {
namespace {

struct BuiltinCommand {
    const char* name;
    const char* helper;
    const char* usage;
};

constexpr BuiltinCommand k_builtin_commands[] = {
    {"help", "Show available engine commands and default cvar syntax.", "/help"},
    {"cvar.list", "List registered cvars. Optional argument filters by prefix.", "/cvar.list [prefix]"},
};

struct CollectCVarListContext {
    Array<std::string>* lines      = nullptr;
    std::string_view    prefix;
    bool                use_prefix = false;
    int                 count      = 0;
};

struct CollectCandidateContext {
    std::string_view        token;
    CommandCandidateVisitor visitor     = nullptr;
    void*                   user_data   = nullptr;
    size_t                  remaining   = 0;
    bool                    visited_any = false;
};

std::string Trim(std::string_view in) {
    size_t begin = 0;
    while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) {
        ++begin;
    }

    size_t end = in.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
        --end;
    }

    return std::string(in.substr(begin, end - begin));
}

std::string ToLower(std::string_view in) {
    std::string out(in);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

bool StartsWithInsensitive(std::string_view text, std::string_view prefix) {
    if (prefix.size() > text.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[i])));
        const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool TrimTrailingHelperQuery(std::string_view text, std::string& out_trimmed) {
    out_trimmed = Trim(text);
    if (out_trimmed.empty() || out_trimmed.back() != '?') {
        return false;
    }

    out_trimmed.pop_back();
    out_trimmed = Trim(out_trimmed);
    return true;
}

const BuiltinCommand* FindBuiltinCommand(std::string_view name) {
    const std::string lowered = ToLower(name);
    for (const BuiltinCommand& command : k_builtin_commands) {
        if (lowered == command.name) {
            return &command;
        }
    }
    return nullptr;
}

std::string CVarTypeName(CVar::EType type) {
    switch (type) {
        case CVar::EType::Bool:
            return "bool";
        case CVar::EType::Int:
            return "int";
        case CVar::EType::Float:
            return "float";
        case CVar::EType::String:
            return "string";
        default:
            return "unknown";
    }
}

std::string GetCVarValueText(const CVar::ICVar* cvar) {
    if (!cvar) {
        return {};
    }

    char buffer[512]{};
    cvar->CopyValueString(buffer, sizeof(buffer));
    return buffer;
}

void AppendCVarListEntry(CVar::ICVar* cvar, void* user_data) {
    auto* context = static_cast<CollectCVarListContext*>(user_data);
    if (!context || !context->lines || !cvar) {
        return;
    }

    const std::string name(cvar->GetName());
    if (context->use_prefix && !StartsWithInsensitive(name, context->prefix)) {
        return;
    }

    context->lines->push_back(
        name + " (" + CVarTypeName(cvar->GetType()) + ") = " + GetCVarValueText(cvar)
    );
    ++context->count;
}

void AppendCVarCandidate(CVar::ICVar* cvar, void* user_data) {
    auto* context = static_cast<CollectCandidateContext*>(user_data);
    if (!context || !context->visitor || !cvar || context->remaining == 0) {
        return;
    }

    const std::string_view name = cvar->GetName();
    if (!context->token.empty() && !StartsWithInsensitive(name, context->token)) {
        return;
    }

    context->visitor(CommandCandidateView{name, cvar->GetHelper(), false}, context->user_data);
    context->visited_any = true;
    --context->remaining;
}

struct OutputEntry {
    uint64_t    sequence = 0;
    std::string text;
};

} // namespace

struct EngineCommandProcessor::PendingStorage {
    std::mutex           mutex;
    DEQueue<std::string> commands;
};

struct EngineCommandProcessor::OutputStorage {
    std::mutex            mutex;
    DEQueue<OutputEntry>  entries;
    uint64_t              next_sequence = 1;
};

EngineCommandProcessor::~EngineCommandProcessor() {
    if (m_pending_storage) {
        MoerDelete(m_pending_storage);
        m_pending_storage = nullptr;
    }
    if (m_output_storage) {
        MoerDelete(m_output_storage);
        m_output_storage = nullptr;
    }
}

void EngineCommandProcessor::SubmitText(std::string_view text) {
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) {
        return;
    }

    if (!m_pending_storage) {
        m_pending_storage = MoerNew(PendingStorage)();
    }

    std::lock_guard lock(m_pending_storage->mutex);
    m_pending_storage->commands.push_back(trimmed);
}

EngineCommandProcessor& EngineCommandProcessor::operator<<(std::string_view text) {
    SubmitText(text);
    return *this;
}

bool EngineCommandProcessor::ProcessPending(size_t max_commands_per_frame) {
    if (max_commands_per_frame == 0) {
        return false;
    }

    if (!m_pending_storage) {
        return false;
    }

    Array<std::string> commands;
    {
        std::lock_guard lock(m_pending_storage->mutex);
        const size_t command_count = (std::min)(max_commands_per_frame, m_pending_storage->commands.size());
        commands.reserve(command_count);
        for (size_t i = 0; i < command_count; ++i) {
            commands.push_back(std::move(m_pending_storage->commands.front()));
            m_pending_storage->commands.pop_front();
        }
    }

    for (const std::string& command : commands) {
        ProcessCommand(command);
    }
    return !commands.empty();
}

bool EngineCommandProcessor::PollOutput(
    uint64_t             &next_sequence,
    CommandOutputVisitor  visitor,
    void*                 user_data,
    size_t                max_count
) {
    if (!visitor || max_count == 0 || !m_output_storage) {
        return false;
    }

    std::lock_guard lock(m_output_storage->mutex);
    if (m_output_storage->entries.empty()) {
        return false;
    }

    const uint64_t first_sequence = m_output_storage->entries.front().sequence;
    if (next_sequence < first_sequence) {
        next_sequence = first_sequence;
    }

    bool visited_any = false;
    for (const OutputEntry& entry : m_output_storage->entries) {
        if (entry.sequence < next_sequence) {
            continue;
        }
        visitor(CommandOutputLineView{entry.sequence, entry.text}, user_data);
        next_sequence = entry.sequence + 1;
        visited_any = true;
        if (--max_count == 0) {
            break;
        }
    }

    return visited_any;
}

bool EngineCommandProcessor::VisitCandidates(
    std::string_view        input,
    CommandCandidateVisitor visitor,
    void*                   user_data,
    size_t                  max_count
) const {
    if (!visitor || max_count == 0) {
        return false;
    }

    const std::string trimmed = Trim(input);
    if (trimmed.empty()) {
        return false;
    }

    bool visited_any = false;
    if (trimmed.front() == '/') {
        std::string token = Trim(std::string_view(trimmed).substr(1));
        const size_t token_end = token.find_first_of(" \t?");
        if (token_end != std::string::npos) {
            token.resize(token_end);
        }

        for (const BuiltinCommand& command : k_builtin_commands) {
            if (!token.empty() && !StartsWithInsensitive(command.name, token)) {
                continue;
            }

            const std::string command_text = "/" + std::string(command.name);
            visitor(CommandCandidateView{command_text, command.helper, true}, user_data);
            visited_any = true;
            if (--max_count == 0) {
                break;
            }
        }
        return visited_any;
    }

    std::string token = trimmed;
    const size_t token_end = token.find_first_of(" \t?");
    if (token_end != std::string::npos) {
        token.resize(token_end);
    }

    CollectCandidateContext context{token, visitor, user_data, max_count, false};
    CVar::VisitAll(&AppendCVarCandidate, &context);
    return context.visited_any;
}

void EngineCommandProcessor::ClearOutput() {
    if (!m_output_storage) {
        return;
    }
    std::lock_guard lock(m_output_storage->mutex);
    m_output_storage->entries.clear();
}

void EngineCommandProcessor::ProcessCommand(std::string_view text) {
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) {
        return;
    }

    if (trimmed.front() == '/') {
        ProcessSlashCommand(trimmed);
        return;
    }

    ProcessDefaultCVar(trimmed);
}

void EngineCommandProcessor::ProcessSlashCommand(std::string_view text) {
    std::string helper_target;
    const bool wants_help = TrimTrailingHelperQuery(text, helper_target);
    const std::string body = wants_help ? Trim(std::string_view(helper_target).substr(1)) : Trim(std::string_view(text).substr(1));
    if (body.empty()) {
        AppendOutput("Error: Missing command after '/'. Try /help");
        return;
    }

    const size_t first_space = body.find_first_of(" \t");
    const std::string command_name = first_space == std::string::npos ? body : body.substr(0, first_space);
    const std::string arguments = first_space == std::string::npos ? std::string{} : Trim(body.substr(first_space + 1));

    const BuiltinCommand* command = FindBuiltinCommand(command_name);
    if (!command) {
        AppendOutput(std::string("Error: Unknown command: /") + command_name);
        return;
    }

    if (wants_help) {
        AppendCommandHelp(command->name, command->helper, command->usage);
        return;
    }

    if (command_name == "help") {
        AppendOutput("Commands:");
        for (const BuiltinCommand& builtin : k_builtin_commands) {
            AppendOutput(std::string("  /") + builtin.name + " - " + builtin.helper);
        }
        AppendOutput("CVar:");
        AppendOutput("  <cvar>          show current value");
        AppendOutput("  <cvar> <value>  set value");
        AppendOutput("  <cvar> ?        show helper text");
        return;
    }

    if (command_name == "cvar.list") {
        Array<std::string> lines;
        CollectCVarListContext context{
            .lines = &lines,
            .prefix = arguments,
            .use_prefix = !arguments.empty(),
            .count = 0,
        };
        CVar::VisitAll(&AppendCVarListEntry, &context);
        for (const std::string& line_text : lines) {
            AppendOutput(line_text);
        }
        if (context.count == 0) {
            AppendOutput(std::string("No cvar matches prefix: ") + arguments);
        }
        return;
    }
}

void EngineCommandProcessor::ProcessDefaultCVar(std::string_view text) {
    std::string helper_target;
    std::string expression = Trim(text);
    const bool wants_help = TrimTrailingHelperQuery(expression, helper_target);
    if (wants_help) {
        expression = helper_target;
    }

    const size_t first_space = expression.find_first_of(" \t");
    const std::string path = first_space == std::string::npos ? expression : expression.substr(0, first_space);
    if (path.empty()) {
        AppendOutput("Error: Missing cvar name.");
        return;
    }

    CVar::ICVar* cvar = CVar::Find(path);
    if (!cvar) {
        AppendOutput(std::string("Error: Unknown cvar: ") + path);
        return;
    }

    if (wants_help) {
        AppendOutput(path + " = " + GetCVarValueText(cvar));
        if (!cvar->GetHelper().empty()) {
            AppendOutput("  help: " + std::string(cvar->GetHelper()));
        }
        if (cvar->GetType() == CVar::EType::Bool) {
            if (!cvar->GetTrueHelper().empty()) {
                AppendOutput("  true: " + std::string(cvar->GetTrueHelper()));
            }
            if (!cvar->GetFalseHelper().empty()) {
                AppendOutput("  false: " + std::string(cvar->GetFalseHelper()));
            }
        }
        return;
    }

    if (first_space == std::string::npos) {
        AppendOutput(path + " = " + GetCVarValueText(cvar));
        return;
    }

    const std::string argument = Trim(expression.substr(first_space + 1));
    if (argument.empty()) {
        AppendOutput(path + " = " + GetCVarValueText(cvar));
        return;
    }

    if (const char* error = cvar->SetValueFromString(argument)) {
        AppendOutput(std::string("Error: ") + error);
        return;
    }

    AppendOutput(path + " = " + GetCVarValueText(cvar));
}

void EngineCommandProcessor::AppendCommandHelp(
    std::string_view command_name,
    std::string_view helper,
    std::string_view usage
) {
    AppendOutput(std::string("/") + std::string(command_name));
    if (!helper.empty()) {
        AppendOutput("  help: " + std::string(helper));
    }
    if (!usage.empty()) {
        AppendOutput("  usage: " + std::string(usage));
    }
}

void EngineCommandProcessor::AppendOutput(std::string_view text) {
    if (!m_output_storage) {
        m_output_storage = MoerNew(OutputStorage)();
    }

    std::string line(text);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    if (line.empty()) {
        return;
    }

    std::lock_guard lock(m_output_storage->mutex);
    m_output_storage->entries.push_back(OutputEntry{
        .sequence = m_output_storage->next_sequence++,
        .text = std::move(line),
    });

    constexpr size_t k_max_output_entries = 2048;
    while (m_output_storage->entries.size() > k_max_output_entries) {
        m_output_storage->entries.pop_front();
    }
}

} // namespace Moer::Command