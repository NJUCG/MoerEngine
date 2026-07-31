#include "ProfileDumpTesting.h"
#include "log/LogSystem.h"
#include "misc/Assert.h"
#include "platform/Platform.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpCodec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
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
#include <DbgHelp.h>
// clang-format on
#include "WindowsCrashDiagnosticsTesting.h"
#endif

namespace {

using namespace Moer::Diagnostics;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

class ScopedDirectory {
public:
    ScopedDirectory() {
        static std::atomic<std::uint64_t> next_id{1};
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto tick =
                static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::filesystem::path candidate =
                std::filesystem::temp_directory_path() /
                ("moer-fatal-diagnostics-" + std::to_string(tick) + "-" +
                 std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path = candidate;
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error("fatal diagnostics fixture directory could not be created");
    }

    ~ScopedDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }

    std::filesystem::path path;
};

std::string ReadText(const std::filesystem::path& _path) {
    std::ifstream stream(_path, std::ios::binary);
    Expect(stream.is_open(), "fatal metadata could not be opened");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> ReadBinary(const std::filesystem::path& _path) {
    std::ifstream stream(_path, std::ios::binary);
    Expect(stream.is_open(), "binary artifact could not be opened");
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
    );
}

struct DecodedPrefixSummary {
    bool valid{false};
    bool found_sentinel{false};
    bool found_session_end{false};
};

DecodedPrefixSummary DecodeProfilePrefix(const std::filesystem::path& _path, std::string_view _sentinel) {
    using namespace Moer::ProfileDump;

    const std::vector<std::uint8_t>                     bytes = ReadBinary(_path);
    std::unordered_map<std::uint64_t, SchemaDescriptor> schemas;
    DecodedPrefixSummary                                summary{
                                       .valid = true,
    };
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        PacketView         packet{};
        std::size_t        consumed      = 0;
        const DecodeStatus packet_status = DecodePacket(
            std::span<const std::uint8_t>(bytes).subspan(offset), CodecLimits{}, packet, consumed
        );
        if (packet_status != DecodeStatus::Ok || consumed == 0) {
            summary.valid = false;
            return summary;
        }
        offset += consumed;

        if (packet.header.type == PacketType::SessionEnd) {
            summary.found_session_end = true;
            continue;
        }
        if (packet.header.type == PacketType::Schema) {
            SchemaDescriptor schema{};
            if (DecodeSchemaPayload(packet, CodecLimits{}, schema) != DecodeStatus::Ok) {
                summary.valid = false;
                return summary;
            }
            schemas.insert_or_assign(ComputeSchemaHash(schema), std::move(schema));
            continue;
        }
        if (packet.header.type != PacketType::Record) {
            continue;
        }

        if (packet.payload.size() < sizeof(std::uint64_t)) {
            summary.valid = false;
            return summary;
        }
        std::uint64_t schema_hash = 0;
        for (std::size_t byte = 0; byte < sizeof(schema_hash); ++byte) {
            schema_hash |= static_cast<std::uint64_t>(packet.payload[byte]) << (byte * 8);
        }
        const auto schema = schemas.find(schema_hash);
        if (schema == schemas.end()) {
            summary.valid = false;
            return summary;
        }
        DecodedRecord record{};
        if (DecodeRecordPayload(packet, schema->second, CodecLimits{}, record) != DecodeStatus::Ok) {
            summary.valid = false;
            return summary;
        }
        for (const FieldValue& value : record.values) {
            if (const auto* text = std::get_if<ProfileString>(&value);
                text && std::string_view(*text) == _sentinel) {
                summary.found_sentinel = true;
            }
        }
    }
    return summary;
}

bool Contains(std::string_view _text, std::string_view _needle) {
    return _text.find(_needle) != std::string_view::npos;
}

std::vector<std::filesystem::path>
FindArtifacts(const std::filesystem::path& _directory, std::string_view _extension) {
    std::vector<std::filesystem::path>        matches;
    std::error_code                           error;
    const std::filesystem::path               crash_directory = _directory / "logs" / "crash";
    std::filesystem::directory_iterator       iterator(crash_directory, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error) && iterator->path().extension() == _extension) {
            matches.push_back(iterator->path());
        }
        iterator.increment(error);
    }
    std::ranges::sort(matches);
    return matches;
}

