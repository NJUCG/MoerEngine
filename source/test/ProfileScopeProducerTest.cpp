#include "ProfileDumpTesting.h"
#include "config/GlobalConfig.h"
#include "profile/ProfileDumpCodec.h"
#include "profile/ProfileDumpTemplates.h"
#include "profile/ProfileScope.h"
#include "taskgraph/TaskSystem.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace Moer::ProfileDump;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

class ScopedOutput {
public:
    explicit ScopedOutput(std::string_view _stem) {
        static std::atomic<std::uint64_t> next_id{0};
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto now =
                static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            const std::filesystem::path candidate =
                std::filesystem::temp_directory_path() /
                (std::string(_stem) + "-" + std::to_string(now) + "-" +
                 std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                directory = candidate;
                path      = directory / "capture.mpds";
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::runtime_error("profile scope fixture directory could not be created");
            }
        }
        throw std::runtime_error("profile scope fixture could not reserve a unique directory");
    }

    ~ScopedOutput() {
        CpuScopeProducer::Deactivate();
        static_cast<void>(Shutdown());
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

RuntimeConfig MakeRuntimeConfig(const ScopedOutput& _output) {
    RuntimeConfig config{};
    config.output_path         = _output.path;
    config.replace_existing    = false;
    config.tls_publish_records = 64;
    config.tls_publish_bytes   = 16 * 1024;
    config.tls_max_records     = 128;
    config.tls_max_bytes       = 128 * 1024;
    config.queue_max_chunks    = 64;
    config.queue_max_records   = 4096;
    config.queue_max_bytes     = 4 * 1024 * 1024;
    return config;
}

void WriteTextFile(const std::filesystem::path& _path, std::string_view _contents) {
    std::ofstream stream(_path, std::ios::binary | std::ios::trunc);
    Expect(stream.is_open(), "profile scope config fixture could not be opened");
    stream.write(_contents.data(), static_cast<std::streamsize>(_contents.size()));
    Expect(stream.good(), "profile scope config fixture could not be written");
}

void ExpectFlushSucceeded(FlushResult _result, std::string_view _message) {
    Expect(_result == FlushResult::Completed || _result == FlushResult::NothingPending, _message);
}

SchemaHandle StartCpuProducer(const RuntimeConfig& _config) {
    const StartResult start_result = Start(_config);
    if (start_result != StartResult::Started) {
        throw std::runtime_error(
            "ProfileDump runtime failed to start (result=" +
            std::to_string(static_cast<unsigned int>(start_result)) + ")"
        );
    }
    const SchemaRegistration registration = RegisterSchema(Templates::CpuScope());
    Expect(
        registration.status == SchemaStatus::Registered,
        "CpuScope schema failed to register"
    );
    Expect(
        CpuScopeProducer::Activate(registration.handle) == CpuScopeActivationResult::Activated,
        "CpuScope producer failed to activate"
    );
    return registration.handle;
}

void StopCpuProducer() {
    CpuScopeProducer::Deactivate();
    ExpectFlushSucceeded(FlushThreadLocal(), "producer TLS failed to flush");
    Expect(Shutdown() == ShutdownResult::Completed, "ProfileDump runtime failed to shut down");
}

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& _path) {
    std::ifstream stream(_path, std::ios::binary | std::ios::ate);
    Expect(stream.is_open(), "profile scope output could not be opened");
    const std::streamoff size = stream.tellg();
    Expect(size >= 0, "profile scope output size is invalid");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    Expect(stream.good() || stream.eof(), "profile scope output could not be read");
    return bytes;
}

struct ParsedDump {
    std::vector<SchemaDescriptor> schemas;
    std::vector<DecodedRecord>    records;
};

