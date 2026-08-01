#include "taskgraph/TaskGraph.h"
#include "taskgraph/TaskSystem.h"
#include "taskgraph/WorkStealing.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <Windows.h>
// clang-format on
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Moer::TaskGraphDetail::BuildWorkerPoolTopology;
using Moer::TaskGraphDetail::ChaseLevDeque;
using namespace std::chrono_literals;

struct TestSuite {
    void Check(bool condition, const char* test, const char* detail) {
        if (condition) {
            return;
        }
        failed = true;
        std::cerr << "[FAIL] " << test << ": " << detail << '\n';
    }

    void Skip(const char* test, const char* detail) {
        ++skipped;
        std::cout << "[SKIP] " << test << ": " << detail << '\n';
    }

    bool     failed  = false;
    uint32_t skipped = 0;
};

struct Item {
    uint32_t id = 0;
};

struct ChildProcessResult {
    bool launched         = false;
    bool completed        = false;
    bool expected_failure = false;
    int  exit_code        = 0;
};

constexpr std::string_view NamedTargetDrainMarker = "named-target-publication-reached\n";

int RunNamedTargetDuringDrainChild(const std::filesystem::path& marker_path) {
    Moer::TaskSystem::Init();
    TaskGraph& graph = TaskGraph::GetInterface();

    std::atomic_bool owner_started{false};
    GraphEventRef owner = LambdaTask::Dispatch(
        [&] {
            owner_started.store(true, std::memory_order_release);
            while (!graph.IsDraining()) {
                std::this_thread::yield();
            }
            // Allocate and construct every task/event resource before the
            // marker. Dispatch below then crosses only the admission boundary,
            // so an allocation/setup failure cannot satisfy the death test.
            auto late_named_task = LambdaTask::Create([] {}, EThread::EMainThread);
            static_cast<void>(late_named_task.GetCompletionEvent());
            {
                std::ofstream marker(marker_path, std::ios::binary | std::ios::trunc);
                marker.write(NamedTargetDrainMarker.data(), NamedTargetDrainMarker.size());
                marker.flush();
                if (!marker) {
                    std::_Exit(EXIT_FAILURE);
                }
            }
            static_cast<void>(late_named_task.Dispatch());
        },
        EThread::AnyThread_HighPri
    );

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!owner_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!owner_started.load(std::memory_order_acquire)) {
        return EXIT_FAILURE;
    }

    std::jthread shutdown_thread([] { Moer::TaskSystem::ShutDown(); });
    shutdown_thread.join();
    return EXIT_SUCCESS;
}

