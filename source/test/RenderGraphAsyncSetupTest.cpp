#include "rendergraph/RenderGraphPassParameters.h"
#include "rendergraph/RenderGraphSetup.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Moer::Render {

class RenderGraphAsyncSetupTestAccess {
public:
    static void InjectBatchOwnerFailure(RenderGraph& graph) noexcept {
        AddFault(graph, RenderGraph::SetupFaultForTesting::BatchOwnerCreate);
    }

    static void InjectTaskDispatchFailure(RenderGraph& graph) noexcept {
        AddFault(graph, RenderGraph::SetupFaultForTesting::TaskDispatchThrows);
    }

    static void InjectFailureDiagnosticFailure(RenderGraph& graph) noexcept {
        AddFault(graph, RenderGraph::SetupFaultForTesting::FailureDiagnostic);
    }

private:
    static void AddFault(
        RenderGraph&                      graph,
        RenderGraph::SetupFaultForTesting fault
    ) noexcept {
        graph.setup_faults_for_testing |= static_cast<uint8_t>(fault);
    }
};

} // namespace Moer::Render

namespace {

using Moer::Render::CommandList;
using Moer::Render::RenderGraph;

class TestSuite {
public:
    void Check(
        bool             condition,
        std::string_view test_name,
        std::string_view expectation
    ) {
        if (condition) {
            return;
        }
        ++failure_count;
        std::cerr << "[FAIL] " << test_name << ": " << expectation << '\n';
    }

    [[nodiscard]] int FailureCount() const {
        return failure_count;
    }

private:
    int failure_count = 0;
};

[[nodiscard]] bool Contains(std::string_view text, std::string_view fragment) {
    return text.find(fragment) != std::string_view::npos;
}

struct ChildProcessResult {
    bool started   = false;
    bool timed_out = false;
    int  exit_code = -1;
};

#if defined(_WIN32)
[[nodiscard]] std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> path(1024);
    for (;;) {
        const DWORD length =
            ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            return {};
        }
        if (length < path.size() - 1) {
            return std::wstring(path.data(), length);
        }
        path.resize(path.size() * 2);
    }
}

[[nodiscard]] ChildProcessResult RunStarvationChild(const char*) {
    const std::wstring executable = CurrentExecutablePath();
    if (executable.empty()) {
        return {};
    }
    std::wstring command =
        L"\"" + executable + L"\" --worker-starvation-child";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    if (::CreateProcessW(
            executable.c_str(),
            mutable_command.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process
        ) == FALSE) {
        return {};
    }

    ChildProcessResult result{.started = true};
    const DWORD        wait_result = ::WaitForSingleObject(process.hProcess, 5000);
    if (wait_result != WAIT_OBJECT_0) {
        result.timed_out = wait_result == WAIT_TIMEOUT;
        static_cast<void>(::TerminateProcess(process.hProcess, 0xEE));
        static_cast<void>(::WaitForSingleObject(process.hProcess, 2000));
    }
    DWORD exit_code = 0;
    if (::GetExitCodeProcess(process.hProcess, &exit_code) != FALSE) {
        result.exit_code = static_cast<int>(exit_code);
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return result;
}
#else
[[nodiscard]] ChildProcessResult RunStarvationChild(const char* argv0) {
    const std::filesystem::path executable = std::filesystem::absolute(argv0);
    const pid_t                 child      = ::fork();
    if (child < 0) {
        return {};
    }
    if (child == 0) {
        ::execl(
            executable.c_str(),
            executable.c_str(),
            "--worker-starvation-child",
            static_cast<char*>(nullptr)
        );
        ::_exit(127);
    }

    ChildProcessResult result{.started = true};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    int        status   = 0;
    for (;;) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) :
                                                   128 + WTERMSIG(status);
            return result;
        }
        if (waited < 0 && errno != EINTR) {
            static_cast<void>(::kill(child, SIGKILL));
            static_cast<void>(::waitpid(child, &status, 0));
            return result;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            static_cast<void>(::kill(child, SIGKILL));
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            result.exit_code = 128 + SIGKILL;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
#endif

void AddNoOpPass(RenderGraph& graph, std::string_view name = "NoOp") {
    graph.AddPass(
        name,
        [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); },
        [] {}
    );
}

struct VoidPrepare {
    void operator()(const int&) const {}
};

struct ReferencePrepare {
    int& operator()(const int&) const;
};