ParsedDump ParseDump(const std::filesystem::path& _path, const CodecLimits& _limits) {
    const std::vector<std::uint8_t> bytes = ReadBinaryFile(_path);
    Expect(!bytes.empty(), "profile scope output is empty");

    ParsedDump  parsed;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        PacketView  packet{};
        std::size_t consumed = 0;
        Expect(
            DecodePacket(std::span<const std::uint8_t>(bytes).subspan(offset), _limits, packet, consumed) ==
                DecodeStatus::Ok,
            "profile scope packet failed validation"
        );
        Expect(consumed != 0, "profile scope decoder made no progress");

        if (packet.header.type == PacketType::Schema) {
            SchemaDescriptor schema{};
            Expect(
                DecodeSchemaPayload(packet, _limits, schema) == DecodeStatus::Ok,
                "profile scope schema failed to decode"
            );
            parsed.schemas.push_back(std::move(schema));
        } else if (packet.header.type == PacketType::Record) {
            DecodedRecord record{};
            bool          decoded = false;
            for (const SchemaDescriptor& schema : parsed.schemas) {
                if (DecodeRecordPayload(packet, schema, _limits, record) == DecodeStatus::Ok) {
                    decoded = true;
                    break;
                }
            }
            Expect(decoded, "profile scope record did not match a preceding schema");
            parsed.records.push_back(std::move(record));
        }
        offset += consumed;
    }
    return parsed;
}

struct CpuRecordView {
    std::uint64_t thread_id{0};
    std::string_view name{};
    std::uint64_t begin_ns{0};
    std::uint64_t end_ns{0};
    std::uint32_t depth{0};
};

CpuRecordView ViewCpuRecord(const DecodedRecord& _record) {
    Expect(_record.values.size() == 5, "CpuScope record has the wrong field count");
    return {
        .thread_id = std::get<std::uint64_t>(_record.values[0]),
        .name      = std::get<ProfileString>(_record.values[1]),
        .begin_ns  = std::get<std::uint64_t>(_record.values[2]),
        .end_ns    = std::get<std::uint64_t>(_record.values[3]),
        .depth     = std::get<std::uint32_t>(_record.values[4]),
    };
}

const DecodedRecord& FindRecord(const ParsedDump& _dump, std::string_view _name) {
    const auto found = std::find_if(_dump.records.begin(), _dump.records.end(), [&](const auto& _record) {
        return ViewCpuRecord(_record).name == _name;
    });
    Expect(found != _dump.records.end(), "expected CpuScope record was not found");
    return *found;
}

SchemaDescriptor MakeWrongSchema() {
    return {
        .name           = "WrongCpuScope",
        .event_type     = "contract.wrong_cpu_scope",
        .kind           = EventKind::Instant,
        .channel        = Channel::CpuThread,
        .schema_version = 1,
        .fields         = {{"value", FieldType::UInt64}},
    };
}

void TestProfileDumpConfigParsing() {
    ScopedOutput output("moer-profile-scope-config");

    const std::filesystem::path defaults_path = output.directory / "defaults.toml";
    WriteTextFile(defaults_path, "[engine]\n");
    const Moer::Config::GlobalConfig defaults =
        Moer::Config::GlobalConfig::LoadConfigFromTomlFile(defaults_path.generic_string());
    Expect(!defaults.engine.profile_dump.enabled, "profile dump default must remain disabled");
    Expect(
        defaults.engine.profile_dump.output_path == "./profile/MoerProfile.mpd",
        "profile dump default output path changed"
    );
    Expect(
        defaults.engine.profile_dump.replace_existing,
        "profile dump default replacement policy changed"
    );

    const std::filesystem::path explicit_path = output.directory / "explicit.toml";
    WriteTextFile(
        explicit_path,
        "[engine.profile_dump]\n"
        "enabled = true\n"
        "output_path = \"./capture/custom.mpd\"\n"
        "replace_existing = false\n"
    );
    const Moer::Config::GlobalConfig explicit_config =
        Moer::Config::GlobalConfig::LoadConfigFromTomlFile(explicit_path.generic_string());
    Expect(explicit_config.engine.profile_dump.enabled, "profile dump enabled flag was not parsed");
    Expect(
        explicit_config.engine.profile_dump.output_path == "./capture/custom.mpd",
        "profile dump output path was not parsed"
    );
    Expect(
        !explicit_config.engine.profile_dump.replace_existing,
        "profile dump replacement policy was not parsed"
    );
}