ChildProcessResult RunNamedTargetDuringDrainProcess(
    const std::filesystem::path& executable,
    const std::filesystem::path& marker_path
) {
#if PLATFORM_WINDOWS
    std::wstring command = L"\"" + executable.wstring() +
                           L"\" --named-target-drain-child \"" + marker_path.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    if (::CreateProcessW(
            nullptr,
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

    const DWORD wait_result = ::WaitForSingleObject(process.hProcess, 15000);
    if (wait_result != WAIT_OBJECT_0) {
        static_cast<void>(::TerminateProcess(process.hProcess, 0xEE));
        static_cast<void>(::WaitForSingleObject(process.hProcess, 2000));
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        return {
            .launched = true, .completed = false, .expected_failure = false, .exit_code = 0
        };
    }

    DWORD      exit_code     = 0;
    const BOOL got_exit_code = ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    constexpr DWORD expected_fail_fast_exit_code = 0xC0000602u;
    return {
        .launched         = true,
        .completed        = got_exit_code != FALSE,
        .expected_failure = got_exit_code != FALSE &&
                            exit_code == expected_fail_fast_exit_code,
        .exit_code        = static_cast<int>(exit_code)
    };
#else
    const std::string executable_string = executable.string();
    const std::string marker_string     = marker_path.string();
    const pid_t       child             = ::fork();
    if (child < 0) {
        return {};
    }
    if (child == 0) {
        ::execl(
            executable_string.c_str(),
            executable_string.c_str(),
            "--named-target-drain-child",
            marker_string.c_str(),
            static_cast<char*>(nullptr)
        );
        ::_exit(127);
    }

    int        status   = 0;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    for (;;) {
        const pid_t wait_result = ::waitpid(child, &status, WNOHANG);
        if (wait_result == child) {
            const bool exited   = WIFEXITED(status);
            const bool signaled = WIFSIGNALED(status);
            return {
                .launched         = true,
                .completed        = exited || signaled,
                .expected_failure = signaled && WTERMSIG(status) == SIGABRT,
                .exit_code        = exited ? WEXITSTATUS(status) :
                                            (signaled ? -WTERMSIG(status) : 0)
            };
        }
        if (wait_result < 0 && errno != EINTR) {
            return {.launched = true};
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            static_cast<void>(::kill(child, SIGKILL));
            while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
            return {.launched = true, .completed = false};
        }
        std::this_thread::sleep_for(10ms);
    }
#endif
}

class RegisteredExternalRunnable final : public Runnable {
public:
    uint32_t Run() override {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        const EThread::Type identity = TaskGraph::GetInterface().GetCurrentThread();
        observed_external.store(EThread::IsUnKnownThread(identity), std::memory_order_release);

        GraphEventRef event = LambdaTask::Dispatch([this] {
            dispatched_task_ran.store(true, std::memory_order_release);
        });
        TaskGraph::GetInterface().WaitUntilTaskComplete(event, identity);
        return 0;
    }

    void Init() override {}
    void Stop() override {}
    void Exit() override {}
    ThreadIndex GetIndex() override {
        return EThread::UNKNOWN_THREAD;
    }

    std::atomic_bool start{false};
    std::atomic_bool observed_external{false};
    std::atomic_bool dispatched_task_ran{false};
};

void TestWorkerPoolTopology(TestSuite& suite) {
    struct Case {
        int32_t workers;
        std::array<int32_t, EThread::PriorityCount> expected;
    };
    constexpr Case cases[] = {
        {3, {1, 1, 1}},
        {4, {2, 1, 1}},
        {5, {2, 2, 1}},
        {6, {2, 2, 2}},
        {31, {11, 10, 10}},
        {253, {85, 84, 84}},
    };

    for (const Case& test_case : cases) {
        const auto topology = BuildWorkerPoolTopology(EThread::NamedThreadCount, test_case.workers);
        suite.Check(
            topology.counts == test_case.expected,
            "worker topology",
            "priority distribution is not balanced"
        );

        int32_t visited = 0;
        for (int32_t priority = 0; priority < EThread::PriorityCount; ++priority) {
            suite.Check(
                topology.counts[priority] > 0,
                "worker topology",
                "a priority pool has no worker"
            );
            for (int32_t local = 0; local < topology.counts[priority]; ++local) {
                const int32_t thread = topology.ThreadIndex(priority, local);
                suite.Check(
                    topology.PriorityForThread(thread) == priority &&
                        topology.LocalIndex(thread, priority) == local,
                    "worker topology",
                    "global/local worker mapping is not reversible"
                );
                ++visited;
            }
        }
        suite.Check(
            visited == test_case.workers,
            "worker topology",
            "topology did not cover every worker exactly once"
        );
        suite.Check(
            topology.ThreadIndex(
                EThread::PriorityCount - 1,
                topology.counts[EThread::PriorityCount - 1] - 1
            ) < EThread::UNKNOWN_THREAD,
            "worker topology",
            "a valid worker aliases the reserved UNKNOWN_THREAD index"
        );
    }
}

void TestDequeOrderAndGrowth(TestSuite& suite) {
    ChaseLevDeque<Item> deque(2);
    std::vector<Item>   items(4096);
    for (uint32_t index = 0; index < items.size(); ++index) {
        items[index].id = index;
        deque.PushBottom(&items[index]);
    }

    suite.Check(
        deque.ApproximateSize() == static_cast<int64_t>(items.size()),
        "deque growth",
        "grown deque reported the wrong size"
    );
    for (uint32_t offset = 0; offset < items.size(); ++offset) {
        Item* item = deque.PopBottom();
        suite.Check(item != nullptr, "owner LIFO", "owner lost an item after growth");
        if (item != nullptr) {
            suite.Check(
                item->id == items.size() - offset - 1,
                "owner LIFO",
                "owner order changed across a buffer generation"
            );
        }
    }
    suite.Check(deque.PopBottom() == nullptr, "owner LIFO", "empty deque returned an item");

    for (uint32_t index = 0; index < 64; ++index) {
        deque.PushBottom(&items[index]);
    }
    for (uint32_t index = 0; index < 64; ++index) {
        Item* item = deque.StealTop();
        suite.Check(item != nullptr, "thief FIFO", "thief lost an item");
        if (item != nullptr) {
            suite.Check(item->id == index, "thief FIFO", "thief order is not FIFO from top");
        }
    }
    suite.Check(deque.StealTop() == nullptr, "thief FIFO", "empty deque was stealable");
}

void TestLastItemRace(TestSuite& suite) {
    constexpr uint32_t rounds = 20000;
    ChaseLevDeque<Item> deque(2);
    std::vector<Item>   items(rounds);
    std::vector<Item*>  stolen(rounds, nullptr);
    std::barrier        start(2);
    std::barrier        finish(2);

    std::jthread thief([&] {
        for (uint32_t round = 0; round < rounds; ++round) {
            start.arrive_and_wait();
            stolen[round] = deque.StealTop();
            finish.arrive_and_wait();
        }
    });

    for (uint32_t round = 0; round < rounds; ++round) {
        items[round].id = round;
        deque.PushBottom(&items[round]);
        start.arrive_and_wait();
        Item* popped = deque.PopBottom();
        finish.arrive_and_wait();

        const uint32_t winners = static_cast<uint32_t>(popped != nullptr) +
                                 static_cast<uint32_t>(stolen[round] != nullptr);
        suite.Check(
            winners == 1,
            "last-item race",
            "owner and thief did not produce exactly one winner"
        );
        Item* winner = popped != nullptr ? popped : stolen[round];
        suite.Check(
            winner == &items[round],
            "last-item race",
            "the winner observed a stale slot"
        );
    }
}

void TestConcurrentExactlyOnce(TestSuite& suite) {
    constexpr uint32_t item_count   = 50000;
    constexpr uint32_t thief_count  = 6;
    ChaseLevDeque<Item> deque(2);
    std::vector<Item>   items(item_count);
    auto seen = std::make_unique<std::atomic<uint32_t>[]>(item_count);
    for (uint32_t index = 0; index < item_count; ++index) {
        items[index].id = index;
        seen[index].store(0, std::memory_order_relaxed);
    }

    std::atomic<uint32_t> consumed{0};
    std::atomic_bool      start{false};
    std::atomic_bool      stop{false};
    std::atomic_bool      invalid{false};

    auto record = [&](Item* item) {
        if (item == nullptr) {
            return;
        }
        const uintptr_t address = reinterpret_cast<uintptr_t>(item);
        const uintptr_t begin   = reinterpret_cast<uintptr_t>(items.data());
        const uintptr_t end     = begin + items.size() * sizeof(Item);
        if (address < begin || address >= end || (address - begin) % sizeof(Item) != 0) {
            invalid.store(true, std::memory_order_relaxed);
            return;
        }
        if (seen[item->id].fetch_add(1, std::memory_order_relaxed) != 0) {
            invalid.store(true, std::memory_order_relaxed);
        }
        consumed.fetch_add(1, std::memory_order_release);
    };

    std::vector<std::jthread> thieves;
    thieves.reserve(thief_count);
    for (uint32_t thief_index = 0; thief_index < thief_count; ++thief_index) {
        thieves.emplace_back([&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_acquire)) {
                if (Item* item = deque.StealTop()) {
                    record(item);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (uint32_t index = 0; index < item_count; ++index) {
        deque.PushBottom(&items[index]);
        if ((index & 7u) == 7u) {
            record(deque.PopBottom());
        }
    }

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (consumed.load(std::memory_order_acquire) < item_count &&
           std::chrono::steady_clock::now() < deadline) {
        if (Item* item = deque.PopBottom()) {
            record(item);
        } else {
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    thieves.clear();

    suite.Check(
        consumed.load(std::memory_order_acquire) == item_count,
        "concurrent deque",
        "items were lost or the deque stopped making progress"
    );
    suite.Check(
        !invalid.load(std::memory_order_relaxed),
        "concurrent deque",
        "an item was duplicated or a stale pointer was observed"
    );
    for (uint32_t index = 0; index < item_count; ++index) {
        if (seen[index].load(std::memory_order_relaxed) != 1) {
            suite.Check(false, "concurrent deque", "an item was not consumed exactly once");
            break;
        }
    }
}

void TestNamedTargetDrainRejection(
    TestSuite&                   suite,
    const std::filesystem::path& executable
) {
    const auto nonce = std::to_string(
                           std::chrono::steady_clock::now().time_since_epoch().count()
                       ) +
#if PLATFORM_WINDOWS
                       "_" + std::to_string(::GetCurrentProcessId());
#else
                       "_" + std::to_string(::getpid());
#endif
    const std::filesystem::path marker_path =
        std::filesystem::temp_directory_path() / ("moer_named_target_drain_" + nonce + ".marker");
    std::error_code filesystem_error;
    std::filesystem::remove(marker_path, filesystem_error);

    const ChildProcessResult result =
        RunNamedTargetDuringDrainProcess(executable, marker_path);

    std::ifstream marker(marker_path, std::ios::binary);
    const std::string marker_contents(
        (std::istreambuf_iterator<char>(marker)),
        std::istreambuf_iterator<char>()
    );
    marker.close();
    std::filesystem::remove(marker_path, filesystem_error);

    suite.Check(
        result.launched && result.completed && result.expected_failure &&
            marker_contents == NamedTargetDrainMarker,
        "named-target drain admission",
        "child did not reach the late named-target publication and fail fast within the deadline"
    );
}

void TestTaskGraphIntegration(TestSuite& suite) {
    Moer::TaskSystem::Init();
    TaskGraph& graph = TaskGraph::GetInterface();

    std::atomic<uint32_t> wrong_priority{0};
    GraphEventArray       priority_events;
    constexpr EThread::Type priorities[] = {
        EThread::AnyThread_HighPri,
        EThread::AnyThread_NormalPri,
        EThread::AnyThread_LowPri,
    };
    for (EThread::Type expected : priorities) {
        for (uint32_t task_index = 0; task_index < 64; ++task_index) {
            priority_events.emplace_back(LambdaTask::Dispatch(
                [&, expected](EThread::Type current, const GraphEventRef&) {
                    if (EThread::GetThreadPriority(current) !=
                        EThread::GetThreadPriority(expected)) {
                        wrong_priority.fetch_add(1, std::memory_order_relaxed);
                    }
                },
                expected
            ));
        }
    }
    graph.WaitUntilTasksComplete(priority_events, EThread::EMainThread);
    suite.Check(
        wrong_priority.load(std::memory_order_relaxed) == 0,
        "priority isolation",
        "a task crossed a strict priority-pool boundary"
    );

    std::atomic<uint32_t> burst_count{0};
    for (uint32_t round = 0; round < 100; ++round) {
        GraphEventArray burst;
        for (uint32_t task_index = 0; task_index < 8; ++task_index) {
            burst.emplace_back(LambdaTask::Dispatch([&] {
                burst_count.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        graph.WaitUntilTasksComplete(burst, EThread::EMainThread);
        std::this_thread::sleep_for(1ms);
    }
    suite.Check(
        burst_count.load(std::memory_order_relaxed) == 800,
        "park/wake bursts",
        "a publish/register/wait race stranded work"
    );

    RegisteredExternalRunnable external;
    RunnableThread* external_thread = RunnableThread::Create(
        &external,
        ThreadAttributes{.affinity = Affinity::All(), .name = "TaskGraphExternalOwnerTest"}
    );
    external.start.store(true, std::memory_order_release);
    external_thread->WaitUntilFinished();
    suite.Check(
        external.observed_external.load(std::memory_order_acquire) &&
            external.dispatched_task_ran.load(std::memory_order_acquire),
        "scheduler owner identity",
        "a registered external Runnable was treated as a deque owner or could not dispatch"
    );
    MoerDelete(external_thread);

    EThread::Type steal_priority = EThread::AnyThread_HighPri;
    uint32_t      steal_workers  = 0;
    for (EThread::Type priority : priorities) {
        const uint32_t workers = graph.GetWorkerThreadCount(priority);
        if (workers > steal_workers) {
            steal_workers  = workers;
            steal_priority = priority;
        }
    }

    auto run_local_steal_case = [&](uint32_t child_count, const char* test_name) {
        GraphEventRef  release     = GraphEvent::CreateGraphEvent();
        GraphEventArray children;
        children.reserve(child_count);
        std::atomic<uint32_t> completed{0};
        std::atomic<int32_t>  owner_thread{-1};
        std::atomic_bool      owner_timed_out{false};
        std::mutex            child_threads_mutex;
        std::set<int32_t>     child_threads;

        for (uint32_t child = 0; child < child_count; ++child) {
            std::function<void()> function = [&] {
                const int32_t current = EThread::GetThreadIndex(graph.GetCurrentThread());
                {
                    std::lock_guard lock(child_threads_mutex);
                    child_threads.insert(current);
                }
                completed.fetch_add(1, std::memory_order_release);
            };
            children.emplace_back(
                LambdaTask::Create(std::move(function), steal_priority)
                    .Wait(release)
                    .Dispatch()
            );
        }

        GraphEventRef owner = LambdaTask::Dispatch(
            [&, release](EThread::Type current, const GraphEventRef&) {
                owner_thread.store(EThread::GetThreadIndex(current), std::memory_order_release);
                release->TryUnlockSubsequents(current);

                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (completed.load(std::memory_order_acquire) < child_count &&
                       std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::yield();
                }
                if (completed.load(std::memory_order_acquire) != child_count) {
                    owner_timed_out.store(true, std::memory_order_release);
                }
            },
            steal_priority
        );

        graph.WaitUntilTaskComplete(owner, EThread::EMainThread);
        graph.WaitUntilTasksComplete(children, EThread::EMainThread);
        const int32_t owner_index = owner_thread.load(std::memory_order_acquire);
        bool          ran_on_peer = false;
        {
            std::lock_guard lock(child_threads_mutex);
            ran_on_peer = !child_threads.empty() && !child_threads.contains(owner_index);
        }
        suite.Check(
            !owner_timed_out.load(std::memory_order_acquire) && ran_on_peer,
            test_name,
            "idle peers did not steal while the deque owner was blocked"
        );
    };

    if (steal_workers > 1) {
        run_local_steal_case(1, "single local continuation stealing");
        run_local_steal_case(std::max<uint32_t>(64, steal_workers * 4), "local fan-out stealing");
    } else {
        suite.Skip(
            "TaskGraph local stealing integration",
            "runtime topology has only one worker per priority; deque stealing is covered synthetically"
        );
    }

    // Shutdown must keep every pool alive while an already-running task can
    // still publish a continuation into a different priority pool.
    std::atomic_bool shutdown_task_started{false};
    std::atomic_bool release_shutdown_task{false};
    std::atomic_bool cross_priority_child_ran{false};
    GraphEventRef shutdown_owner = LambdaTask::Dispatch(
        [&] {
            shutdown_task_started.store(true, std::memory_order_release);
            while (!release_shutdown_task.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            LambdaTask::Dispatch(
                [&] { cross_priority_child_ran.store(true, std::memory_order_release); },
                EThread::AnyThread_LowPri
            );
        },
        EThread::AnyThread_HighPri
    );
    const auto shutdown_start_deadline = std::chrono::steady_clock::now() + 5s;
    while (!shutdown_task_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < shutdown_start_deadline) {
        std::this_thread::yield();
    }
    suite.Check(
        shutdown_task_started.load(std::memory_order_acquire),
        "shutdown drain",
        "owner task did not start before the shutdown regression"
    );

    std::jthread shutdown_thread([] { Moer::TaskSystem::ShutDown(); });
    const auto drain_deadline = std::chrono::steady_clock::now() + 5s;
    while ((!graph.IsDraining() || !graph.IsWaitingForIdle()) &&
           std::chrono::steady_clock::now() < drain_deadline) {
        std::this_thread::yield();
    }
    const bool drain_observed = graph.IsDraining() && graph.IsWaitingForIdle();
    suite.Check(
        drain_observed,
        "shutdown drain",
        "shutdown did not enter WaitUntilIdle behind the publication drain gate"
    );
    release_shutdown_task.store(true, std::memory_order_release);
    shutdown_thread.join();
    suite.Check(
        cross_priority_child_ran.load(std::memory_order_acquire),
        "shutdown drain",
        "shutdown closed a target pool before a cross-priority child completed"
    );

    // Reinitialization verifies worker Event, deque-buffer and scheduler
    // lifetimes end only after all worker threads have joined.
    Moer::TaskSystem::Init();
    std::atomic_bool rerun{false};
    GraphEventRef second_lifetime = LambdaTask::Dispatch([&] {
        rerun.store(true, std::memory_order_release);
    });
    TaskGraph::GetInterface().WaitUntilTaskComplete(second_lifetime, EThread::EMainThread);
    suite.Check(
        rerun.load(std::memory_order_acquire),
        "scheduler lifetime",
        "task did not run after TaskGraph reinitialization"
    );
    Moer::TaskSystem::ShutDown();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--named-target-drain-child") {
        return RunNamedTargetDuringDrainChild(argv[2]);
    }

    TestSuite suite;
    TestWorkerPoolTopology(suite);
    TestDequeOrderAndGrowth(suite);
    TestLastItemRace(suite);
    TestConcurrentExactlyOnce(suite);
    TestTaskGraphIntegration(suite);
    TestNamedTargetDrainRejection(suite, std::filesystem::absolute(argv[0]));

    if (suite.failed) {
        return EXIT_FAILURE;
    }
    std::cout << "TaskGraph scheduler contract passed (skipped=" << suite.skipped << ")\n";
    return EXIT_SUCCESS;
}