[[noreturn]] void RunFatalChild(const std::filesystem::path& _directory) {
#if PLATFORM_WINDOWS
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    std::filesystem::current_path(_directory);
    static_cast<void>(Platform::InitializeCrashDiagnostics());
    std::thread worker([] {
        MOER_ASSERT(false, "diagnostic child {}", 42);
    });
    worker.join();
    std::abort();
}

[[noreturn]] void RunFatalArtifactFailureChild(const std::filesystem::path& _directory) {
    std::filesystem::current_path(_directory);
    {
        std::ofstream blocker(_directory / "logs", std::ios::binary);
        if (!blocker.is_open()) {
            std::abort();
        }
        blocker << "not-a-directory";
    }
    RunFatalChild(_directory);
}

[[noreturn]] void RunFatalArtifactWorkerPausedChild(const std::filesystem::path& _directory) {
#if PLATFORM_WINDOWS
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    std::filesystem::current_path(_directory);
    if (!Platform::InitializeCrashDiagnostics()) {
        std::abort();
    }
#if PLATFORM_WINDOWS
    if (!Moer::PlatformTesting::ConfigureCrashWorkerPauseBeforeDump(true)) {
        std::abort();
    }
#endif
    MOER_ASSERT(false, "paused artifact worker");
    std::abort();
}

[[noreturn]] void RunConcurrentFatalChild(const std::filesystem::path& _directory) {
#if PLATFORM_WINDOWS
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    std::filesystem::current_path(_directory);
    if (!Platform::InitializeCrashDiagnostics()) {
        std::abort();
    }

    std::atomic<std::uint32_t> ready{0};
    std::atomic<bool>          start{false};
    const auto                 fatal_worker = [&](std::uint32_t _index) {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        MOER_ASSERT(false, "concurrent fatal {}", _index);
    };
    std::thread first(fatal_worker, 1);
    std::thread second(fatal_worker, 2);
    while (ready.load(std::memory_order_acquire) != 2) {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);
    first.join();
    second.join();
    std::abort();
}

Moer::ProfileDump::RuntimeConfig MakeProfileConfig(const std::filesystem::path& _directory) {
    Moer::ProfileDump::RuntimeConfig config{};
    config.output_path         = _directory / "fatal-profile.mpds";
    config.tls_publish_records = 1;
    config.tls_publish_bytes   = 1024;
    config.tls_max_records     = 64;
    config.tls_max_bytes       = 64 * 1024;
    config.queue_max_chunks    = 64;
    config.queue_max_records   = 4096;
    config.queue_max_bytes     = 4 * 1024 * 1024;
    return config;
}

[[noreturn]] void RunFatalProfileChild(const std::filesystem::path& _directory, bool _pause_writer) {
#if PLATFORM_WINDOWS
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    std::filesystem::current_path(_directory);
    static_cast<void>(Platform::InitializeCrashDiagnostics());
    using namespace Moer::ProfileDump;

    Testing::ClearHooks();
    if (_pause_writer) {
        if (!Testing::ConfigureWriterPauseAfterStart(true)) {
            std::abort();
        }
    }
    RuntimeConfig config = MakeProfileConfig(_directory);
    if (Start(config) != StartResult::Started) {
        std::abort();
    }
    if (_pause_writer) {
        if (!Testing::WaitForWriterPaused(2000)) {
            std::abort();
        }
    } else {
        const SchemaDescriptor schema{
            .name           = "FatalSentinel",
            .event_type     = "contract.fatal_sentinel",
            .kind           = EventKind::Instant,
            .channel        = Channel::CpuThread,
            .schema_version = 1,
            .fields         = {{"label", FieldType::String}},
        };
        const SchemaRegistration registration = RegisterSchema(schema);
        if (registration.status != SchemaStatus::Registered) {
            std::abort();
        }
        const std::array<FieldValueView, 1> values = {
            FieldValueView{std::string_view("crash-durable-sentinel")},
        };
        if (Emit(registration.handle, values) != EmitStatus::Accepted) {
            std::abort();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (GetRuntimeStats().records_written < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        if (GetRuntimeStats().records_written < 1) {
            std::abort();
        }
    }

    std::thread worker([_pause_writer] {
        MOER_ASSERT(false, "profile fatal writer_paused={}", _pause_writer);
    });
    worker.join();
    std::abort();
}

#if PLATFORM_WINDOWS
std::filesystem::path CurrentExecutablePath() {
    std::vector<wchar_t> path(1024);
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            throw std::runtime_error("GetModuleFileNameW failed");
        }
        if (length < path.size() - 1) {
            return std::filesystem::path(std::wstring_view(path.data(), length));
        }
        path.resize(path.size() * 2);
    }
}