struct GraphPrepare {
    int operator()(const int&, RenderGraph&) const;
};

template<typename Prepare>
concept AcceptsSetupPrepare = requires(RenderGraph& graph, Prepare prepare) {
    graph.AddSetupPass("CompileContract", 1, std::move(prepare));
};

static_assert(!AcceptsSetupPrepare<VoidPrepare>);
static_assert(!AcceptsSetupPrepare<ReferencePrepare>);
static_assert(!AcceptsSetupPrepare<GraphPrepare>);

struct MoveOnlyInput {
    std::unique_ptr<int> value{};

    MoveOnlyInput() = default;
    MoveOnlyInput(const MoveOnlyInput&) = delete;
    MoveOnlyInput& operator=(const MoveOnlyInput&) = delete;
    MoveOnlyInput(MoveOnlyInput&&) = default;
    MoveOnlyInput& operator=(MoveOnlyInput&&) = default;
};

struct MoveOnlyPrepare {
    std::unique_ptr<int> offset{};
    std::atomic<int>*    calls = nullptr;

    MoveOnlyPrepare() = default;
    MoveOnlyPrepare(const MoveOnlyPrepare&) = delete;
    MoveOnlyPrepare& operator=(const MoveOnlyPrepare&) = delete;
    MoveOnlyPrepare(MoveOnlyPrepare&&) = default;
    MoveOnlyPrepare& operator=(MoveOnlyPrepare&&) = default;

    int operator()(const MoveOnlyInput& input) {
        calls->fetch_add(1, std::memory_order_relaxed);
        return *input.value + *offset;
    }
};