void TestDisabledAndActivationValidation() {
    static_assert(!std::is_copy_constructible_v<ScopedCpuProfile>);
    static_assert(!std::is_move_constructible_v<ScopedCpuProfile>);
    static_assert(ScopedCpuProfile::kMaxNameBytes == 256);

    CpuScopeProducer::Deactivate();
    Expect(!CpuScopeProducer::IsActive(), "producer began active");
    { MOER_PROFILE_SCOPE("Disabled.BeforeRuntime"); }
    const SchemaHandle plausible{
        .hash       = ComputeSchemaHash(Templates::CpuScope()),
        .generation = 1,
    };
    Expect(
        CpuScopeProducer::Activate({}) == CpuScopeActivationResult::InvalidHandle,
        "producer accepted an empty handle"
    );
    Expect(
        CpuScopeProducer::Activate(plausible) == CpuScopeActivationResult::RuntimeNotRunning,
        "producer activated without a running runtime"
    );

    ScopedOutput        output("moer-profile-scope-activation");
    const RuntimeConfig config = MakeRuntimeConfig(output);
    Expect(Start(config) == StartResult::Started, "activation runtime failed to start");
    { ScopedCpuProfile disabled("Disabled.WhileRunning"); }

    const SchemaRegistration wrong = RegisterSchema(MakeWrongSchema());
    const SchemaRegistration cpu   = RegisterSchema(Templates::CpuScope());
    Expect(wrong.status == SchemaStatus::Registered, "wrong-schema fixture failed to register");
    Expect(cpu.status == SchemaStatus::Registered, "CpuScope fixture failed to register");
    Expect(
        CpuScopeProducer::Activate(
            {.hash = cpu.handle.hash, .generation = cpu.handle.generation + 1}
        ) == CpuScopeActivationResult::StaleGeneration,
        "producer accepted a stale generation"
    );
    Expect(
        CpuScopeProducer::Activate(wrong.handle) == CpuScopeActivationResult::WrongSchema,
        "producer accepted a different schema"
    );
    Expect(
        CpuScopeProducer::Activate(cpu.handle) == CpuScopeActivationResult::Activated,
        "valid CpuScope handle did not activate"
    );
    Expect(CpuScopeProducer::IsActive(), "producer did not publish active state");
    Expect(
        CpuScopeProducer::Activate(cpu.handle) == CpuScopeActivationResult::AlreadyActive,
        "duplicate activation was not idempotent"
    );
    { ScopedCpuProfile valid("Enabled.AfterActivation"); }
    StopCpuProducer();

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 1, "disabled scopes emitted records");
    Expect(
        ViewCpuRecord(dump.records.front()).name == "Enabled.AfterActivation",
        "enabled scope emitted the wrong name"
    );
}

void TestNestedDecode() {
    ScopedOutput        output("moer-profile-scope-nested");
    const RuntimeConfig config = MakeRuntimeConfig(output);
    static_cast<void>(StartCpuProducer(config));
    {
        MOER_PROFILE_SCOPE("Nested.Outer");
        { ScopedCpuProfile inner("Nested", "Inner"); }
    }
    StopCpuProducer();

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.schemas.size() == 1, "nested capture wrote the wrong schema count");
    Expect(dump.schemas.front() == Templates::CpuScope(), "nested capture schema drifted");
    Expect(dump.records.size() == 2, "nested capture wrote the wrong record count");

    const CpuRecordView outer = ViewCpuRecord(FindRecord(dump, "Nested.Outer"));
    const CpuRecordView inner = ViewCpuRecord(FindRecord(dump, "Nested.Inner"));
    Expect(outer.thread_id != 0 && inner.thread_id == outer.thread_id, "nested thread id changed");
    Expect(outer.depth == 0 && inner.depth == 1, "nested TLS depth is incorrect");
    Expect(
        outer.begin_ns <= inner.begin_ns && inner.end_ns <= outer.end_ns,
        "nested timestamps are not contained"
    );
    Expect(outer.end_ns >= outer.begin_ns && inner.end_ns >= inner.begin_ns, "scope duration reversed");
}