DWORD RunFatalChildProcess(
    const std::filesystem::path& _directory,
    std::wstring_view            _mode = L"--fatal-child"
) {
    const std::filesystem::path executable = CurrentExecutablePath();
    std::wstring                command =
        L"\"" + executable.wstring() + L"\" " + std::wstring(_mode) + L" \"" + _directory.wstring() + L"\"";
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW        startup{};
    PROCESS_INFORMATION process{};
    startup.cb = sizeof(startup);
    Expect(
        ::CreateProcessW(
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
        ) != FALSE,
        "fatal child process could not be created"
    );

    const DWORD wait_result = ::WaitForSingleObject(process.hProcess, 15000);
    if (wait_result != WAIT_OBJECT_0) {
        static_cast<void>(::TerminateProcess(process.hProcess, 0xEE));
        static_cast<void>(::WaitForSingleObject(process.hProcess, 2000));
        ::CloseHandle(process.hThread);
        ::CloseHandle(process.hProcess);
        throw std::runtime_error("fatal child did not terminate within the bounded timeout");
    }

    DWORD      exit_code     = 0;
    const BOOL got_exit_code = ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    Expect(got_exit_code != FALSE, "fatal child exit code was unavailable");
    return exit_code;
}
#endif

void TestEnsureContract() {
    ResetEnsureFailures();
    int        evaluation_count = 0;
    const bool result           = MOER_ENSURE(++evaluation_count == 0, "ensure value {}", evaluation_count);
    Expect(!result, "failed ensure returned true");
    Expect(evaluation_count == 1, "ensure expression was evaluated more than once");
    Expect(HasEnsureFailures(), "failed ensure did not set the process flag");

    ResetEnsureFailures();
    Expect(MOER_ENSURE(true, "unreachable ensure"), "successful ensure returned false");
    Expect(!HasEnsureFailures(), "successful ensure changed the process failure flag");

    auto saved_logger = spdlog::default_logger();
    spdlog::set_default_logger(std::shared_ptr<spdlog::logger>{});
    ResetEnsureFailures();
    const bool missing_logger_result =
        MOER_ENSURE(false, "ensure without a default logger");
    spdlog::set_default_logger(std::move(saved_logger));
    Expect(!missing_logger_result, "ensure without a default logger returned true");
    Expect(
        HasEnsureFailures(),
        "ensure without a default logger did not set the process flag"
    );
}

void TestFixedMessageAndStackContract() {
    const std::string    oversized(kFailureMessageCapacity * 2, 'x');
    const FailureMessage message = Detail::FormatFailureMessage("{}", oversized);
    Expect(message.size == kFailureMessageCapacity - 1, "fixed fatal message did not clamp to capacity");
    Expect(message.truncated, "oversized fatal message was not marked truncated");
    FailureMessage corrupted = message;
    corrupted.size           = std::numeric_limits<std::uint32_t>::max();
    Expect(
        corrupted.View().size() == kFailureMessageCapacity - 1,
        "public failure message view trusted a corrupted size"
    );

    const PlatformStackTrace stack = Platform::CaptureStackTrace();
    Expect(stack.frame_count > 0, "platform stack capture returned no frames");
    Expect(stack.frame_count <= stack.frames.size(), "platform stack capture exceeded fixed storage");
}