void TestSynchronousFallbackAndOneShot(TestSuite& suite) {
    constexpr std::string_view test_name =
        "synchronous setup fallback and one-shot result";

    std::atomic<int> setup_calls{0};
    RenderGraph      graph("SetupFallback");
    MoveOnlyInput    input{};
    input.value = std::make_unique<int>(40);
    MoveOnlyPrepare prepare{};
    prepare.offset = std::make_unique<int>(2);
    prepare.calls  = &setup_calls;

    auto value = graph.AddSetupPass(
        "MoveOnly",
        std::move(input),
        std::move(prepare)
    );
    AddNoOpPass(graph);

    bool read_before_compile_failed = false;
    try {
        (void)value.Get();
    } catch (const std::logic_error&) {
        read_before_compile_failed = true;
    }

    suite.Check(value.IsValid(), test_name, "setup registration returned an invalid value");
    suite.Check(
        read_before_compile_failed,
        test_name,
        "a pending setup result was readable before Compile"
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(
        value.IsReady() && value.Get() == 42,
        test_name,
        "move-only input/functor did not publish the expected result"
    );

    // A second compile is supported for diagnostics/re-lowering, but setup is
    // deliberately write-once and must not run again.
    suite.Check(
        setup_calls.load(std::memory_order_relaxed) == 1,
        test_name,
        "setup did not execute exactly once during the first Compile"
    );
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    suite.Check(
        setup_calls.load() == 1 && value.Get() == 42,
        test_name,
        "repeated Compile changed a write-once setup result"
    );
}

void TestNullableSetupFailsClosed(TestSuite& suite) {
    constexpr std::string_view test_name = "nullable setup callback";

    using NullablePrepare = std::function<int(const int&)>;
    RenderGraph empty_function("EmptySetupFunction");
    const auto empty_value = empty_function.AddSetupPass(
        "EmptyFunction",
        1,
        NullablePrepare{}
    );
    AddNoOpPass(empty_function);
    suite.Check(
        !empty_value.IsValid() && !empty_function.Compile() &&
            Contains(empty_function.GetCompileError(), "no prepare callback"),
        test_name,
        "an empty std::function did not fail at declaration time"
    );

    using PreparePointer = int (*)(const int&);
    PreparePointer null_prepare = nullptr;
    RenderGraph    null_function("NullSetupFunction");
    const auto null_value = null_function.AddSetupPass(
        "NullFunction",
        1,
        null_prepare
    );
    AddNoOpPass(null_function);
    suite.Check(
        !null_value.IsValid() && !null_function.Compile() &&
            Contains(null_function.GetCompileError(), "no prepare callback"),
        test_name,
        "a null function pointer did not fail at declaration time"
    );
}

void TestAsyncOrderJoinAndNamedThreadIsolation(TestSuite& suite) {
    constexpr std::string_view test_name =
        "async setup order, join, and named-thread isolation";

    std::mutex              mutex{};
    std::condition_variable cv{};
    bool                    first_started = false;
    bool                    allow_finish  = false;
    bool                    setup_finished = false;
    std::vector<int>        order{};
    std::thread::id         setup_thread{};
    const std::thread::id   caller_thread = std::this_thread::get_id();
    std::atomic<bool>       named_task_ran{false};
    GraphEventRef           named_event{};

    RenderGraph graph("AsyncSetupOrder");
    const auto first = graph.AddSetupPass(
        "First",
        20,
        [&](const int& input) {
            setup_thread = std::this_thread::get_id();
            auto event = LambdaTask::Dispatch(
                [&] { named_task_ran.store(true, std::memory_order_release); },
                EThread::EMainThread
            );
            {
                std::unique_lock lock(mutex);
                named_event   = std::move(event);
                order.push_back(1);
                first_started = true;
                cv.notify_all();
                cv.wait(lock, [&] { return allow_finish; });
            }
            return input + 1;
        }
    );
    const auto second = graph.AddSetupPass(
        "Second",
        first,
        [&](const Moer::Render::RGPreparedValue<int>& dependency) {
            std::lock_guard lock(mutex);
            order.push_back(2);
            setup_finished = true;
            return dependency.Get() * 2;
        }
    );
    AddNoOpPass(graph);

    std::thread releaser([&] {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return first_started; });
        allow_finish = true;
        cv.notify_all();
    });
    const bool compiled = graph.Compile();
    releaser.join();

    bool finished_at_return = false;
    std::vector<int> observed_order{};
    GraphEventRef event_to_drain{};
    {
        std::lock_guard lock(mutex);
        finished_at_return = setup_finished;
        observed_order     = order;
        event_to_drain     = named_event;
    }
    const bool avoided_named_reentry =
        !named_task_ran.load(std::memory_order_acquire);
    if (event_to_drain) {
        TaskGraph::GetInterface().WaitUntilTaskComplete(
            event_to_drain,
            EThread::EMainThread
        );
    }

    suite.Check(compiled, test_name, graph.GetCompileError());
    suite.Check(
        setup_thread != caller_thread,
        test_name,
        "setup did not execute on a TaskGraph worker"
    );
    suite.Check(
        finished_at_return && observed_order == std::vector<int>{1, 2} &&
            first.Get() == 21 && second.Get() == 42,
        test_name,
        "Compile returned before ordered setup publication completed"
    );
    suite.Check(
        avoided_named_reentry &&
            named_task_ran.load(std::memory_order_acquire),
        test_name,
        "setup join pumped unrelated named-thread work or explicit drain failed"
    );
}