void TestMultithreadedTlsDepth() {
    ScopedOutput        output("moer-profile-scope-threads");
    const RuntimeConfig config = MakeRuntimeConfig(output);
    static_cast<void>(StartCpuProducer(config));

    constexpr std::size_t thread_count = 4;
    std::vector<std::jthread> threads;
    threads.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        threads.emplace_back([index] {
            const std::string suffix = std::to_string(index);
            ScopedCpuProfile outer("Thread.Outer." + suffix);
            ScopedCpuProfile inner("Thread.Inner." + suffix);
        });
    }
    threads.clear();
    StopCpuProducer();

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == thread_count * 2, "multithreaded capture lost a scope");
    std::unordered_set<std::uint64_t> thread_ids;
    for (std::size_t index = 0; index < thread_count; ++index) {
        const std::string   suffix = std::to_string(index);
        const CpuRecordView outer = ViewCpuRecord(FindRecord(dump, "Thread.Outer." + suffix));
        const CpuRecordView inner = ViewCpuRecord(FindRecord(dump, "Thread.Inner." + suffix));
        Expect(outer.thread_id == inner.thread_id, "one thread changed id between nested scopes");
        Expect(outer.depth == 0 && inner.depth == 1, "TLS depth leaked between producer threads");
        thread_ids.insert(outer.thread_id);
    }
    Expect(thread_ids.size() == thread_count, "worker scopes did not retain distinct thread ids");
}

void TestRestartRejectsStaleScope() {
    ScopedOutput        first_output("moer-profile-scope-stale-first");
    const RuntimeConfig first_config = MakeRuntimeConfig(first_output);
    const SchemaHandle  first_handle = StartCpuProducer(first_config);
    std::optional<ScopedCpuProfile> stale_scope;
    stale_scope.emplace("Stale.OldSession");
    CpuScopeProducer::Deactivate();
    Expect(Shutdown() == ShutdownResult::Completed, "first stale-session shutdown failed");

    ScopedOutput        second_output("moer-profile-scope-stale-second");
    const RuntimeConfig second_config = MakeRuntimeConfig(second_output);
    const SchemaHandle  second_handle = StartCpuProducer(second_config);
    Expect(
        second_handle.generation != first_handle.generation,
        "ProfileDump restart reused a producer generation"
    );
    { ScopedCpuProfile current("Fresh.BeforeStaleDestructor"); }
    stale_scope.reset();
    Expect(
        CpuScopeProducer::IsActive(),
        "stale destructor disabled the restarted producer"
    );
    Expect(
        GetRuntimeStats().records_dropped_stale_generation == 0,
        "stale destructor charged the restarted session"
    );
    { ScopedCpuProfile current("Fresh.AfterStaleDestructor"); }
    StopCpuProducer();

    const ParsedDump dump = ParseDump(second_output.path, second_config.codec_limits);
    Expect(dump.records.size() == 2, "stale generation polluted the restarted capture");
    static_cast<void>(FindRecord(dump, "Fresh.BeforeStaleDestructor"));
    static_cast<void>(FindRecord(dump, "Fresh.AfterStaleDestructor"));
}