void TestFatalChildArtifacts() {
#if PLATFORM_WINDOWS
    ScopedDirectory output;
    for (std::size_t run = 1; run <= 2; ++run) {
        const DWORD exit_code = RunFatalChildProcess(output.path);
        Expect(exit_code != 0, "fatal child exited successfully");

        const auto metadata = FindArtifacts(output.path, ".txt");
        const auto dumps    = FindArtifacts(output.path, ".dmp");
        Expect(metadata.size() == run, "fatal metadata was overwritten or duplicated");
        Expect(dumps.size() == run, "fatal minidump was overwritten or missing");
        Expect(
            metadata.back().stem() == dumps.back().stem(),
            "fatal metadata and minidump did not use one paired identity"
        );
        const std::vector<std::uint8_t> dump = ReadBinary(dumps.back());
        Expect(dump.size() >= sizeof(MINIDUMP_HEADER), "fatal minidump is smaller than its header");
        auto* header = reinterpret_cast<const MINIDUMP_HEADER*>(dump.data());
        Expect(header->Signature == MINIDUMP_SIGNATURE, "fatal minidump omitted the MDMP signature");
        PMINIDUMP_DIRECTORY directory    = nullptr;
        void*               stream       = nullptr;
        ULONG               stream_bytes = 0;
        Expect(
            ::MiniDumpReadDumpStream(
                const_cast<std::uint8_t*>(dump.data()), ThreadListStream, &directory, &stream, &stream_bytes
            ) != FALSE &&
                stream != nullptr && stream_bytes >= sizeof(MINIDUMP_THREAD_LIST),
            "fatal minidump omitted its thread-list stream"
        );

        const std::string text = ReadText(metadata.back());
        Expect(
            Contains(text, "artifact_kind=controlled_fatal_snapshot"),
            "fatal metadata omitted its controlled-fatal contract"
        );
        Expect(
            Contains(text, "exception_context=none"),
            "fatal metadata did not disclose the missing SEH context"
        );
        Expect(Contains(text, "expression=false"), "fatal metadata omitted the failed expression");
        Expect(Contains(text, "message=diagnostic child 42"), "fatal metadata omitted the formatted message");
        Expect(
            Contains(text, "profile_flush=not_running"),
            "fatal metadata omitted the ProfileDump flush outcome"
        );
        Expect(Contains(text, "dump_written=1"), "fatal metadata did not confirm the minidump");
        Expect(Contains(text, "dump_flushed=1"), "fatal metadata did not confirm minidump durability");
        Expect(
            Contains(text, "artifact_phase=pre_dump") && Contains(text, "artifact_phase=complete"),
            "fatal metadata omitted its two-phase artifact contract"
        );
        Expect(
            Contains(text, "stack_count=") && Contains(text, "stack[0]=0x"),
            "fatal metadata omitted raw stack addresses"
        );
    }
#endif
}

void TestFatalProfileFlushOutcomes() {
#if PLATFORM_WINDOWS
    {
        ScopedDirectory output;
        const DWORD     exit_code = RunFatalChildProcess(output.path, L"--fatal-profile-durable");
        Expect(exit_code != 0, "durable-profile fatal child exited successfully");

        const auto metadata = FindArtifacts(output.path, ".txt");
        const auto dumps    = FindArtifacts(output.path, ".dmp");
        Expect(metadata.size() == 1 && dumps.size() == 1, "durable-profile fatal artifacts are incomplete");
        const std::string metadata_text = ReadText(metadata.front());
        Expect(
            Contains(metadata_text, "profile_flush=completed"),
            "durable-profile fatal did not flush the written prefix"
        );

        std::vector<std::filesystem::path>        in_progress;
        std::error_code                           error;
        std::filesystem::directory_iterator       iterator(output.path, error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end) {
            if (iterator->path().extension() == ".inprogress") {
                in_progress.push_back(iterator->path());
            }
            iterator.increment(error);
        }
        Expect(in_progress.size() == 1, "durable-profile fatal finalized or lost the in-progress stream");
        const DecodedPrefixSummary prefix =
            DecodeProfilePrefix(in_progress.front(), "crash-durable-sentinel");
        Expect(prefix.valid, "durable-profile fatal prefix did not decode");
        Expect(prefix.found_sentinel, "durable-profile fatal stream omitted the decoded sentinel");
        Expect(!prefix.found_session_end, "durable-profile fatal prefix incorrectly contained SessionEnd");
        Expect(
            !std::filesystem::exists(output.path / "fatal-profile.mpds"),
            "durable-profile fatal incorrectly wrote SessionEnd/final output"
        );
    }

    {
        ScopedDirectory output;
        const auto      started   = std::chrono::steady_clock::now();
        const DWORD     exit_code = RunFatalChildProcess(output.path, L"--fatal-profile-paused");
        const auto      elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
        Expect(exit_code != 0, "paused-profile fatal child exited successfully");
        Expect(
            elapsed < std::chrono::seconds(10), "paused-profile fatal child exceeded the bounded exit window"
        );

        const auto metadata = FindArtifacts(output.path, ".txt");
        const auto dumps    = FindArtifacts(output.path, ".dmp");
        Expect(metadata.size() == 1 && dumps.size() == 1, "paused-profile fatal artifacts are incomplete");
        Expect(
            Contains(ReadText(metadata.front()), "profile_flush=timed_out"),
            "paused-profile fatal did not record the bounded timeout"
        );
    }
#endif
}