int RunNormalWorkerStarvationChild() {
    struct StartBarrier {
        std::mutex              mutex{};
        std::condition_variable cv{};
        uint32_t                arrived = 0;
        uint32_t                target  = 0;
    };

    Moer::TaskSystem::Init();
    TaskGraph& task_graph = TaskGraph::GetInterface();
    const uint32_t normal_worker_count =
        task_graph.GetWorkerThreadCount(EThread::AnyThread_NormalPri);
    if (normal_worker_count == 0) {
        Moer::TaskSystem::ShutDown();
        std::cerr << "normal worker starvation child: no Normal workers\n";
        return EXIT_FAILURE;
    }

    auto barrier    = std::make_shared<StartBarrier>();
    barrier->target = normal_worker_count;
    auto failures   = std::make_shared<std::atomic<uint32_t>>(0);
    GraphEventArray compile_events;
    compile_events.reserve(normal_worker_count);

    for (uint32_t worker = 0; worker < normal_worker_count; ++worker) {
        compile_events.emplace_back(LambdaTask::Dispatch(
            [barrier, failures, worker] {
                {
                    std::unique_lock lock(barrier->mutex);
                    ++barrier->arrived;
                    if (barrier->arrived == barrier->target) {
                        barrier->cv.notify_all();
                    } else {
                        barrier->cv.wait(
                            lock,
                            [&] { return barrier->arrived == barrier->target; }
                        );
                    }
                }

                try {
                    RenderGraph graph("NormalWorkerSetupStarvation");
                    const auto prepared = graph.AddSetupPass(
                        "PrepareOnSaturatedPool",
                        worker,
                        [](const uint32_t& input) { return input + 1; }
                    );
                    AddNoOpPass(graph);
                    if (!graph.Compile() || prepared.Get() != worker + 1) {
                        failures->fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failures->fetch_add(1, std::memory_order_relaxed);
                }
            },
            EThread::AnyThread_NormalPri
        ));
    }

    task_graph.WaitUntilTasksComplete(compile_events, EThread::EMainThread);
    const uint32_t failure_count = failures->load(std::memory_order_relaxed);
    Moer::TaskSystem::ShutDown();
    if (failure_count != 0) {
        std::cerr << "normal worker starvation child: " << failure_count
                  << " compile failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "normal worker starvation child: all workers completed\n";
    return EXIT_SUCCESS;
}

void TestNormalWorkerStarvationSubprocess(TestSuite& suite, const char* argv0) {
    constexpr std::string_view test_name =
        "normal worker compile avoids setup pool starvation";
    const ChildProcessResult result = RunStarvationChild(argv0);
    suite.Check(result.started, test_name, "could not launch isolated child process");
    suite.Check(
        result.started && !result.timed_out && result.exit_code == EXIT_SUCCESS,
        test_name,
        result.timed_out ?
            "all Normal workers blocked waiting for setup work queued to the same pool" :
            "isolated worker-saturation child failed"
    );
}

void TestFailureJoinAndStableRetry(TestSuite& suite) {
    constexpr std::string_view test_name =
        "setup failure joins and remains stable across Compile retries";

    std::mutex              mutex{};
    std::condition_variable cv{};
    bool                    started = false;
    bool                    release = false;
    bool                    finished = false;
    std::atomic<int>        setup_calls{0};
    std::atomic<int>        skipped_calls{0};
    std::atomic<int>        record_calls{0};

    RenderGraph graph("SetupAndCompilerFailure");
    const auto failed = graph.AddSetupPass(
        "FailingSetup",
        1,
        [&](const int&) -> int {
            setup_calls.fetch_add(1, std::memory_order_relaxed);
            {
                std::unique_lock lock(mutex);
                started = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release; });
                finished = true;
            }
            throw std::runtime_error("setup boom");
        }
    );
    const auto skipped = graph.AddSetupPass(
        "SkippedSetup",
        2,
        [&](const int& value) {
            skipped_calls.fetch_add(1, std::memory_order_relaxed);
            return value;
        }
    );
    graph.AddRecordPass(
        "InvalidRecord",
        [](RenderGraph::PassBuilder& builder) {
            builder.Read(RenderGraph::TokenHandle{}).SideEffect();
        },
        [&](CommandList&) {
            record_calls.fetch_add(1, std::memory_order_relaxed);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    std::thread releaser([&] {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return started; });
        release = true;
        cv.notify_all();
    });
    const bool compiled = graph.Compile();
    releaser.join();

    bool failed_get_throws = false;
    try {
        (void)failed.Get();
    } catch (const std::runtime_error& exception) {
        failed_get_throws = Contains(exception.what(), "setup boom");
    }

    suite.Check(
        !compiled && finished && !graph.IsCompiled(),
        test_name,
        "Compile did not join the throwing setup before returning failure"
    );
    suite.Check(
        Contains(graph.GetCompileError(), "declared an invalid resource") &&
            Contains(graph.GetCompileError(), "FailingSetup") &&
            Contains(graph.GetCompileError(), "setup boom"),
        test_name,
        "compiler-primary and setup diagnostics were not preserved together"
    );
    suite.Check(
        failed.HasFailed() && failed_get_throws && skipped.HasFailed() &&
            Contains(skipped.GetError(), "skipped") &&
            skipped_calls.load(std::memory_order_relaxed) == 0,
        test_name,
        "failed/skipped prepared values did not reach stable terminal states"
    );

    const bool retried = graph.Compile();
    suite.Check(
        !retried && setup_calls.load(std::memory_order_relaxed) == 1 &&
            skipped_calls.load(std::memory_order_relaxed) == 0,
        test_name,
        "Compile retry re-executed one-shot setup work"
    );
    suite.Check(
        !graph.ExecuteRecording({}, {}, true) &&
            record_calls.load(std::memory_order_relaxed) == 0,
        test_name,
        "recording ran after setup/compiler failure"
    );
}