void TestRestartReplacesStaleProducerPublication() {
    ScopedOutput        first_output("moer-profile-scope-stale-publication-first");
    const RuntimeConfig first_config = MakeRuntimeConfig(first_output);
    const SchemaHandle  first_handle = StartCpuProducer(first_config);
    std::optional<ScopedCpuProfile> stale_scope;
    stale_scope.emplace("Stale.BeforeMissedDeactivate");
    Expect(
        Shutdown() == ShutdownResult::Completed,
        "stale-publication first runtime failed to shut down"
    );
    Expect(
        !CpuScopeProducer::IsActive(),
        "producer reported an old stopped generation as active"
    );

    ScopedOutput        second_output("moer-profile-scope-stale-publication-second");
    const RuntimeConfig second_config = MakeRuntimeConfig(second_output);
    Expect(Start(second_config) == StartResult::Started, "stale-publication restart failed");
    stale_scope.reset();
    Expect(
        GetRuntimeStats().records_dropped_stale_generation == 0,
        "old scope charged the restarted runtime before producer activation"
    );
    const SchemaRegistration second_schema = RegisterSchema(Templates::CpuScope());
    Expect(
        second_schema.status == SchemaStatus::Registered &&
            second_schema.handle.generation != first_handle.generation,
        "stale-publication restart did not create a new schema generation"
    );
    Expect(
        CpuScopeProducer::Activate(second_schema.handle) ==
            CpuScopeActivationResult::Activated,
        "new generation could not replace stale producer publication"
    );
    { ScopedCpuProfile current("Fresh.AfterMissedDeactivate"); }
    StopCpuProducer();

    const ParsedDump dump = ParseDump(second_output.path, second_config.codec_limits);
    Expect(dump.records.size() == 1, "stale producer publication polluted the restarted session");
    static_cast<void>(FindRecord(dump, "Fresh.AfterMissedDeactivate"));
}

void TestNameBoundariesAndDepthRecovery() {
    ScopedOutput        output("moer-profile-scope-names");
    const RuntimeConfig config = MakeRuntimeConfig(output);
    static_cast<void>(StartCpuProducer(config));

    const std::string accepted(ScopedCpuProfile::kMaxNameBytes, 'a');
    const std::string rejected(ScopedCpuProfile::kMaxNameBytes + 1, 'b');
    { ScopedCpuProfile scope(accepted); }
    {
        ScopedCpuProfile ignored(rejected);
        ScopedCpuProfile recovered("Name.AfterRejected");
    }
    {
        ScopedCpuProfile ignored("");
        ScopedCpuProfile recovered("Name.AfterEmpty");
    }
    {
        const std::string prefix(100, 'p');
        const std::string name(155, 'n');
        ScopedCpuProfile  joined(prefix, name);
    }
    {
        const std::string prefix(100, 'q');
        const std::string name(156, 'n');
        ScopedCpuProfile  ignored(prefix, name);
        ScopedCpuProfile  recovered("Name.AfterRejectedPrefix");
    }
    StopCpuProducer();

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 5, "name validation accepted or rejected the wrong boundary");
    Expect(ViewCpuRecord(FindRecord(dump, accepted)).depth == 0, "256-byte name changed depth");
    Expect(
        ViewCpuRecord(FindRecord(dump, "Name.AfterRejected")).depth == 0,
        "257-byte name leaked TLS depth"
    );
    Expect(
        ViewCpuRecord(FindRecord(dump, "Name.AfterEmpty")).depth == 0,
        "empty name leaked TLS depth"
    );
    Expect(
        ViewCpuRecord(FindRecord(dump, std::string(100, 'p') + "." + std::string(155, 'n'))).depth == 0,
        "256-byte joined name was rejected"
    );
    Expect(
        ViewCpuRecord(FindRecord(dump, "Name.AfterRejectedPrefix")).depth == 0,
        "oversized joined name leaked TLS depth"
    );
}

class TaskSystemGuard {
public:
    TaskSystemGuard() {
        Moer::TaskSystem::Init();
        active_ = true;
    }
    ~TaskSystemGuard() {
        if (active_) {
            Moer::TaskSystem::ShutDown();
        }
    }
    void Shutdown() {
        if (active_) {
            Moer::TaskSystem::ShutDown();
            active_ = false;
        }
    }

private:
    bool active_{false};
};

class WriterResumeGuard {
public:
    ~WriterResumeGuard() {
        if (active_) {
            Testing::ResumeWriter();
        }
    }

    void Resume() {
        if (active_) {
            Testing::ResumeWriter();
            active_ = false;
        }
    }

private:
    bool active_{true};
};