void TestArtifactFailureStillTerminates() {
#if PLATFORM_WINDOWS
    ScopedDirectory output;
    const auto      started   = std::chrono::steady_clock::now();
    const DWORD     exit_code = RunFatalChildProcess(output.path, L"--fatal-artifact-failure");
    const auto      elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
    Expect(exit_code != 0, "artifact-failure fatal child exited successfully");
    Expect(
        elapsed < std::chrono::seconds(10), "artifact-failure fatal child exceeded the bounded exit window"
    );
    Expect(
        !std::filesystem::exists(output.path / "logs" / "crash"),
        "artifact-failure fixture unexpectedly created a crash directory"
    );
#endif
}

void TestArtifactWorkerTimeoutStillTerminates() {
#if PLATFORM_WINDOWS
    ScopedDirectory output;
    const auto      started   = std::chrono::steady_clock::now();
    const DWORD     exit_code = RunFatalChildProcess(output.path, L"--fatal-artifact-worker-paused");
    const auto      elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
    Expect(exit_code != 0, "paused artifact-worker child exited successfully");
    Expect(elapsed < std::chrono::seconds(5), "paused artifact worker made fatal termination unbounded");

    const auto metadata = FindArtifacts(output.path, ".txt");
    Expect(metadata.size() == 1, "paused artifact worker did not publish its pre-dump sidecar");
    const std::string text = ReadText(metadata.front());
    Expect(
        Contains(text, "artifact_phase=pre_dump"),
        "paused artifact worker sidecar omitted its durable first phase"
    );
    Expect(
        !Contains(text, "artifact_phase=complete"),
        "paused artifact worker unexpectedly completed the minidump phase"
    );
#endif
}

void TestConcurrentFatalKeepsSingleArtifactOwner() {
#if PLATFORM_WINDOWS
    ScopedDirectory output;
    const auto      started   = std::chrono::steady_clock::now();
    const DWORD     exit_code = RunFatalChildProcess(output.path, L"--fatal-concurrent");
    const auto      elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started);
    Expect(exit_code != 0, "concurrent-fatal child exited successfully");
    Expect(elapsed < std::chrono::seconds(5), "concurrent fatal arbitration exceeded its bounded window");
    Expect(
        FindArtifacts(output.path, ".txt").size() == 1, "concurrent fatal created more than one sidecar owner"
    );
    Expect(
        FindArtifacts(output.path, ".dmp").size() == 1,
        "concurrent fatal created more than one minidump owner"
    );
#endif
}

} // namespace

int main(int _argc, char** _argv) {
    if (_argc == 3 && std::string_view(_argv[1]) == "--fatal-child") {
        RunFatalChild(std::filesystem::path(_argv[2]));
    }
    if (_argc == 3 && std::string_view(_argv[1]) == "--fatal-profile-durable") {
        RunFatalProfileChild(std::filesystem::path(_argv[2]), false);
    }
    if (_argc == 3 && std::string_view(_argv[1]) == "--fatal-profile-paused") {
        RunFatalProfileChild(std::filesystem::path(_argv[2]), true);
    }
    if (_argc == 3 && std::string_view(_argv[1]) == "--fatal-artifact-failure") {
        RunFatalArtifactFailureChild(std::filesystem::path(_argv[2]));
    }
    if (_argc == 3 && std::string_view(_argv[1]) == "--fatal-artifact-worker-paused") {
        RunFatalArtifactWorkerPausedChild(std::filesystem::path(_argv[2]));
    }
    if (_argc == 3 && std::string_view(_argv[1]) == "--fatal-concurrent") {
        RunConcurrentFatalChild(std::filesystem::path(_argv[2]));
    }

    try {
        TestEnsureContract();
        TestFixedMessageAndStackContract();
        TestFatalChildArtifacts();
        TestFatalProfileFlushOutcomes();
        TestArtifactFailureStillTerminates();
        TestArtifactWorkerTimeoutStillTerminates();
        TestConcurrentFatalKeepsSingleArtifactOwner();
        std::cout << "Fatal diagnostics contract tests passed." << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Fatal diagnostics contract test failed: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
