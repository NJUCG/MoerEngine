#include "profile/ProfileDumpCodec.h"
#include "profile/ProfileDumpTemplates.h"
#include "profile_consumer/ProfileSummaryContract.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Json = nlohmann::json;
using namespace Moer::ProfileDump;
using NativeProcessArgument = std::filesystem::path::string_type;

static_assert(ProfileSummaryExitCode(SessionLoadStatus::Complete) == 0);
static_assert(ProfileSummaryExitCode(SessionLoadStatus::ForensicTruncated) == 2);
static_assert(ProfileSummaryExitCode(SessionLoadStatus::InvalidArgument) == 10);
static_assert(ProfileSummaryExitCode(SessionLoadStatus::OpenFailed) == 11);
static_assert(ProfileSummaryExitCode(SessionLoadStatus::ProtocolViolation) == 12);
static_assert(ProfileSummaryExitCode(SessionLoadStatus::LimitExceeded) == 13);
static_assert(ProfileSummaryExitCode(SessionLoadStatus::ResourceExhausted) == 14);

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

class SessionBuilder {
public:
    void Begin() {
        Moer::Array<std::uint8_t> payload;
        EncodeSessionBeginPayload(
            {
                .generation      = generation,
                .started_unix_ns = 123456789,
            },
            payload
        );
        Append(PacketType::SessionBegin, payload);
    }

    void End() {
        Moer::Array<std::uint8_t> payload;
        EncodeSessionEndPayload(
            {
                .generation      = generation,
                .records_written = record_count,
                .records_dropped = dropped_count,
            },
            payload
        );
        Append(PacketType::SessionEnd, payload);
    }

    void Schema(const SchemaDescriptor& _schema) {
        Moer::Array<std::uint8_t> payload;
        Expect(
            EncodeSchemaPayload(_schema, {}, payload) == EncodeStatus::Ok, "fixture schema failed to encode"
        );
        Append(PacketType::Schema, payload);
    }

    void Record(
        const SchemaDescriptor&         _schema,
        std::uint64_t                   _sequence,
        std::span<const FieldValueView> _values
    ) {
        Moer::Array<std::uint8_t> payload;
        Expect(
            EncodeRecordPayload(
                ComputeSchemaHash(_schema), _sequence, _schema.fields, _values, {}, payload
            ) == EncodeStatus::Ok,
            "fixture record failed to encode"
        );
        Append(PacketType::Record, payload);
        ++record_count;
    }

    void Loss(const LossNotice& _loss) {
        Moer::Array<std::uint8_t> payload;
        EncodeLossPayload(_loss, payload);
        Append(PacketType::Loss, payload);
        dropped_count += _loss.record_count;
    }

    std::vector<std::uint8_t> bytes{};

private:
    void Append(PacketType _type, std::span<const std::uint8_t> _payload) {
        Moer::Array<std::uint8_t> packet;
        Expect(
            WrapPacket(_type, next_packet_index++, _payload, {}, packet) == EncodeStatus::Ok,
            "fixture packet failed to encode"
        );
        bytes.insert(bytes.end(), packet.begin(), packet.end());
    }

    std::uint64_t generation{17};
    std::uint64_t next_packet_index{0};
    std::uint64_t record_count{0};
    std::uint64_t dropped_count{0};
};

SchemaDescriptor UnknownSchema() {
    return {
        .name           = "CliCounter",
        .event_type     = "cli.counter",
        .kind           = EventKind::Counter,
        .channel        = Channel::CpuThread,
        .schema_version = 1,
        .fields         = {{"value", FieldType::UInt64}},
    };
}