void TestTaskSystemShutdownPublishesWorkerTls() {
    ScopedOutput  output("moer-profile-scope-task-shutdown");
    RuntimeConfig config = MakeRuntimeConfig(output);
    config.tls_publish_records = 64;
    static_cast<void>(StartCpuProducer(config));

    TaskSystemGuard task_system;
    GraphEventRef event = LambdaTask::Dispatch([] {
        ScopedCpuProfile outer("Task.Worker.Outer");
        ScopedCpuProfile inner("Task.Worker.Inner");
    });
    event->Wait();
    task_system.Shutdown();

    StopCpuProducer();
    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 2, "TaskSystem shutdown did not publish worker TLS");
    Expect(
        ViewCpuRecord(FindRecord(dump, "Task.Worker.Outer")).depth == 0 &&
            ViewCpuRecord(FindRecord(dump, "Task.Worker.Inner")).depth == 1,
        "TaskSystem worker depth changed"
    );
}

void TestQueueFullDoesNotDisableProducer() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureWriterPauseAfterStart(true),
        "writer pause hook could not be configured"
    );
    WriterResumeGuard resume_guard;

    ScopedOutput  output("moer-profile-scope-queue-full");
    RuntimeConfig config = MakeRuntimeConfig(output);
    config.max_record_bytes    = 4096;
    config.tls_publish_records = 1;
    config.tls_publish_bytes   = 4096;
    config.tls_max_records     = 1;
    config.tls_max_bytes       = 4096;
    config.queue_max_chunks    = 1;
    config.queue_max_records   = 1;
    config.queue_max_bytes     = 4096;
    static_cast<void>(StartCpuProducer(config));
    Expect(Testing::WaitForWriterPaused(2000), "writer did not reach the queue-full pause point");

    { ScopedCpuProfile accepted("QueueFull.Accepted"); }
    { ScopedCpuProfile dropped("QueueFull.Dropped"); }
    Expect(
        CpuScopeProducer::IsActive(),
        "bounded queue pressure disabled the CPU producer"
    );

    resume_guard.Resume();
    ExpectFlushSucceeded(FlushAll(), "queue-full recovery did not drain accepted work");
    { ScopedCpuProfile recovered("QueueFull.Recovered"); }
    StopCpuProducer();
    Testing::ClearHooks();

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 2, "queue-full recovery wrote the wrong record count");
    static_cast<void>(FindRecord(dump, "QueueFull.Accepted"));
    static_cast<void>(FindRecord(dump, "QueueFull.Recovered"));
}

void TestFaultSelfDisables() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureFault(Testing::FaultPoint::WritePacket, 2),
        "writer fault hook could not be configured"
    );

    ScopedOutput  output("moer-profile-scope-fault");
    RuntimeConfig config = MakeRuntimeConfig(output);
    config.tls_publish_records = 1;
    static_cast<void>(StartCpuProducer(config));
    { ScopedCpuProfile first("Fault.Trigger"); }
    Expect(FlushAll() == FlushResult::Faulted, "writer fault did not become observable");
    Expect(CpuScopeProducer::IsActive(), "producer stopped before observing an Emit failure");
    { ScopedCpuProfile second("Fault.Observe"); }
    Expect(!CpuScopeProducer::IsActive(), "sink fault did not self-disable the producer");
    { ScopedCpuProfile ignored("Fault.AfterDisable"); }
    Expect(Shutdown() == ShutdownResult::Faulted, "faulted writer shutdown reported success");
    Testing::ClearHooks();
}

} // namespace

int main() {
    try {
        TestProfileDumpConfigParsing();
        TestDisabledAndActivationValidation();
        TestNestedDecode();
        TestMultithreadedTlsDepth();
        TestRestartRejectsStaleScope();
        TestRestartReplacesStaleProducerPublication();
        TestNameBoundariesAndDepthRecovery();
        TestTaskSystemShutdownPublishesWorkerTls();
        TestQueueFullDoesNotDisableProducer();
        TestFaultSelfDisables();
        std::cout << "ProfileScope producer contract tests passed." << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        CpuScopeProducer::Deactivate();
        if (TaskGraph::IsInitialized()) {
            Moer::TaskSystem::ShutDown();
        }
        static_cast<void>(Shutdown());
        Testing::ClearHooks();
        std::cerr << "ProfileScope producer contract test failed: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