void TestGraphDestructionTerminatesPendingValue(TestSuite& suite) {
    constexpr std::string_view test_name =
        "graph destruction terminates undispatched setup";

    Moer::Render::RGPreparedValue<int> orphan{};
    std::atomic<int>                   setup_calls{0};
    {
        RenderGraph graph("UndispatchedSetup");
        orphan = graph.AddSetupPass(
            "NeverDispatched",
            1,
            [&](const int& value) {
                setup_calls.fetch_add(1, std::memory_order_relaxed);
                return value;
            }
        );
        AddNoOpPass(graph);
    }

    suite.Check(
        orphan.HasFailed() && setup_calls.load(std::memory_order_relaxed) == 0 &&
            Contains(orphan.GetError(), "destroyed before"),
        test_name,
        "destroying an uncompiled graph left its setup result pending"
    );
}

void TestBatchOwnerFailureTerminalizesValues(TestSuite& suite) {
    constexpr std::string_view test_name =
        "setup batch owner failure is terminal and one-shot";

    Moer::Render::RGPreparedValue<int> survivor{};
    std::atomic<int>                   setup_calls{0};
    bool                               threw = false;
    {
        RenderGraph graph("BatchOwnerFailure");
        survivor = graph.AddSetupPass(
            "NeverRuns",
            42,
            [&](const int& value) {
                setup_calls.fetch_add(1, std::memory_order_relaxed);
                return value;
            }
        );
        AddNoOpPass(graph);
        Moer::Render::RenderGraphAsyncSetupTestAccess::InjectBatchOwnerFailure(
            graph
        );

        bool compiled = false;
        try {
            compiled = graph.Compile();
        } catch (...) {
            threw = true;
        }

        bool get_failed = false;
        try {
            (void)survivor.Get();
        } catch (const std::runtime_error&) {
            get_failed = true;
        }

        suite.Check(
            !threw && !compiled && !graph.IsCompiled() && survivor.HasFailed() &&
                get_failed && setup_calls.load(std::memory_order_relaxed) == 0,
            test_name,
            "batch owner failure escaped, compiled, ran setup, or left a value Pending"
        );
        suite.Check(
            Contains(graph.GetCompileError(), "batch creation failed") &&
                Contains(survivor.GetError(), "batch creation failed"),
            test_name,
            "batch owner failure did not retain a stable fallback diagnostic"
        );
        suite.Check(
            !graph.Compile() && setup_calls.load(std::memory_order_relaxed) == 0 &&
                survivor.HasFailed(),
            test_name,
            "Compile retry reran or revived a batch that failed before dispatch"
        );
    }

    suite.Check(
        survivor.HasFailed() && setup_calls.load(std::memory_order_relaxed) == 0,
        test_name,
        "graph destruction changed the terminal value after owner failure"
    );
}

void TestFailureDiagnosticFaultIsTerminal(TestSuite& suite) {
    constexpr std::string_view test_name =
        "callback diagnostic allocation fault preserves terminal state";

    std::atomic<int> failing_calls{0};
    std::atomic<int> skipped_calls{0};
    RenderGraph      graph("CallbackDiagnosticFailure");
    const auto failed = graph.AddSetupPass(
        "DiagnosticFault",
        1,
        [&](const int&) -> int {
            failing_calls.fetch_add(1, std::memory_order_relaxed);
            throw std::runtime_error("detail must be optional");
        }
    );
    const auto skipped = graph.AddSetupPass(
        "SkippedAfterDiagnosticFault",
        2,
        [&](const int& value) {
            skipped_calls.fetch_add(1, std::memory_order_relaxed);
            return value;
        }
    );
    AddNoOpPass(graph);
    Moer::Render::RenderGraphAsyncSetupTestAccess::
        InjectFailureDiagnosticFailure(graph);

    bool threw    = false;
    bool compiled = false;
    try {
        compiled = graph.Compile();
    } catch (...) {
        threw = true;
    }

    suite.Check(
        !threw && !compiled && failed.HasFailed() && skipped.HasFailed() &&
            failing_calls.load(std::memory_order_relaxed) == 1 &&
            skipped_calls.load(std::memory_order_relaxed) == 0,
        test_name,
        "diagnostic fault escaped, hung, or left failed/skipped values Pending"
    );
    suite.Check(
        Contains(graph.GetCompileError(), "diagnostic unavailable") &&
            Contains(failed.GetError(), "setup failed") &&
            Contains(skipped.GetError(), "skipped"),
        test_name,
        "fallback diagnostics were not available after detailed storage failed"
    );
    suite.Check(
        !graph.Compile() && failing_calls.load(std::memory_order_relaxed) == 1 &&
            skipped_calls.load(std::memory_order_relaxed) == 0,
        test_name,
        "retry reran a callback after diagnostic allocation failure"
    );
}