void PopulateCompleteSession(SessionBuilder& _builder) {
    const SchemaDescriptor unknown = UnknownSchema();
    _builder.Begin();
    _builder.Schema(Templates::CpuScope());
    _builder.Schema(Templates::GpuFrame());
    _builder.Schema(Templates::GpuScope());
    _builder.Schema(unknown);

    const std::array<FieldValueView, 5> cpu{
        std::uint64_t{7},
        std::string_view("cli-cpu"),
        std::uint64_t{10},
        std::uint64_t{20},
        std::uint32_t{0},
    };
    _builder.Record(Templates::CpuScope(), 1, cpu);

    const std::array<FieldValueView, 7> frame{
        std::uint64_t{42},
        std::uint32_t{0},
        true,
        std::uint64_t{1},
        std::uint64_t{0},
        std::uint64_t{0},
        std::string_view{},
    };
    _builder.Record(Templates::GpuFrame(), 2, frame);

    const std::array<FieldValueView, 18> scope{
        std::uint64_t{42},
        std::uint64_t{1},
        std::uint64_t{0},
        std::uint64_t{0},
        std::uint64_t{0},
        std::uint32_t{0},
        std::uint32_t{5},
        std::uint32_t{6},
        std::string_view("cli-gpu"),
        std::uint32_t{0},
        std::uint64_t{100},
        std::uint64_t{110},
        std::uint32_t{64},
        2.0,
        20.0,
        20.0,
        std::uint32_t{0},
        std::string_view{},
    };
    _builder.Record(Templates::GpuScope(), 3, scope);

    const std::array<FieldValueView, 1> counter{std::uint64_t{9}};
    _builder.Record(unknown, 4, counter);
    _builder.Loss({
        .first_sequence = 5,
        .last_sequence  = 5,
        .record_count   = 1,
        .value_bytes    = 8,
        .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
    });
    _builder.End();
}

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        static std::atomic<std::uint64_t> next{0};
        for (std::uint32_t attempt = 0; attempt < 32; ++attempt) {
            const auto                  tick = std::chrono::steady_clock::now().time_since_epoch().count();
            const std::filesystem::path candidate =
                std::filesystem::temp_directory_path() /
                ("profile-summary-cli-" + std::to_string(tick) + "-" + std::to_string(next.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path = candidate;
                return;
            }
        }
        throw std::runtime_error("could not create ProfileSummary CLI test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path{};
};

void WriteBytes(const std::filesystem::path& _path, std::span<const std::uint8_t> _bytes) {
    std::ofstream stream(_path, std::ios::binary | std::ios::trunc);
    Expect(stream.is_open(), "could not create ProfileDump fixture");
    stream.write(reinterpret_cast<const char*>(_bytes.data()), static_cast<std::streamsize>(_bytes.size()));
    Expect(stream.good(), "could not write ProfileDump fixture");
}

std::string ReadText(const std::filesystem::path& _path) {
    std::ifstream stream(_path, std::ios::binary);
    Expect(stream.is_open(), "could not open child-process capture");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct ProcessResult {
    int         exit_code{-1};
    std::string standard_output{};
    std::string standard_error{};
};

#if defined(_WIN32)

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE _handle = INVALID_HANDLE_VALUE) noexcept : handle(_handle) {}

    ~ScopedHandle() {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
        }
    }

    ScopedHandle(const ScopedHandle&)            = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& _other) noexcept : handle(_other.handle) {
        _other.handle = INVALID_HANDLE_VALUE;
    }

    ScopedHandle& operator=(ScopedHandle&& _other) noexcept {
        if (this != &_other) {
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
                CloseHandle(handle);
            }
            handle        = _other.handle;
            _other.handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }

    HANDLE Get() const noexcept {
        return handle;
    }

private:
    HANDLE handle{INVALID_HANDLE_VALUE};
};

