#include "log/LogSystem.h"
#include "scripting/PythonRuntimeConfig.h"
#include "scripting/ScriptExecutionRequest.h"
#include "scripting/ScriptHost.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct TestCase {
    const char*                           label;
    Moer::scripting::EScriptRequestOrigin origin;
    Moer::scripting::EScriptSessionPolicy session_policy;
    std::string                           session_id;
    std::string                           code;
    bool                                  expect_success;
    std::string_view                      expected_stdout;
};

std::filesystem::path ResolveExecutablePath(const char* argv0) {
    return std::filesystem::path(argv0);
}

Moer::scripting::PythonRuntimeConfig BuildRuntimeConfig(const char* argv0) {
    const std::filesystem::path executable_path = ResolveExecutablePath(argv0);
    const std::filesystem::path executable_dir  = executable_path.parent_path();

    Moer::scripting::PythonRuntimeConfig config;
    config.runtime_root = executable_dir;
    config.program_path = executable_path;
    config.stdlib_dir   = executable_dir / "Lib";
    config.dll_dir      = executable_dir / "DLLs";
    return config;
}

bool ValidateResult(const Moer::scripting::ScriptExecutionResult& result, const TestCase& test_case) {
    if (result.success != test_case.expect_success) {
        std::cerr << "[cpp] " << test_case.label
                  << " failed: success mismatch. actual=" << (result.success ? "true" : "false")
                  << ", expected=" << (test_case.expect_success ? "true" : "false") << std::endl;
        if (!result.exception_text.empty()) {
            std::cerr << "[cpp] exception: " << result.exception_text << std::endl;
        }
        return false;
    }

    if (result.stdout_text != test_case.expected_stdout) {
        std::cerr << "[cpp] " << test_case.label << " failed: stdout mismatch. actual='" << result.stdout_text
                  << "', expected='" << test_case.expected_stdout << "'" << std::endl;
        return false;
    }

    if (!test_case.expect_success && result.exception_text.empty()) {
        std::cerr << "[cpp] " << test_case.label
                  << " failed: expected failure result to carry exception text." << std::endl;
        return false;
    }

    if (!result.stderr_text.empty()) {
        std::cout << "[cpp] " << test_case.label << " stderr:\n" << result.stderr_text << std::endl;
    }

    return true;
}

} // namespace

int main(int argc, const char** argv) {
    std::cout << "Moer Engine Scripting Session Policy Test Starting..." << std::endl;

    if (argc <= 0) {
        std::cerr << "[cpp] Invalid argc." << std::endl;
        return 10;
    }

    Moer::LogSystem::Init();

    Moer::scripting::PythonRuntimeConfig runtime_config = BuildRuntimeConfig(argv[0]);
    Moer::scripting::ScriptHost          script_host(std::move(runtime_config));
    script_host.Start();

    const TestCase test_cases[] = {
        {
            .label           = "shared-global-write",
            .origin          = Moer::scripting::EScriptRequestOrigin::EditorUiPanel,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::SharedGlobal,
            .session_id      = "",
            .code            = "x = 123\nprint('shared-write')\n",
            .expect_success  = true,
            .expected_stdout = "shared-write\n",
        },
        {
            .label           = "shared-global-read",
            .origin          = Moer::scripting::EScriptRequestOrigin::EditorUiPanel,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::SharedGlobal,
            .session_id      = "",
            .code            = "print(x)\n",
            .expect_success  = true,
            .expected_stdout = "123\n",
        },
        {
            .label           = "named-session-a-write",
            .origin          = Moer::scripting::EScriptRequestOrigin::Terminal,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::NamedSession,
            .session_id      = "terminal-A",
            .code            = "y = 7\nprint('named-a-write')\n",
            .expect_success  = true,
            .expected_stdout = "named-a-write\n",
        },
        {
            .label           = "named-session-a-read",
            .origin          = Moer::scripting::EScriptRequestOrigin::Terminal,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::NamedSession,
            .session_id      = "terminal-A",
            .code            = "print(y)\n",
            .expect_success  = true,
            .expected_stdout = "7\n",
        },
        {
            .label           = "named-session-b-isolated",
            .origin          = Moer::scripting::EScriptRequestOrigin::Terminal,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::NamedSession,
            .session_id      = "terminal-B",
            .code            = "print('present' if 'y' in globals() else 'missing')\n",
            .expect_success  = true,
            .expected_stdout = "missing\n",
        },
        {
            .label           = "stateless-write",
            .origin          = Moer::scripting::EScriptRequestOrigin::Mcp,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::Stateless,
            .session_id      = "",
            .code            = "z = 1\nprint('stateless-write')\n",
            .expect_success  = true,
            .expected_stdout = "stateless-write\n",
        },
        {
            .label           = "stateless-read-missing",
            .origin          = Moer::scripting::EScriptRequestOrigin::Mcp,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::Stateless,
            .session_id      = "",
            .code            = "print('present' if 'z' in globals() else 'missing')\n",
            .expect_success  = true,
            .expected_stdout = "missing\n",
        },
        {
            .label           = "named-session-missing-id",
            .origin          = Moer::scripting::EScriptRequestOrigin::Terminal,
            .session_policy  = Moer::scripting::EScriptSessionPolicy::NamedSession,
            .session_id      = "",
            .code            = "print('should-not-run')\n",
            .expect_success  = false,
            .expected_stdout = "",
        },
    };

    int exit_code = 0;

    for (size_t index = 0; index < std::size(test_cases); ++index) {
        const TestCase& test_case = test_cases[index];

        Moer::scripting::ScriptExecutionRequest request;
        request.origin         = test_case.origin;
        request.session_policy = test_case.session_policy;
        request.session_id     = test_case.session_id;
        request.source_name    = test_case.label;
        request.code           = test_case.code;

        Moer::scripting::ScriptExecutionFuture       future = script_host.Submit(std::move(request));
        const Moer::scripting::ScriptExecutionResult result = future.get();

        if (!ValidateResult(result, test_case)) {
            exit_code = 1;
            break;
        }

        std::cout << "[cpp] " << test_case.label << " passed." << std::endl;
    }

    script_host.Stop();
    return exit_code;
}