void TestTaskDispatchAndDiagnosticFaultsAreTerminal(TestSuite& suite) {
    constexpr std::string_view test_name =
        "task dispatch exception plus diagnostic fault preserves terminal state";

    std::atomic<int> setup_calls{0};
    RenderGraph      graph("TaskDispatchDiagnosticFailure");
    const auto value = graph.AddSetupPass(
        "NeverDispatched",
        9,
        [&](const int& input) {
            setup_calls.fetch_add(1, std::memory_order_relaxed);
            return input;
        }
    );
    AddNoOpPass(graph);
    Moer::Render::RenderGraphAsyncSetupTestAccess::InjectTaskDispatchFailure(
        graph
    );
    Moer::Render::RenderGraphAsyncSetupTestAccess::
        InjectFailureDiagnosticFailure(graph);

    bool threw    = false;
    bool compiled = false;
    try {
        compiled = graph.Compile();
    } catch (...) {
        threw = true;
    }

    suite.Check(
        !threw && !compiled && value.HasFailed() &&
            setup_calls.load(std::memory_order_relaxed) == 0,
        test_name,
        "dispatch fault escaped, ran setup, or left the prepared value Pending"
    );
    suite.Check(
        Contains(graph.GetCompileError(), "diagnostic unavailable") &&
            Contains(value.GetError(), "dispatch"),
        test_name,
        "dispatch failure did not publish stable fallback diagnostics"
    );
    suite.Check(
        !graph.Compile() && setup_calls.load(std::memory_order_relaxed) == 0 &&
            value.HasFailed(),
        test_name,
        "retry reran setup after task dispatch failure"
    );
}

struct LifetimeProbe {
    explicit LifetimeProbe(std::atomic<int>& destructions) :
        destructions(destructions) {}

    ~LifetimeProbe() {
        destructions.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<int>& destructions;
};

struct MoveOnlyPayload {
    std::unique_ptr<LifetimeProbe> probe{};
    int                            value = 0;

    MoveOnlyPayload() = default;
    MoveOnlyPayload(const MoveOnlyPayload&) = delete;
    MoveOnlyPayload& operator=(const MoveOnlyPayload&) = delete;
    MoveOnlyPayload(MoveOnlyPayload&&) = default;
    MoveOnlyPayload& operator=(MoveOnlyPayload&&) = default;
};

struct ConsumerParameters {
    DEFINE_RG_TOKEN_ACCESS(token, RenderGraph::AccessMode::Write);
    Moer::Render::RGPreparedValue<MoveOnlyPayload> prepared{};