std::wstring QuoteWindowsArgument(std::wstring_view _argument) {
    std::wstring quoted;
    quoted.push_back(L'"');
    std::size_t backslash_count = 0;
    for (const wchar_t character : _argument) {
        if (character == L'\\') {
            ++backslash_count;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslash_count, L'\\');
            quoted.push_back(character);
        }
        backslash_count = 0;
    }
    quoted.append(backslash_count * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

ProcessResult RunProcess(
    const std::filesystem::path&              _executable,
    const std::vector<NativeProcessArgument>& _arguments,
    const std::filesystem::path&              _working_directory,
    const std::filesystem::path&              _stdout_path,
    const std::filesystem::path&              _stderr_path
) {
    SECURITY_ATTRIBUTES security{};
    security.nLength        = sizeof(security);
    security.bInheritHandle = TRUE;

    ScopedHandle stdout_handle(CreateFileW(
        _stdout_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));
    ScopedHandle stderr_handle(CreateFileW(
        _stderr_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));
    ScopedHandle stdin_handle(CreateFileW(
        L"NUL",
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    ));
    Expect(
        stdout_handle.Get() != INVALID_HANDLE_VALUE && stderr_handle.Get() != INVALID_HANDLE_VALUE &&
            stdin_handle.Get() != INVALID_HANDLE_VALUE,
        "could not create child-process handles"
    );

    std::wstring command_line = QuoteWindowsArgument(_executable.native());
    for (const NativeProcessArgument& argument : _arguments) {
        command_line.push_back(L' ');
        command_line += QuoteWindowsArgument(argument);
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdInput  = stdin_handle.Get();
    startup.hStdOutput = stdout_handle.Get();
    startup.hStdError  = stderr_handle.Get();

    PROCESS_INFORMATION process{};
    const BOOL          created = CreateProcessW(
        _executable.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        _working_directory.c_str(),
        &startup,
        &process
    );
    if (!created) {
        throw std::runtime_error(
            "could not launch MoerProfileSummary, Win32 error " + std::to_string(GetLastError())
        );
    }

    ScopedHandle process_handle(process.hProcess);
    ScopedHandle thread_handle(process.hThread);
    Expect(WaitForSingleObject(process_handle.Get(), INFINITE) == WAIT_OBJECT_0, "child process wait failed");

    DWORD exit_code = 0;
    Expect(GetExitCodeProcess(process_handle.Get(), &exit_code) != FALSE, "child process exit query failed");

    stdout_handle = ScopedHandle{};
    stderr_handle = ScopedHandle{};
    return {
        .exit_code       = static_cast<int>(exit_code),
        .standard_output = ReadText(_stdout_path),
        .standard_error  = ReadText(_stderr_path),
    };
}

#else

ProcessResult RunProcess(
    const std::filesystem::path&              _executable,
    const std::vector<NativeProcessArgument>& _arguments,
    const std::filesystem::path&              _working_directory,
    const std::filesystem::path&              _stdout_path,
    const std::filesystem::path&              _stderr_path
) {
    std::vector<std::string> arguments;
    arguments.reserve(_arguments.size() + 1);
    arguments.push_back(_executable.native());
    for (const NativeProcessArgument& argument : _arguments) {
        arguments.push_back(argument);
    }

    const pid_t child = fork();
    Expect(child >= 0, "could not fork MoerProfileSummary");
    if (child == 0) {
        const int stdout_fd = open(_stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int stderr_fd = open(_stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (stdout_fd < 0 || stderr_fd < 0 || chdir(_working_directory.c_str()) != 0 ||
            dup2(stdout_fd, STDOUT_FILENO) < 0 || dup2(stderr_fd, STDERR_FILENO) < 0) {
            _exit(127);
        }
        close(stdout_fd);
        close(stderr_fd);

        std::vector<char*> argument_pointers;
        argument_pointers.reserve(arguments.size() + 1);
        for (std::string& argument : arguments) {
            argument_pointers.push_back(argument.data());
        }
        argument_pointers.push_back(nullptr);
        execv(_executable.c_str(), argument_pointers.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw std::runtime_error("could not wait for MoerProfileSummary");
        }
    }
    const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return {
        .exit_code       = exit_code,
        .standard_output = ReadText(_stdout_path),
        .standard_error  = ReadText(_stderr_path),
    };
}

#endif

class ProcessRunner {
public:
    ProcessRunner(std::filesystem::path _executable, std::filesystem::path _directory) :
        executable(std::move(_executable)),
        directory(std::move(_directory)) {}

    ProcessResult Run(std::string_view _label, std::vector<NativeProcessArgument> _arguments) {
        const std::filesystem::path stdout_path = directory / (std::string(_label) + ".stdout");
        const std::filesystem::path stderr_path = directory / (std::string(_label) + ".stderr");
        return RunProcess(executable, _arguments, directory, stdout_path, stderr_path);
    }

private:
    std::filesystem::path executable{};
    std::filesystem::path directory{};
};

Json ParseJson(const ProcessResult& _result, std::string_view _case_name) {
    try {
        return Json::parse(_result.standard_output);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string(_case_name) + " did not emit valid JSON: " + error.what() +
            "; stdout=" + _result.standard_output + "; stderr=" + _result.standard_error
        );
    }
}

bool IsDecimalString(const Json& _value) {
    if (!_value.is_string()) {
        return false;
    }
    const std::string& text = _value.get_ref<const std::string&>();
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char character) {
        return std::isdigit(character) != 0;
    });
}

void ExpectU64(const Json& _value, std::string_view _field_name) {
    Expect(IsDecimalString(_value), std::string(_field_name) + " is not a decimal string");
}

void ExpectContractHeader(const Json& _document) {
    Expect(_document.at("contract") == "moer.profile.summary", "summary contract changed");
    Expect(_document.at("version") == 1, "summary contract version changed");
}

void TestCliBoundaryContract() {
    SessionErrorCode emergency_error  = SessionErrorCode::None;
    const auto       emergency_writer = [&](SessionErrorCode _error) noexcept {
        emergency_error = _error;
    };

    Expect(
        RunProfileSummaryCliBoundary(
            [] {
                return 2;
            },
            emergency_writer
        ) == 2 &&
            emergency_error == SessionErrorCode::None,
        "CLI boundary changed a normal exit code"
    );
    Expect(
        RunProfileSummaryCliBoundary(
            []() -> int {
                throw std::bad_alloc{};
            },
            emergency_writer
        ) == 14 &&
            emergency_error == SessionErrorCode::ResourceAllocationFailed,
        "CLI boundary did not map bad_alloc to exit code 14"
    );
    Expect(
        RunProfileSummaryCliBoundary(
            []() -> int {
                throw 7;
            },
            emergency_writer
        ) == 14 &&
            emergency_error == SessionErrorCode::UnexpectedFailure,
        "CLI boundary did not map an unexpected exception to exit code 14"
    );

    for (const SessionErrorCode error :
         {SessionErrorCode::ResourceAllocationFailed, SessionErrorCode::UnexpectedFailure}) {
        const Json emergency = Json::parse(ProfileSummaryEmergencyJson(error));
        ExpectContractHeader(emergency);
        Expect(
            emergency.at("load").at("status") == "resource_exhausted" &&
                emergency.at("load").at("error_code") ==
                    (error == SessionErrorCode::ResourceAllocationFailed ? "resource_allocation_failed" :
                                                                           "unexpected_failure") &&
                emergency.at("load").at("codec_status") == "ok" && !emergency.contains("session"),
            "emergency JSON contract changed"
        );
    }
}

void ExpectLoadU64Fields(const Json& _document) {
    const Json& load = _document.at("load");
    ExpectU64(load.at("input_bytes"), "load.input_bytes");
    ExpectU64(load.at("valid_prefix_bytes"), "load.valid_prefix_bytes");
}

void ExpectUsableSessionU64Fields(const Json& _document, bool _has_session_end) {
    const Json& session = _document.at("session");
    ExpectU64(session.at("generation"), "session.generation");
    ExpectU64(session.at("started_unix_ns"), "session.started_unix_ns");
    if (_has_session_end) {
        ExpectU64(session.at("records_written"), "session.records_written");
        ExpectU64(session.at("records_dropped"), "session.records_dropped");
    } else {
        Expect(session.at("records_written").is_null(), "forensic records_written is not null");
        Expect(session.at("records_dropped").is_null(), "forensic records_dropped is not null");
    }

    const Json& packets = _document.at("packets");
    for (const char* field : {"total", "schema_packets", "unique_schemas", "records", "loss_notices"}) {
        ExpectU64(packets.at(field), std::string("packets.") + field);
    }

    const Json& records = _document.at("records");
    for (const char* field : {"cpu_scopes", "gpu_frames", "gpu_scopes", "unknown"}) {
        ExpectU64(records.at(field), std::string("records.") + field);
    }

    const Json& loss = _document.at("loss");
    ExpectU64(loss.at("noticed_records"), "loss.noticed_records");
    ExpectU64(loss.at("value_bytes"), "loss.value_bytes");
    ExpectU64(loss.at("reason_mask"), "loss.reason_mask");
    if (_has_session_end) {
        ExpectU64(loss.at("unnotified_records"), "loss.unnotified_records");
    } else {
        Expect(loss.at("unnotified_records").is_null(), "forensic unnotified_records is not null");
    }

    const Json& cpu = _document.at("cpu");
    for (const char* field : {"tracks", "scopes", "orphan_scopes", "begin_ns", "end_ns"}) {
        ExpectU64(cpu.at(field), std::string("cpu.") + field);
    }

    const Json& gpu = _document.at("gpu");
    for (const char* field : {"total", "complete", "degraded_complete", "incomplete", "invalid"}) {
        ExpectU64(gpu.at("frames").at(field), std::string("gpu.frames.") + field);
    }
    for (const char* field : {"total", "ready", "error", "orphan"}) {
        ExpectU64(gpu.at("scopes").at(field), std::string("gpu.scopes.") + field);
    }
    ExpectU64(gpu.at("tracks"), "gpu.tracks");
    for (const Json& domain : gpu.at("domains")) {
        ExpectU64(domain.at("ready_scopes"), "gpu.domains[].ready_scopes");
        ExpectU64(domain.at("error_scopes"), "gpu.domains[].error_scopes");
    }
    ExpectU64(_document.at("logical_model_bytes"), "logical_model_bytes");
}

void TestComplete(ProcessRunner& _runner, const std::filesystem::path& _path) {
    const ProcessResult result = _runner.Run("complete", {_path.native()});
    Expect(result.exit_code == 0, "complete capture did not return exit code 0");

    const Json document = ParseJson(result, "complete capture");
    ExpectContractHeader(document);
    ExpectLoadU64Fields(document);
    Expect(document.at("load").at("status") == "complete", "complete status changed");
    Expect(document.at("load").at("error_code") == "none", "complete error code changed");
    Expect(document.at("load").at("codec_status") == "ok", "complete codec status changed");
    Expect(document.at("load").at("error_byte_offset").is_null(), "complete error offset is not null");
    Expect(document.at("load").at("error_packet_index").is_null(), "complete error packet is not null");
    Expect(
        document.at("load").at("incomplete_byte_offset").is_null(), "complete incomplete offset is not null"
    );
    Expect(
        document.at("load").at("incomplete_packet_index").is_null(), "complete incomplete packet is not null"
    );
    Expect(document.at("session").at("has_session_end") == true, "complete session has no SessionEnd");
    ExpectUsableSessionU64Fields(document, true);
    Expect(document.at("session").at("generation") == "17", "complete generation changed");
    Expect(document.at("session").at("records_written") == "4", "complete record total changed");
    Expect(document.at("session").at("records_dropped") == "1", "complete drop total changed");
    Expect(
        document.at("packets") ==
            Json{
                {"total", "11"},
                {"schema_packets", "4"},
                {"unique_schemas", "4"},
                {"records", "4"},
                {"loss_notices", "1"},
            },
        "complete packet summary changed"
    );
    Expect(
        document.at("records") ==
            Json{
                {"cpu_scopes", "1"},
                {"gpu_frames", "1"},
                {"gpu_scopes", "1"},
                {"unknown", "1"},
            },
        "complete record summary changed"
    );
    const Json& loss = document.at("loss");
    Expect(
        loss.at("noticed_records") == "1" && loss.at("unnotified_records") == "0" &&
            loss.at("value_bytes") == "8" &&
            loss.at("reason_mask") == std::to_string(static_cast<std::uint32_t>(LossReason::QueueFull)),
        "complete Loss summary changed"
    );
    const Json& cpu = document.at("cpu");
    Expect(
        cpu.at("clock") == "steady_clock_ns" && cpu.at("absolute_unix_anchor") == false &&
            cpu.at("tracks") == "1" && cpu.at("scopes") == "1" && cpu.at("orphan_scopes") == "0" &&
            cpu.at("has_range") == true && cpu.at("begin_ns") == "10" && cpu.at("end_ns") == "20",
        "complete CPU summary changed"
    );
    const Json& gpu = document.at("gpu");
    Expect(
        gpu.at("clock") == "raw_device_ticks" && gpu.at("cross_cpu_alignment") == "none" &&
            gpu.at("cross_domain_alignment") == "none" && gpu.at("tracks") == "1" &&
            gpu.at("frames").at("total") == "1" && gpu.at("frames").at("complete") == "1" &&
            gpu.at("frames").at("degraded_complete") == "0" && gpu.at("frames").at("incomplete") == "0" &&
            gpu.at("frames").at("invalid") == "0" && gpu.at("scopes").at("total") == "1" &&
            gpu.at("scopes").at("ready") == "1" && gpu.at("scopes").at("error") == "0" &&
            gpu.at("scopes").at("orphan") == "0",
        "complete GPU summary changed"
    );
    Expect(gpu.at("domains").size() == 1, "complete GPU domain count changed");
    const Json& domain = gpu.at("domains").front();
    Expect(
        domain.at("native_queue_id") == 5 && domain.at("family_id") == 6 &&
            domain.at("logical_queues") == Json::array({"graphics"}) &&
            domain.at("has_ready_timestamps") == true && domain.at("timing_capability_trusted") == true &&
            domain.at("valid_bits") == 64 && domain.at("tick_period_ns") == 2.0 &&
            domain.at("ready_scopes") == "1" && domain.at("error_scopes") == "0",
        "complete GPU domain metadata changed"
    );
}

void TestForensic(ProcessRunner& _runner, const std::filesystem::path& _path) {
    const ProcessResult result =
        _runner.Run("forensic", {_path.native(), std::filesystem::path("--allow-truncated").native()});
    Expect(result.exit_code == 2, "forensic capture did not return exit code 2");

    const Json document = ParseJson(result, "forensic capture");
    ExpectContractHeader(document);
    ExpectLoadU64Fields(document);
    const Json& load = document.at("load");
    Expect(load.at("status") == "forensic_truncated", "forensic status changed");
    Expect(load.at("incomplete_reason") == "missing_session_end", "forensic reason changed");
    Expect(load.at("error_code") == "none", "forensic result reported an error");
    Expect(load.at("codec_status") == "ok", "forensic codec status changed");
    Expect(load.at("error_byte_offset").is_null(), "forensic error offset is not null");
    Expect(load.at("error_packet_index").is_null(), "forensic error packet is not null");
    ExpectU64(load.at("incomplete_byte_offset"), "load.incomplete_byte_offset");
    ExpectU64(load.at("incomplete_packet_index"), "load.incomplete_packet_index");
    Expect(document.at("session").at("has_session_end") == false, "forensic session has SessionEnd");
    ExpectUsableSessionU64Fields(document, false);
}

void TestProtocolViolation(ProcessRunner& _runner, const std::filesystem::path& _path) {
    const ProcessResult result = _runner.Run("protocol", {_path.native()});
    Expect(result.exit_code == 12, "protocol violation did not return exit code 12");

    const Json document = ParseJson(result, "protocol violation");
    ExpectContractHeader(document);
    ExpectLoadU64Fields(document);
    const Json& load = document.at("load");
    Expect(load.at("status") == "protocol_violation", "protocol status changed");
    Expect(load.at("error_code") == "session_begin_duplicate", "protocol error code changed");
    Expect(load.at("codec_status") == "ok", "protocol codec status changed");
    ExpectU64(load.at("error_byte_offset"), "load.error_byte_offset");
    ExpectU64(load.at("error_packet_index"), "load.error_packet_index");
    Expect(load.at("incomplete_byte_offset").is_null(), "protocol incomplete offset is not null");
    Expect(load.at("incomplete_packet_index").is_null(), "protocol incomplete packet is not null");
    Expect(!document.contains("session"), "failed protocol load exposed a session");
}

void TestMissingFile(ProcessRunner& _runner, const std::filesystem::path& _path) {
    const ProcessResult result = _runner.Run("missing", {_path.native()});
    Expect(result.exit_code == 11, "missing file did not return exit code 11");

    const Json document = ParseJson(result, "missing file");
    ExpectContractHeader(document);
    ExpectLoadU64Fields(document);
    const Json& load = document.at("load");
    Expect(load.at("status") == "open_failed", "missing-file status changed");
    Expect(load.at("error_code") == "file_open_failed", "missing-file error code changed");
    Expect(load.at("codec_status") == "ok", "missing-file codec status changed");
    Expect(load.at("error_byte_offset").is_null(), "missing-file error offset is not null");
    Expect(load.at("error_packet_index").is_null(), "missing-file error packet is not null");
    Expect(load.at("incomplete_byte_offset").is_null(), "missing-file incomplete offset is not null");
    Expect(load.at("incomplete_packet_index").is_null(), "missing-file incomplete packet is not null");
    Expect(!document.contains("session"), "missing-file load exposed a session");
}

void TestLimitExceeded(ProcessRunner& _runner, const std::filesystem::path& _path) {
    const ProcessResult result = _runner.Run("limit", {_path.native()});
    Expect(result.exit_code == 13, "codec limit failure did not return exit code 13");

    const Json document = ParseJson(result, "codec limit failure");
    ExpectContractHeader(document);
    const Json& load = document.at("load");
    Expect(load.at("status") == "limit_exceeded", "codec limit status changed");
    Expect(load.at("error_code") == "codec_header_invalid", "codec limit error code changed");
    Expect(load.at("limit") == "codec", "codec limit kind changed");
    Expect(load.at("codec_status") == "payload_too_large", "codec limit codec status changed");
    Expect(!document.contains("session"), "codec limit failure exposed a session");
}

void TestInvalidArguments(ProcessRunner& _runner, const std::filesystem::path& _valid_path) {
    const ProcessResult missing_argument = _runner.Run("invalid-missing-argument", {});
    Expect(missing_argument.exit_code == 10, "missing CLI argument did not return exit code 10");
    Expect(missing_argument.standard_output.empty(), "missing CLI argument unexpectedly emitted JSON");
    Expect(!missing_argument.standard_error.empty(), "missing CLI argument emitted no usage diagnostic");

    const ProcessResult unknown_option = _runner.Run(
        "invalid-option", {_valid_path.native(), std::filesystem::path("--unknown-option").native()}
    );
    Expect(unknown_option.exit_code == 10, "unknown CLI option did not return exit code 10");
    Expect(unknown_option.standard_output.empty(), "unknown CLI option unexpectedly emitted JSON");
    Expect(!unknown_option.standard_error.empty(), "unknown CLI option emitted no diagnostic");
}

} // namespace