    DEFINE_RG_PARAMETER_ACCESS(token);
};

void TestParallelRecordingReadsOnePreparedValue(TestSuite& suite) {
    using namespace std::chrono_literals;
    constexpr std::string_view test_name =
        "parallel recording reads one immutable prepared value";

    std::atomic<int>         destructions{0};
    std::atomic<int>         record_count{0};
    std::atomic<int>         value_sum{0};
    std::atomic<int>         inflight{0};
    std::atomic<int>         max_inflight{0};
    std::atomic<const void*> observed_address{nullptr};
    std::atomic<bool>        address_mismatch{false};
    std::mutex               mutex{};
    std::condition_variable  cv{};
    int                      entered = 0;
    bool                     timed_out = false;

    {
        RenderGraph graph("PreparedValueParallelRead");
        const auto prepared = graph.AddSetupPass(
            "BuildPayload",
            42,
            [&](const int& value) {
                MoveOnlyPayload payload{};
                payload.probe = std::make_unique<LifetimeProbe>(destructions);
                payload.value = value;
                return payload;
            }
        );
        const auto first_token  = graph.CreateTransientToken("FirstToken");
        const auto second_token = graph.CreateTransientToken("SecondToken");

        auto add_consumer = [&](std::string_view name, RenderGraph::TokenHandle token) {
            ConsumerParameters parameters{};
            parameters.token    = token;
            parameters.prepared = prepared;
            graph.AddRecordPass(
                name,
                std::move(parameters),
                [](RenderGraph::PassBuilder& builder) { builder.SideEffect(); },
                [&](CommandList&, const ConsumerParameters& immutable_parameters) {
                    const auto& payload = immutable_parameters.prepared.Get();
                    const void* expected = nullptr;
                    if (!observed_address.compare_exchange_strong(
                            expected,
                            &payload,
                            std::memory_order_acq_rel
                        ) && expected != &payload) {
                        address_mismatch.store(true, std::memory_order_release);
                    }
                    const int current = inflight.fetch_add(1) + 1;
                    int       observed = max_inflight.load();
                    while (observed < current &&
                           !max_inflight.compare_exchange_weak(observed, current)) {}
                    {
                        std::unique_lock lock(mutex);
                        ++entered;
                        cv.notify_all();
                        if (!cv.wait_for(lock, 2s, [&] { return entered == 2; })) {
                            timed_out = true;
                        }
                    }
                    value_sum.fetch_add(payload.value, std::memory_order_relaxed);
                    record_count.fetch_add(1, std::memory_order_relaxed);
                    inflight.fetch_sub(1);
                },
                RenderGraph::PassExecutionClass::ParallelRecordEligible
            );
        };
        add_consumer("FirstConsumer", first_token);
        add_consumer("SecondConsumer", second_token);

        suite.Check(graph.Compile(), test_name, graph.GetCompileError());
        size_t published_sources = 0;
        const bool executed = graph.ExecuteRecording(
            {},
            {},
            true,
            [&](Moer::Array<Moer::Render::RHIRecordingSource>&& sources) {
                published_sources += sources.size();
            }
        );

        suite.Check(executed, test_name, graph.GetCompileError());
        suite.Check(
            published_sources == 2 && record_count.load() == 2 &&
                value_sum.load() == 84 && max_inflight.load() >= 2 &&
                !timed_out && !address_mismatch.load(),
            test_name,
            "parallel consumers did not share one immutable setup result"
        );
        suite.Check(
            destructions.load(std::memory_order_relaxed) == 0,
            test_name,
            "prepared payload was destroyed before graph recording completed"
        );
    }

    suite.Check(
        destructions.load(std::memory_order_relaxed) == 1,
        test_name,
        "move-only prepared payload was not destroyed exactly once"
    );
}

void TestDeclarationFreezeAfterDispatch(TestSuite& suite) {
    constexpr std::string_view test_name =
        "graph declarations freeze after setup dispatch";

    RenderGraph graph("SetupDeclarationFreeze");
    const auto prepared = graph.AddSetupPass(
        "Prepare",
        7,
        [](const int& value) { return value; }
    );
    AddNoOpPass(graph);
    suite.Check(graph.Compile(), test_name, graph.GetCompileError());
    const auto late_token = graph.CreateTransientToken("LateMutation");
    suite.Check(
        prepared.IsReady() && !late_token.IsValid() && !graph.IsCompiled() &&
            !graph.Compile() &&
            Contains(graph.GetCompileError(), "cannot be mutated after asynchronous setup"),
        test_name,
        "post-dispatch graph mutation was not rejected fail-closed"
    );
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--worker-starvation-child") {
        return RunNormalWorkerStarvationChild();
    }

    TestSuite suite;
    TestSynchronousFallbackAndOneShot(suite);
    TestNullableSetupFailsClosed(suite);
    TestGraphDestructionTerminatesPendingValue(suite);
    TestBatchOwnerFailureTerminalizesValues(suite);
    TestNormalWorkerStarvationSubprocess(suite, argv[0]);

    Moer::TaskSystem::Init();
    TestAsyncOrderJoinAndNamedThreadIsolation(suite);
    TestFailureJoinAndStableRetry(suite);
    TestParallelRecordingReadsOnePreparedValue(suite);
    TestDeclarationFreezeAfterDispatch(suite);
    TestFailureDiagnosticFaultIsTerminal(suite);
    TestTaskDispatchAndDiagnosticFaultsAreTerminal(suite);
    Moer::TaskSystem::ShutDown();

    if (suite.FailureCount() != 0) {
        std::cerr << "TestRenderGraphAsyncSetup: " << suite.FailureCount()
                  << " failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "TestRenderGraphAsyncSetup: all checks passed\n";
    return EXIT_SUCCESS;
}