int main(int _argument_count, char** _arguments) {
    try {
        TestCliBoundaryContract();
        Expect(_argument_count == 2, "expected MoerProfileSummary path in argv[1]");
        const std::filesystem::path executable = std::filesystem::absolute(_arguments[1]);
        Expect(std::filesystem::is_regular_file(executable), "MoerProfileSummary executable does not exist");

        ScopedTempDirectory         temporary;
        const std::filesystem::path complete_path = temporary.path / std::filesystem::path(u8"完整 捕获.mpd");
        const std::filesystem::path forensic_path = temporary.path / "forensic.mpd";
        const std::filesystem::path protocol_path = temporary.path / "protocol.mpd";
        const std::filesystem::path limit_path    = temporary.path / "limit.mpd";

        SessionBuilder complete;
        PopulateCompleteSession(complete);
        WriteBytes(complete_path, complete.bytes);

        SessionBuilder forensic;
        forensic.Begin();
        WriteBytes(forensic_path, forensic.bytes);

        SessionBuilder protocol;
        protocol.Begin();
        protocol.Begin();
        WriteBytes(protocol_path, protocol.bytes);

        CodecLimits               oversized_limits{};
        Moer::Array<std::uint8_t> oversized_payload(
            oversized_limits.max_packet_payload_bytes + 1, std::uint8_t{0}
        );
        oversized_limits.max_packet_payload_bytes = oversized_payload.size();
        Moer::Array<std::uint8_t> oversized_packet;
        Expect(
            WrapPacket(PacketType::Schema, 0, oversized_payload, oversized_limits, oversized_packet) ==
                EncodeStatus::Ok,
            "could not construct codec limit fixture"
        );
        WriteBytes(limit_path, oversized_packet);

        ProcessRunner runner(executable, temporary.path);
        TestComplete(runner, complete_path);
        TestForensic(runner, forensic_path);
        TestProtocolViolation(runner, protocol_path);
        TestMissingFile(runner, temporary.path / "missing.mpd");
        TestLimitExceeded(runner, limit_path);
        TestInvalidArguments(runner, complete_path);

        std::cout << "ProfileSummary CLI contract passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ProfileSummary CLI contract failed: " << error.what() << '\n';
        return 1;
    }
}
