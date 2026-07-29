#include "ProfileDumpTesting.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpCodec.h"
#include "profile/ProfileDumpTemplates.h"
#include "profile/RenderProfileCapture.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIQuery.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace Moer;
using namespace Moer::ProfileDump;
using namespace Moer::Render;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

void ExpectNear(
    double           _actual,
    double           _expected,
    std::string_view _message
) {
    if (std::abs(_actual - _expected) > 1.0e-9) {
        throw std::runtime_error(std::string(_message));
    }
}

class ScopedOutput {
public:
    explicit ScopedOutput(std::string_view _stem) {
        static std::atomic<std::uint64_t> next_id{0};
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto now = static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().
                    time_since_epoch().count()
            );
            directory = std::filesystem::temp_directory_path() /
                        (std::string(_stem) + "-" +
                         std::to_string(now) + "-" +
                         std::to_string(next_id.fetch_add(
                             1, std::memory_order_relaxed
                         )));
            std::error_code error;
            if (std::filesystem::create_directory(directory, error)) {
                path = directory / "capture.mpd";
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error(
            "RenderProfileCapture test could not reserve a temp directory"
        );
    }

    ~ScopedOutput() {
        static_cast<void>(ProfileDump::Shutdown());
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    [[nodiscard]] bool HasInProgressFile() const {
        std::error_code error;
        for (std::filesystem::directory_iterator it(directory, error), end;
             !error && it != end;
             it.increment(error)) {
            if (it->path().filename().string().ends_with(
                    ".inprogress"
                )) {
                return true;
            }
        }
        return false;
    }

    std::filesystem::path directory{};
    std::filesystem::path path{};
};

struct ParsedDump {
    std::vector<PacketType>       packet_types{};
    std::vector<SchemaDescriptor> schemas{};
    std::vector<DecodedRecord>    records{};
};

std::vector<std::uint8_t> ReadBinary(
    const std::filesystem::path& _path
) {
    std::ifstream file(_path, std::ios::binary | std::ios::ate);
    Expect(file.is_open(), "capture output could not be opened");
    const std::streamoff size = file.tellg();
    Expect(size >= 0, "capture output size is invalid");
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(size)
    );
    file.seekg(0);
    if (!bytes.empty()) {
        file.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size())
        );
    }
    Expect(file.good() || file.eof(), "capture output read failed");
    return bytes;
}

ParsedDump ParseDump(const std::filesystem::path& _path) {
    const std::vector<std::uint8_t> bytes = ReadBinary(_path);
    Expect(!bytes.empty(), "capture output is empty");

    ParsedDump parsed{};
    CodecLimits limits{};
    std::size_t offset = 0;
    std::uint64_t expected_packet_index = 0;
    while (offset < bytes.size()) {
        PacketView packet{};
        std::size_t consumed = 0;
        Expect(
            DecodePacket(
                std::span<const std::uint8_t>(bytes).subspan(
                    offset
                ),
                limits,
                packet,
                consumed
            ) == DecodeStatus::Ok,
            "capture packet decode failed"
        );
        Expect(
            consumed != 0 &&
                packet.header.packet_index ==
                    expected_packet_index++,
            "capture packet order is not contiguous"
        );
        parsed.packet_types.push_back(packet.header.type);

        if (packet.header.type == PacketType::Schema) {
            SchemaDescriptor schema{};
            Expect(
                DecodeSchemaPayload(packet, limits, schema) ==
                    DecodeStatus::Ok,
                "capture schema decode failed"
            );
            parsed.schemas.push_back(std::move(schema));
        } else if (packet.header.type == PacketType::Record) {
            DecodedRecord record{};
            bool decoded = false;
            for (const SchemaDescriptor& schema : parsed.schemas) {
                if (DecodeRecordPayload(
                        packet, schema, limits, record
                    ) == DecodeStatus::Ok) {
                    decoded = true;
                    break;
                }
            }
            Expect(
                decoded,
                "record appeared before its schema or did not match it"
            );
            parsed.records.push_back(std::move(record));
        }
        offset += consumed;
    }
    Expect(offset == bytes.size(), "capture has trailing bytes");
    return parsed;
}

const SchemaDescriptor& FindSchema(
    const ParsedDump& _dump,
    std::string_view  _name
) {
    const auto it = std::ranges::find_if(
        _dump.schemas,
        [&](const SchemaDescriptor& _schema) {
            return std::string_view(_schema.name) == _name;
        }
    );
    Expect(it != _dump.schemas.end(), "expected schema is missing");
    return *it;
}

std::vector<const DecodedRecord*> RecordsFor(
    const ParsedDump& _dump,
    const SchemaDescriptor& _schema
) {
    const std::uint64_t hash = ComputeSchemaHash(_schema);
    std::vector<const DecodedRecord*> result{};
    for (const DecodedRecord& record : _dump.records) {
        if (record.schema_hash == hash) {
            result.push_back(&record);
        }
    }
    return result;
}

template<typename T>
const T& ValueAt(
    const DecodedRecord& _record,
    std::size_t          _index
) {
    Expect(
        _index < _record.values.size(),
        "decoded record field index is out of range"
    );
    return std::get<T>(_record.values[_index]);
}

std::string_view StringAt(
    const DecodedRecord& _record,
    std::size_t          _index
) {
    return std::string_view(
        ValueAt<ProfileString>(_record, _index)
    );
}

RHIQueueBinding Binding(
    EQueueType   _queue,
    std::uint32_t _native_queue_id,
    std::uint32_t _family_id
) {
    return {
        .queue = _queue,
        .native_queue_id = _native_queue_id,
        .family_id = _family_id,
        .available = true,
    };
}

TimestampQueryResult Timestamp(
    std::uint64_t _begin,
    std::uint64_t _end,
    double        _period = 1.0
) {
    return {
        .begin_tick = _begin,
        .end_tick = _end,
        .valid_bits = 64,
        .tick_period_ns = _period,
        .duration_ns =
            static_cast<double>(_end - _begin) * _period,
    };
}

void ResolveTimestampAt(
    CmdSubmit&                  _submit,
    std::size_t                 _index,
    const TimestampQueryResult& _timestamp
) {
    Expect(
        _index < _submit.query_tokens.size(),
        "test timestamp token index is out of range"
    );
    Expect(
        QueryBackendAccess::ResolveTimestamp(
            _submit.query_tokens[_index], _timestamp
        ),
        "test timestamp did not win terminal publication"
    );
}

void ReleaseSubmit(CmdSubmit& _submit) {
    _submit.callbacks.clear();
    _submit.query_tokens.clear();
}

struct RegisteredGpuSchemas {
    SchemaHandle frame{};
    SchemaHandle scope{};
};

RegisteredGpuSchemas RegisterGpuSchemas() {
    const SchemaRegistration frame =
        RegisterSchema(Templates::GpuFrame());
    const SchemaRegistration scope =
        RegisterSchema(Templates::GpuScope());
    Expect(
        (frame.status == SchemaStatus::Registered ||
         frame.status == SchemaStatus::AlreadyRegistered) &&
            frame.handle,
        "GpuFrame schema registration failed"
    );
    Expect(
        (scope.status == SchemaStatus::Registered ||
         scope.status == SchemaStatus::AlreadyRegistered) &&
            scope.handle,
        "GpuScope schema registration failed"
    );
    return {frame.handle, scope.handle};
}

void StartRuntime(
    const ScopedOutput& _output,
    RuntimeConfig       _config = {}
) {
    RuntimeConfig config = std::move(_config);
    config.output_path = _output.path;
    config.replace_existing = true;
    const StartResult result = Start(config);
    if (result != StartResult::Started) {
        throw std::runtime_error(
            "ProfileDump runtime did not start (result=" +
            std::to_string(static_cast<unsigned int>(result)) + ")"
        );
    }
}

class WriterResumeGuard {
public:
    ~WriterResumeGuard() {
        if (active_) {
            Testing::ResumeWriter();
        }
    }

    void Resume() noexcept {
        if (active_) {
            Testing::ResumeWriter();
            active_ = false;
        }
    }

private:
    bool active_{true};
};

void FinishRuntime(const ScopedOutput& _output) {
    const FlushResult flush = FlushThreadLocal();
    Expect(
        flush == FlushResult::Completed ||
            flush == FlushResult::NothingPending,
        "ProfileDump producer flush failed"
    );
    Expect(
        Shutdown() == ShutdownResult::Completed,
        "ProfileDump runtime did not shut down cleanly"
    );
    Expect(
        std::filesystem::is_regular_file(_output.path),
        "ProfileDump final file is missing"
    );
    Expect(
        !_output.HasInProgressFile(),
        "ProfileDump left an in-progress file"
    );
}

void DefaultSessionFacadeIsInactive() {
    RenderProfileCapture            capture;
    const RenderProfileCaptureStats stats = capture.GetStats();

    Expect(!capture.Valid() && !capture.BeginFrame().Valid(), "default GPU capture facade admitted work");
    Expect(
        capture.RequestStop() == RenderProfileSessionStopResult::Inactive,
        "default GPU capture facade accepted a stop request"
    );
    Expect(
        capture.TryFinishSession() == RenderProfileSessionFinishResult::Inactive,
        "default GPU capture facade reported a terminal session"
    );
    Expect(
        !stats.accepting && !stats.closed && stats.generation == 0,
        "default GPU capture facade exposed non-empty state"
    );
}

void LegacyConstructorRejectsInvalidStreamConfiguration() {
    ScopedOutput output("moer-render-profile-invalid-config");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    GpuScopeStreamConfig       invalid_config{};
    invalid_config.max_resident_frames = 0;

    bool rejected = false;
    try {
        RenderProfileCapture capture(schemas.frame, schemas.scope, invalid_config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    Expect(rejected, "legacy GPU capture constructor silently accepted invalid stream configuration");
    FinishRuntime(output);
}

void StartValidationExceptionsReturnStableResults() {
    ScopedOutput output("moer-render-profile-start-validation-exception");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture       capture;

    RenderProfileTesting::InjectNextStartValidationBadAlloc();
    Expect(
        capture.StartSession({}, {}) ==
            RenderProfileSessionStartResult::InvalidSchema,
        "invalid handles did not retain precedence over lazy schema initialization"
    );
    RenderProfileTesting::ClearStartValidationException();

    RenderProfileTesting::InjectNextStartValidationBadAlloc();
    Expect(
        capture.StartSession(schemas.frame, schemas.scope) ==
            RenderProfileSessionStartResult::ResourceExhausted,
        "schema descriptor allocation failure escaped the noexcept start boundary"
    );
    Expect(
        !capture.Valid() && capture.GetStats().generation == 0,
        "schema descriptor allocation failure published session state"
    );

    RenderProfileTesting::InjectNextStartValidationException();
    Expect(
        capture.StartSession(schemas.frame, schemas.scope) ==
            RenderProfileSessionStartResult::InvalidConfiguration,
        "schema descriptor exception escaped the noexcept start boundary"
    );
    Expect(
        !capture.Valid() && capture.GetStats().generation == 0,
        "schema descriptor exception published session state"
    );

    Expect(
        capture.StartSession(schemas.frame, schemas.scope) ==
            RenderProfileSessionStartResult::Started,
        "capture did not recover after translated start-validation exceptions"
    );
    Expect(
        capture.TryFinishSession() ==
            RenderProfileSessionFinishResult::Closed,
        "recovered capture session did not close cleanly"
    );
    RenderProfileTesting::ClearStartValidationException();
    FinishRuntime(output);
}

void FinishSessionOwnsStopAndTokenDestructorSeal() {
    RenderProfileCapture capture;

    {
        ScopedOutput output("moer-render-profile-finish-owned-stop");
        StartRuntime(output);
        const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) == RenderProfileSessionStartResult::Started,
            "self-stopping GPU capture session did not start"
        );
        RenderProfileFrameToken frame = capture.BeginFrame();
        Expect(frame.Valid() && capture.Seal(frame), "self-stopping session frame did not seal");
        Expect(
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
            "TryFinishSession did not own its admission-stop boundary"
        );
        const RenderProfileCaptureStats stats = capture.GetStats();
        Expect(
            stats.closed && stats.frames_sealed == 1 && stats.frames_emitted == 1 &&
                stats.shutdown_abandoned_frames == 0,
            "self-stopping session did not drain cleanly"
        );
        FinishRuntime(output);
    }

    {
        ScopedOutput output("moer-render-profile-token-auto-seal");
        StartRuntime(output);
        const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) == RenderProfileSessionStartResult::Started,
            "token auto-seal GPU capture session did not start"
        );
        {
            RenderProfileFrameToken frame = capture.BeginFrame();
            Expect(frame.Valid(), "token auto-seal frame admission failed");
            Expect(
                capture.RequestStop() == RenderProfileSessionStopResult::StopRequested,
                "token auto-seal session stop request failed"
            );
            Expect(frame.Valid(), "stop invalidated the token before its destructor");
        }
        Expect(
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
            "stopped token destructor did not auto-seal and drain"
        );
        const RenderProfileCaptureStats stats = capture.GetStats();
        Expect(
            stats.frames_sealed == 1 && stats.frames_emitted == 1 && stats.shutdown_abandoned_frames == 0,
            "token destructor auto-seal accounting is inconsistent"
        );
        FinishRuntime(output);
    }
}

void SessionGateStopsAdmissionDrainsAndRestartsNextGeneration() {
    RenderProfileCapture capture;
    std::uint64_t        first_generation = 0;

    {
        ScopedOutput output("moer-render-profile-session-a");
        StartRuntime(output);
        const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) == RenderProfileSessionStartResult::Started,
            "first GPU capture session did not start"
        );
        first_generation = capture.GetStats().generation;
        Expect(first_generation != 0 && capture.Valid(), "first GPU capture session has no live generation");

        RenderProfileFrameToken frame = capture.BeginFrame();
        Expect(frame.Valid() && frame.FrameId() == 1, "first session frame admission failed");
        Expect(
            capture.RequestStop() == RenderProfileSessionStopResult::StopRequested,
            "first session stop request was not accepted"
        );
        Expect(
            capture.RequestStop() == RenderProfileSessionStopResult::AlreadyStopping,
            "repeated session stop did not report an existing stop"
        );
        Expect(
            !capture.Valid() && !capture.BeginFrame().Valid(),
            "stopping session continued to admit new frames"
        );
        Expect(frame.Valid(), "stopping session invalidated an admitted frame token");
        Expect(
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Pending,
            "session closed while an admitted frame was still unsealed"
        );

        CommandList list(EQueueType::Graphics);
        Expect(
            capture.BindSource(frame, list, Binding(EQueueType::Graphics, 6, 12), 4) ==
                RenderProfileBindResult::Bound,
            "stopping session rejected an admitted frame source"
        );
        list.PushScopeWithTimeScope("SessionA.AcceptedBeforeStop");
        list.PopScopeWithTimeScope();
        CmdSubmit submit = list.Submit();
        Expect(capture.Seal(frame), "stopping session could not seal an admitted frame");
        Expect(
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Pending,
            "session closed before its admitted query completed"
        );

        ResolveTimestampAt(submit, 0, Timestamp(40, 55));
        Expect(
            capture.GetStats().stream.resident_ready_frames == 1,
            "completed query did not publish a ready frame before owner polling"
        );
        Expect(
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
            "finish did not drain the ready frame and close cleanly"
        );
        const RenderProfileCaptureStats closed = capture.GetStats();
        Expect(
            closed.closed && !closed.accepting && closed.frames_admitted == 1 && closed.frames_emitted == 1 &&
                closed.scopes_emitted == 1 && closed.shutdown_abandoned_frames == 0 &&
                closed.terminal_faults == 0,
            "clean session close statistics are inconsistent"
        );
        Expect(
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
            "clean session finish was not idempotent"
        );
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) ==
                RenderProfileSessionStartResult::GenerationAlreadyUsed,
            "closed session rebound inside one ProfileDump generation"
        );

        ReleaseSubmit(submit);
        FinishRuntime(output);
        const ParsedDump dump = ParseDump(output.path);
        Expect(
            RecordsFor(dump, FindSchema(dump, "GpuFrame")).size() == 1 &&
                RecordsFor(dump, FindSchema(dump, "GpuScope")).size() == 1,
            "first session did not persist its clean drain"
        );
    }

    {
        ScopedOutput output("moer-render-profile-session-b");
        StartRuntime(output);
        const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) == RenderProfileSessionStartResult::Started,
            "stable facade did not start in the next runtime generation"
        );
        Expect(
            capture.GetStats().generation != first_generation,
            "stable facade retained the previous runtime generation"
        );

        RenderProfileFrameToken frame = capture.BeginFrame();
        Expect(
            frame.Valid() && frame.FrameId() == 1 && capture.Seal(frame),
            "second session did not reset and seal frame identity"
        );
        Expect(
            capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
                capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
            "second session did not close cleanly"
        );
        const RenderProfileCaptureStats closed = capture.GetStats();
        Expect(
            closed.frames_emitted == 1 && closed.scopes_emitted == 0 && closed.shutdown_abandoned_frames == 0,
            "second session inherited first-generation accounting"
        );

        FinishRuntime(output);
        const ParsedDump dump = ParseDump(output.path);
        Expect(
            RecordsFor(dump, FindSchema(dump, "GpuFrame")).size() == 1 && dump.records.size() == 1,
            "second session output contains cross-generation records"
        );
    }
}

void AbortedPendingQueryCannotPolluteNextRuntimeGeneration() {
    RenderProfileCapture     capture;
    CommandList              persistent(EQueueType::Graphics);
    std::optional<CmdSubmit> old_submit{};
    RenderProfileFrameToken  stale_token{};

    {
        ScopedOutput output("moer-render-profile-session-abort-old");
        StartRuntime(output);
        const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) == RenderProfileSessionStartResult::Started,
            "old GPU capture session did not start"
        );

        RenderProfileFrameToken frame = capture.BeginFrame();
        Expect(
            capture.BindSource(frame, persistent, Binding(EQueueType::Graphics, 7, 13), 1) ==
                RenderProfileBindResult::Bound,
            "old pending source bind failed"
        );
        persistent.PushScopeWithTimeScope("OldGeneration.Pending");
        persistent.PopScopeWithTimeScope();
        old_submit.emplace(persistent.Submit());
        Expect(capture.Seal(frame), "old pending frame seal failed");
        stale_token = capture.BeginFrame();
        Expect(
            stale_token.Valid() && stale_token.FrameId() == 2,
            "old generation stale-token frame admission failed"
        );

        capture.Abort();
        const RenderProfileCaptureStats aborted = capture.GetStats();
        Expect(
            aborted.closed && !aborted.accepting && aborted.shutdown_abandoned_frames == 2 &&
                capture.TryFinishSession() == RenderProfileSessionFinishResult::Aborted,
            "abort did not detach the old pending session"
        );
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) ==
                RenderProfileSessionStartResult::GenerationAlreadyUsed,
            "aborted session rebound inside one ProfileDump generation"
        );
        FinishRuntime(output);
    }

    {
        ScopedOutput output("moer-render-profile-session-abort-new");
        StartRuntime(output);
        const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
        Expect(
            capture.StartSession(schemas.frame, schemas.scope) == RenderProfileSessionStartResult::Started,
            "new GPU capture generation did not rebind"
        );
        CommandList stale_list(EQueueType::Graphics);
        Expect(
            !stale_token.Valid() &&
                capture.BindSource(stale_token, stale_list, Binding(EQueueType::Graphics, 8, 14), 0) ==
                    RenderProfileBindResult::InvalidFrame &&
                !capture.Seal(stale_token) && capture.Valid() && capture.GetStats().frames_attempted == 0,
            "old generation frame token entered or replaced the new session"
        );

        RenderProfileFrameToken frame = capture.BeginFrame();
        Expect(
            capture.BindSource(frame, persistent, Binding(EQueueType::Graphics, 8, 14), 2) ==
                RenderProfileBindResult::Bound,
            "persistent CommandList did not bind to the new session"
        );
        persistent.PushScopeWithTimeScope("NewGeneration.Current");
        persistent.PopScopeWithTimeScope();
        CmdSubmit current_submit = persistent.Submit();
        Expect(capture.Seal(frame), "new generation frame seal failed");
        const RenderProfileCaptureStats before_old_completion = capture.GetStats();
        Expect(
            before_old_completion.stream.resident_frames == 1 &&
                before_old_completion.stream.resident_pending_frames == 1 &&
                before_old_completion.frames_emitted == 0,
            "new generation did not retain its same-id frame while the old callback was pending"
        );

        Expect(
            QueryBackendAccess::ResolveTimestamp(old_submit->query_tokens.front(), Timestamp(1, 2)),
            "old generation query could not complete safely"
        );
        Expect(capture.DrainReadyFrames() == 0, "old generation completion became a current frame");
        const RenderProfileCaptureStats after_old_completion = capture.GetStats();
        Expect(
            after_old_completion.frames_emitted == before_old_completion.frames_emitted &&
                after_old_completion.scopes_emitted == before_old_completion.scopes_emitted &&
                after_old_completion.frames_invalid == before_old_completion.frames_invalid &&
                after_old_completion.stream.resident_frames == before_old_completion.stream.resident_frames,
            "old generation completion mutated the current session"
        );

        ResolveTimestampAt(current_submit, 0, Timestamp(100, 125));
        Expect(capture.DrainReadyFrames() == 1, "new generation frame did not drain");
        Expect(
            capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
                capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
            "new generation did not close after old completion"
        );
        ReleaseSubmit(current_submit);
        ReleaseSubmit(*old_submit);
        FinishRuntime(output);

        const ParsedDump dump   = ParseDump(output.path);
        const auto       frames = RecordsFor(dump, FindSchema(dump, "GpuFrame"));
        const auto       scopes = RecordsFor(dump, FindSchema(dump, "GpuScope"));
        Expect(
            frames.size() == 1 && scopes.size() == 1 &&
                StringAt(*scopes.front(), 8) == "NewGeneration.Current",
            "old generation callback polluted the new dump"
        );
    }
}

void DelayedStaleStartCannotFaultNewGeneration() {
    RenderProfileCapture            capture;
    RegisteredGpuSchemas            stale_schemas{};
    std::atomic_uint32_t            pause_entered{0};
    std::atomic_bool                pause_release{false};
    RenderProfileSessionStartResult stale_result = RenderProfileSessionStartResult::InvalidSchema;

    ScopedOutput stale_output("moer-render-profile-delayed-start-old");
    StartRuntime(stale_output);
    stale_schemas = RegisterGpuSchemas();
    RenderProfileTesting::InstallStartPublishPause(pause_entered, pause_release);

    std::optional<std::jthread> stale_starter;
    try {
        stale_starter.emplace([&] {
            stale_result = capture.StartSession(stale_schemas.frame, stale_schemas.scope);
        });
    } catch (...) {
        RenderProfileTesting::ClearStartPublishPause();
        throw;
    }

    struct PauseCleanup {
        std::atomic_bool& release;
        bool              active{true};

        ~PauseCleanup() {
            if (active) {
                release.store(true, std::memory_order_release);
                RenderProfileTesting::ClearStartPublishPause();
            }
        }

        void Finish() noexcept {
            release.store(true, std::memory_order_release);
            RenderProfileTesting::ClearStartPublishPause();
            active = false;
        }
    } pause_cleanup{pause_release};

    const auto pause_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pause_entered.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < pause_deadline) {
        std::this_thread::yield();
    }
    if (pause_entered.load(std::memory_order_acquire) == 0) {
        pause_cleanup.Finish();
        stale_starter->join();
        Expect(false, "delayed StartSession did not reach its deterministic publish boundary");
    }

    FinishRuntime(stale_output);

    ScopedOutput current_output("moer-render-profile-delayed-start-current");
    StartRuntime(current_output);
    const RegisteredGpuSchemas            current_schemas = RegisterGpuSchemas();
    const RenderProfileSessionStartResult current_result =
        capture.StartSession(current_schemas.frame, current_schemas.scope);

    pause_cleanup.Finish();
    stale_starter->join();
    Expect(
        current_result == RenderProfileSessionStartResult::Started &&
            stale_result == RenderProfileSessionStartResult::StaleGeneration,
        "delayed stale StartSession did not lose to the current generation"
    );
    const RenderProfileCaptureStats current_stats = capture.GetStats();
    Expect(
        capture.Valid() && current_stats.generation == current_schemas.frame.generation &&
            current_stats.terminal_faults == 0,
        "delayed stale StartSession faulted or replaced the current session"
    );

    RenderProfileFrameToken frame = capture.BeginFrame();
    Expect(frame.Valid() && capture.Seal(frame), "current session failed after delayed stale start");
    Expect(
        capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
        "current session did not close after delayed stale start"
    );
    FinishRuntime(current_output);
}

void DelayedStartCannotPublishStaleCandidate() {
    RenderProfileCapture       capture;
    std::atomic_uint32_t       pause_entered{0};
    std::atomic_bool           pause_release{false};
    RenderProfileSessionStartResult stale_result =
        RenderProfileSessionStartResult::InvalidSchema;

    ScopedOutput stale_output("moer-render-profile-publish-race-old");
    StartRuntime(stale_output);
    const RegisteredGpuSchemas stale_schemas = RegisterGpuSchemas();
    RenderProfileTesting::InstallStartPublishPause(pause_entered, pause_release);

    std::optional<std::jthread> stale_starter;
    try {
        stale_starter.emplace([&] {
            stale_result = capture.StartSession(stale_schemas.frame, stale_schemas.scope);
        });
    } catch (...) {
        RenderProfileTesting::ClearStartPublishPause();
        throw;
    }

    struct PauseCleanup {
        std::atomic_bool& release;
        bool              active{true};

        ~PauseCleanup() {
            if (active) {
                release.store(true, std::memory_order_release);
                RenderProfileTesting::ClearStartPublishPause();
            }
        }

        void Finish() noexcept {
            release.store(true, std::memory_order_release);
            RenderProfileTesting::ClearStartPublishPause();
            active = false;
        }
    } pause_cleanup{pause_release};

    const auto pause_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pause_entered.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < pause_deadline) {
        std::this_thread::yield();
    }
    if (pause_entered.load(std::memory_order_acquire) == 0) {
        pause_cleanup.Finish();
        stale_starter->join();
        Expect(false, "StartSession did not pause immediately before publication");
    }

    FinishRuntime(stale_output);

    ScopedOutput current_output("moer-render-profile-publish-race-current");
    StartRuntime(current_output);
    const RegisteredGpuSchemas current_schemas = RegisterGpuSchemas();

    // No current session is published yet, so the delayed compare-exchange
    // succeeds. The post-CAS generation check must still reject and close
    // only that stale candidate instead of reporting Started.
    pause_cleanup.Finish();
    stale_starter->join();
    const RenderProfileCaptureStats stale_stats = capture.GetStats();
    Expect(
        stale_result == RenderProfileSessionStartResult::StaleGeneration &&
            stale_stats.closed && stale_stats.generation == stale_schemas.frame.generation &&
            stale_stats.terminal_faults == 1,
        "post-publication generation check accepted a stale candidate"
    );

    Expect(
        capture.StartSession(current_schemas.frame, current_schemas.scope) ==
            RenderProfileSessionStartResult::Started,
        "current generation could not replace the rejected stale candidate"
    );
    const RenderProfileCaptureStats current_stats = capture.GetStats();
    Expect(
        capture.Valid() && current_stats.generation == current_schemas.frame.generation &&
            current_stats.terminal_faults == 0,
        "current generation inherited the stale candidate's terminal state"
    );
    RenderProfileFrameToken frame = capture.BeginFrame();
    Expect(frame.Valid() && capture.Seal(frame), "current frame failed after stale publication rejection");
    Expect(
        capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
        "current session did not close after stale publication rejection"
    );
    FinishRuntime(current_output);
}

void ConcurrentSessionStartHasOneWinner() {
    ScopedOutput output("moer-render-profile-concurrent-start");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture       capture;

    constexpr std::size_t thread_count = 16;
    std::barrier          start_line(static_cast<std::ptrdiff_t>(thread_count + 1));
    std::array<RenderProfileSessionStartResult, thread_count> results{};
    results.fill(RenderProfileSessionStartResult::InvalidSchema);
    std::vector<std::jthread> starters;
    starters.reserve(thread_count);
    for (std::size_t index = 0; index < thread_count; ++index) {
        starters.emplace_back([&, index] {
            start_line.arrive_and_wait();
            results[index] = capture.StartSession(schemas.frame, schemas.scope);
        });
    }
    start_line.arrive_and_wait();
    starters.clear();

    const std::size_t started =
        static_cast<std::size_t>(std::ranges::count(results, RenderProfileSessionStartResult::Started));
    const std::size_t already_active =
        static_cast<std::size_t>(std::ranges::count(results, RenderProfileSessionStartResult::AlreadyActive));
    Expect(
        started == 1 && already_active == thread_count - 1 && capture.Valid(),
        "concurrent session start did not elect exactly one winner"
    );

    Expect(
        capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
        "concurrently started session did not close cleanly"
    );
    FinishRuntime(output);
}

void StaleSchemaHandlesCannotReplaceCurrentSession() {
    RegisteredGpuSchemas stale_schemas{};
    {
        ScopedOutput output("moer-render-profile-stale-session");
        StartRuntime(output);
        stale_schemas = RegisterGpuSchemas();
        FinishRuntime(output);
    }

    ScopedOutput output("moer-render-profile-current-session");
    StartRuntime(output);
    const RegisteredGpuSchemas current_schemas = RegisterGpuSchemas();
    RenderProfileCapture       capture;
    Expect(
        capture.StartSession(current_schemas.frame, current_schemas.scope) ==
            RenderProfileSessionStartResult::Started,
        "current GPU capture session did not start"
    );
    const std::uint64_t current_generation = capture.GetStats().generation;
    Expect(
        capture.StartSession(stale_schemas.frame, stale_schemas.scope) ==
            RenderProfileSessionStartResult::StaleGeneration,
        "stale schema handles were not explicitly rejected"
    );
    Expect(
        capture.Valid() && capture.GetStats().generation == current_generation,
        "stale schema handles replaced the current session"
    );

    RenderProfileFrameToken frame = capture.BeginFrame();
    Expect(
        frame.Valid() && frame.FrameId() == 1 && capture.Seal(frame),
        "current session did not survive the stale start attempt"
    );
    Expect(
        capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
            capture.TryFinishSession() == RenderProfileSessionFinishResult::Closed,
        "current session did not close after stale-handle rejection"
    );
    FinishRuntime(output);

    const ParsedDump dump = ParseDump(output.path);
    Expect(
        RecordsFor(dump, FindSchema(dump, "GpuFrame")).size() == 1,
        "stale-handle rejection lost the current frame"
    );
}

void SourceAndScopeDropsCloseWithLoss() {
    ScopedOutput output("moer-render-profile-source-scope-loss");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    GpuScopeStreamConfig       stream_config{};
    stream_config.max_scope_name_bytes = 4;
    RenderProfileCapture capture(schemas.frame, schemas.scope, stream_config);

    RenderProfileFrameToken frame = capture.BeginFrame();
    Expect(frame.Valid(), "loss-classification frame admission failed");

    CommandList rejected_source(EQueueType::Graphics);
    Expect(
        capture.BindSource(frame, rejected_source, RHIQueueBinding{}, 0) ==
            RenderProfileBindResult::InvalidQueue,
        "invalid source did not reach the bridge rejection path"
    );

    CommandList dropped_scope(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            frame,
            dropped_scope,
            Binding(EQueueType::Graphics, 9, 15),
            1
        ) == RenderProfileBindResult::Bound,
        "scope-drop source bind failed"
    );
    dropped_scope.PushScopeWithTimeScope("TooLong");
    dropped_scope.PopScopeWithTimeScope();
    CmdSubmit submit = dropped_scope.Submit();
    Expect(capture.Seal(frame), "loss-classification frame seal failed");

    Expect(
        capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
            capture.TryFinishSession() == RenderProfileSessionFinishResult::ClosedWithLoss,
        "source/scope drops did not produce a lossy terminal state"
    );
    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        stats.closed && stats.sources_rejected == 1 && stats.frames_invalid == 1 &&
            stats.stream.scopes_dropped_name_too_large == 1 &&
            stats.frames_emitted == 1 && stats.terminal_faults == 0,
        "source/scope loss accounting is inconsistent"
    );

    ReleaseSubmit(submit);
    FinishRuntime(output);
}

void HappyPathPreservesTopologyAndRawTimestampDomains() {
    ScopedOutput output("moer-render-profile-happy");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture capture(schemas.frame, schemas.scope);
    Expect(capture.Valid(), "GPU capture bridge is not valid");

    RenderProfileFrameToken frame = capture.BeginFrame();
    Expect(
        frame.Valid() && frame.FrameId() == 1,
        "first GPU frame admission failed"
    );

    CommandList graphics(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            frame,
            graphics,
            Binding(EQueueType::Graphics, 3, 7),
            7
        ) == RenderProfileBindResult::Bound,
        "first Graphics source bind failed"
    );
    graphics.PushScopeWithTimeScope("Outer");
    graphics.PushScopeWithTimeScope("Child");
    graphics.PopScopeWithTimeScope();
    graphics.PopScopeWithTimeScope();
    CmdSubmit first_graphics = graphics.Submit();
    Expect(
        !graphics.HasGpuScopeRecorder(),
        "Submit did not consume the first recording generation"
    );

    Expect(
        capture.BindSource(
            frame,
            graphics,
            Binding(EQueueType::Graphics, 3, 7),
            8
        ) == RenderProfileBindResult::Bound,
        "persistent Graphics CommandList could not rebind"
    );
    graphics.PushScopeWithTimeScope("Tail");
    graphics.PopScopeWithTimeScope();
    CmdSubmit second_graphics = graphics.Submit();

    CommandList compute(EQueueType::Compute);
    Expect(
        capture.BindSource(
            frame,
            compute,
            Binding(EQueueType::Compute, 4, 9),
            9
        ) == RenderProfileBindResult::Bound,
        "Compute source bind failed"
    );
    compute.PushScopeWithTimeScope("Compute");
    compute.PopScopeWithTimeScope();
    CmdSubmit compute_submit = compute.Submit();

    Expect(capture.Seal(frame), "first GPU frame seal failed");
    Expect(
        capture.DrainReadyFrames() == 0,
        "frame became visible before query completion"
    );

    ResolveTimestampAt(first_graphics, 1, Timestamp(120, 150));
    ResolveTimestampAt(first_graphics, 0, Timestamp(100, 200));
    ResolveTimestampAt(second_graphics, 0, Timestamp(210, 240));
    ResolveTimestampAt(compute_submit, 0, Timestamp(1000, 1020, 2.0));
    Expect(
        capture.DrainReadyFrames() == 1,
        "completed first frame did not drain"
    );

    {
        RenderProfileFrameToken raii_frame = capture.BeginFrame();
        Expect(
            raii_frame.Valid() && raii_frame.FrameId() == 2,
            "RAII frame admission failed"
        );
    }
    Expect(
        capture.DrainReadyFrames() == 1,
        "FrameToken destructor did not seal an empty frame"
    );

    RenderProfileFrameToken error_frame = capture.BeginFrame();
    Expect(
        error_frame.Valid() && error_frame.FrameId() == 3,
        "error frame admission failed"
    );
    CommandList error_list(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            error_frame,
            error_list,
            Binding(EQueueType::Graphics, 3, 7),
            0
        ) == RenderProfileBindResult::Bound,
        "error source bind failed"
    );
    error_list.PushScopeWithTimeScope("ErrorScope");
    error_list.PopScopeWithTimeScope();
    CmdSubmit error_submit = error_list.Submit();
    Expect(capture.Seal(error_frame), "error frame seal failed");
    Expect(
        QueryBackendAccess::ResolveErrorIfPending(
            error_submit.query_tokens.front(),
            "injected GPU completion failure"
        ),
        "error query did not reach its terminal state"
    );
    Expect(
        capture.DrainReadyFrames() == 1,
        "error frame did not drain"
    );

    capture.ShutdownAfterRhiDrain();
    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        stats.closed && !stats.accepting &&
            stats.frames_attempted == 3 &&
            stats.frames_admitted == 3 &&
            stats.frames_sealed == 3 &&
            stats.frames_emitted == 3 &&
            stats.frames_invalid == 1 &&
            stats.sources_bound == 4 &&
            stats.scopes_emitted == 5 &&
            stats.shutdown_abandoned_frames == 0 &&
            stats.terminal_faults == 0,
        "GPU capture bridge stats are inconsistent"
    );

    ReleaseSubmit(first_graphics);
    ReleaseSubmit(second_graphics);
    ReleaseSubmit(compute_submit);
    ReleaseSubmit(error_submit);
    FinishRuntime(output);

    const ParsedDump dump = ParseDump(output.path);
    const SchemaDescriptor& frame_schema =
        FindSchema(dump, "GpuFrame");
    const SchemaDescriptor& scope_schema =
        FindSchema(dump, "GpuScope");
    Expect(
        frame_schema.schema_version == 1 &&
            frame_schema.fields.size() == 7,
        "GpuFrame schema contract changed"
    );
    Expect(
        scope_schema.schema_version == 2 &&
            scope_schema.fields.size() == 18,
        "GpuScope v2 schema contract changed"
    );

    const auto frame_records = RecordsFor(dump, frame_schema);
    const auto scope_records = RecordsFor(dump, scope_schema);
    Expect(
        frame_records.size() == 3 && scope_records.size() == 5,
        "decoded GPU record counts are incorrect"
    );
    Expect(
        ValueAt<std::uint64_t>(*frame_records[0], 0) == 1 &&
            ValueAt<std::uint32_t>(*frame_records[0], 1) ==
                static_cast<std::uint32_t>(
                    GpuFrameCaptureStatus::Complete
                ) &&
            ValueAt<bool>(*frame_records[0], 2) &&
            ValueAt<std::uint64_t>(*frame_records[0], 3) == 4 &&
            StringAt(*frame_records[0], 6).empty(),
        "first GpuFrame payload is incorrect"
    );
    Expect(
        ValueAt<std::uint64_t>(*frame_records[2], 0) == 3 &&
            ValueAt<std::uint32_t>(*frame_records[2], 1) ==
                static_cast<std::uint32_t>(
                    GpuFrameCaptureStatus::Invalid
                ) &&
            !ValueAt<bool>(*frame_records[2], 2) &&
            ValueAt<std::uint64_t>(*frame_records[2], 5) == 1,
        "error GpuFrame payload is incorrect"
    );

    const std::array<std::string_view, 5> expected_names{
        "Outer", "Child", "Tail", "Compute", "ErrorScope"
    };
    for (std::size_t index = 0;
         index < expected_names.size();
         ++index) {
        Expect(
            StringAt(*scope_records[index], 8) ==
                expected_names[index],
            "GpuScope preorder/name is incorrect"
        );
    }
    Expect(
        ValueAt<std::uint64_t>(*scope_records[0], 0) == 1 &&
            ValueAt<std::uint64_t>(*scope_records[0], 3) == 7 &&
            ValueAt<std::uint32_t>(*scope_records[0], 5) ==
                static_cast<std::uint32_t>(EQueueType::Graphics) &&
            ValueAt<std::uint32_t>(*scope_records[0], 6) == 3 &&
            ValueAt<std::uint32_t>(*scope_records[0], 7) == 7 &&
            ValueAt<std::uint32_t>(*scope_records[0], 16) == 0,
        "outer scope topology/domain payload is incorrect"
    );
    Expect(
        ValueAt<std::uint64_t>(*scope_records[1], 2) ==
                ValueAt<std::uint64_t>(*scope_records[0], 1) &&
            ValueAt<std::uint32_t>(*scope_records[1], 16) == 1,
        "child parent/depth payload is incorrect"
    );
    ExpectNear(
        ValueAt<double>(*scope_records[0], 14),
        100.0,
        "outer total duration is incorrect"
    );
    ExpectNear(
        ValueAt<double>(*scope_records[0], 15),
        70.0,
        "outer exclusive duration is incorrect"
    );
    Expect(
        ValueAt<std::uint64_t>(*scope_records[3], 3) == 9 &&
            ValueAt<std::uint32_t>(*scope_records[3], 5) ==
                static_cast<std::uint32_t>(EQueueType::Compute) &&
            ValueAt<std::uint32_t>(*scope_records[3], 6) == 4 &&
            ValueAt<std::uint32_t>(*scope_records[3], 7) == 9,
        "Compute queue domain/source order is incorrect"
    );
    ExpectNear(
        ValueAt<double>(*scope_records[3], 13),
        2.0,
        "Compute tick period was not preserved"
    );
    ExpectNear(
        ValueAt<double>(*scope_records[3], 14),
        40.0,
        "Compute duration was not preserved"
    );
    Expect(
        ValueAt<std::uint32_t>(*scope_records[4], 9) ==
                static_cast<std::uint32_t>(
                    GpuScopeTerminalStatus::Error
                ) &&
            StringAt(*scope_records[4], 17) ==
                "injected GPU completion failure",
        "error scope diagnostic payload is incorrect"
    );
}

void RaytracingCaptureAggregationWaitsForReverseCompletionAndRecovers() {
    ScopedOutput output("moer-render-profile-raytracing-aggregation");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture capture(schemas.frame, schemas.scope);
    const RHIQueueBinding graphics_binding =
        Binding(EQueueType::Graphics, 5, 11);

    RenderProfileFrameToken ordered_frame = capture.BeginFrame();
    Expect(
        ordered_frame.Valid() && ordered_frame.FrameId() == 1,
        "raytracing ordered frame admission failed"
    );

    CommandList persistent(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            ordered_frame, persistent, graphics_binding, 10
        ) == RenderProfileBindResult::Bound,
        "raytracing linear/graph generation bind failed"
    );
    persistent.PushScopeWithTimeScope("RT.LinearOrGraph");
    persistent.PopScopeWithTimeScope();
    // Production records the export prefix into the already-bound linear
    // generation, then rotates for the independent readback and frame tail.
    persistent.PushScopeWithTimeScope("RT.ExportPrefix");
    persistent.PopScopeWithTimeScope();
    CmdSubmit render_prefix = persistent.Submit();

    CommandList readback(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            ordered_frame, readback, graphics_binding, 20
        ) == RenderProfileBindResult::Bound,
        "raytracing readback generation bind failed"
    );
    readback.PushScopeWithTimeScope("RT.ExportReadback");
    readback.PopScopeWithTimeScope();
    CmdSubmit export_readback = readback.Submit();

    Expect(
        capture.BindSource(
            ordered_frame, persistent, graphics_binding, 30
        ) == RenderProfileBindResult::Bound,
        "raytracing frame tail generation bind failed"
    );
    persistent.PushScopeWithTimeScope("RT.FrameTail");
    persistent.PopScopeWithTimeScope();
    CmdSubmit frame_tail = persistent.Submit().TickProfiling();
    Expect(
        frame_tail.ProfilingPhase() ==
                ERHIProfilingPhase::Complete &&
            frame_tail.query_tokens.size() == 1,
        "raytracing modern tail did not retain its legacy Complete boundary"
    );

    Expect(
        capture.Seal(ordered_frame),
        "raytracing ordered frame seal failed"
    );
    ResolveTimestampAt(frame_tail, 0, Timestamp(300, 310));
    Expect(
        capture.DrainReadyFrames() == 0,
        "raytracing frame drained after only tail completion"
    );
    ResolveTimestampAt(export_readback, 0, Timestamp(200, 210));
    Expect(
        capture.DrainReadyFrames() == 0,
        "raytracing frame drained before render-prefix queries completed"
    );
    ResolveTimestampAt(render_prefix, 1, Timestamp(110, 120));
    Expect(
        capture.DrainReadyFrames() == 0,
        "raytracing frame drained before its linear query completed"
    );
    ResolveTimestampAt(render_prefix, 0, Timestamp(100, 110));
    Expect(
        capture.DrainReadyFrames() == 1,
        "reverse-completed raytracing frame did not drain"
    );

    RenderProfileFrameToken invalid_frame = capture.BeginFrame();
    Expect(
        invalid_frame.Valid() && invalid_frame.FrameId() == 2,
        "raytracing invalid frame admission failed"
    );

    Expect(
        capture.BindSource(
            invalid_frame, persistent, graphics_binding, 10
        ) == RenderProfileBindResult::Bound,
        "invalid raytracing linear generation bind failed"
    );
    persistent.PushScopeWithTimeScope("RT.Invalid.LinearOrGraph");
    persistent.PopScopeWithTimeScope();
    persistent.PushScopeWithTimeScope("RT.Invalid.ExportPrefix");
    persistent.PopScopeWithTimeScope();
    CmdSubmit invalid_render_prefix = persistent.Submit();

    CommandList rejected_readback(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            invalid_frame, rejected_readback, graphics_binding, 20
        ) == RenderProfileBindResult::Bound,
        "invalid raytracing readback bind failed"
    );
    rejected_readback.PushScopeWithTimeScope(
        "RT.Invalid.ExportReadback"
    );
    rejected_readback.PopScopeWithTimeScope();
    CmdSubmit invalid_readback = rejected_readback.Submit();

    Expect(
        capture.BindSource(
            invalid_frame, persistent, graphics_binding, 30
        ) == RenderProfileBindResult::Bound,
        "invalid raytracing tail bind failed"
    );
    persistent.PushScopeWithTimeScope("RT.Invalid.FrameTail");
    persistent.PopScopeWithTimeScope();
    CmdSubmit invalid_tail = persistent.Submit();

    Expect(
        capture.Seal(invalid_frame),
        "invalid raytracing frame seal failed"
    );
    invalid_readback.RejectPendingQueries(
        "raytracing export readback submission rejected"
    );
    Expect(
        QueryBackendAccess::ResolveErrorIfPending(
            invalid_tail.query_tokens.front(),
            "raytracing frame tail query failed"
        ),
        "raytracing tail query error was not published"
    );
    Expect(
        capture.DrainReadyFrames() == 0,
        "invalid raytracing frame drained before healthy sources completed"
    );
    ResolveTimestampAt(
        invalid_render_prefix, 1, Timestamp(120, 130)
    );
    ResolveTimestampAt(
        invalid_render_prefix, 0, Timestamp(110, 120)
    );
    Expect(
        capture.DrainReadyFrames() == 1,
        "invalid raytracing frame did not drain after terminal publication"
    );

    RenderProfileFrameToken recovered_frame = capture.BeginFrame();
    Expect(
        recovered_frame.Valid() && recovered_frame.FrameId() == 3,
        "capture did not admit the frame after raytracing rejection"
    );
    Expect(
        capture.BindSource(
            recovered_frame, persistent, graphics_binding, 50
        ) == RenderProfileBindResult::Bound,
        "recovered raytracing tail bind failed"
    );
    persistent.PushScopeWithTimeScope("RT.Recovered.FrameTail");
    persistent.PopScopeWithTimeScope();
    CmdSubmit recovered_tail = persistent.Submit();
    Expect(
        capture.Seal(recovered_frame),
        "recovered raytracing frame seal failed"
    );
    ResolveTimestampAt(recovered_tail, 0, Timestamp(500, 520));
    Expect(
        capture.DrainReadyFrames() == 1,
        "recovered raytracing frame did not drain"
    );

    capture.ShutdownAfterRhiDrain();
    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        stats.closed && !stats.accepting &&
            stats.frames_attempted == 3 &&
            stats.frames_admitted == 3 &&
            stats.frames_sealed == 3 &&
            stats.frames_emitted == 3 &&
            stats.frames_invalid == 1 &&
            stats.sources_bound == 7 &&
            stats.scopes_emitted == 9 &&
            stats.shutdown_abandoned_frames == 0 &&
            stats.terminal_faults == 0,
        "raytracing capture aggregation stats are inconsistent"
    );

    ReleaseSubmit(render_prefix);
    ReleaseSubmit(export_readback);
    ReleaseSubmit(frame_tail);
    ReleaseSubmit(invalid_render_prefix);
    ReleaseSubmit(invalid_readback);
    ReleaseSubmit(invalid_tail);
    ReleaseSubmit(recovered_tail);
    FinishRuntime(output);

    const ParsedDump dump = ParseDump(output.path);
    const auto frames = RecordsFor(
        dump, FindSchema(dump, "GpuFrame")
    );
    const auto scopes = RecordsFor(
        dump, FindSchema(dump, "GpuScope")
    );
    Expect(
        frames.size() == 3 && scopes.size() == 9,
        "raytracing capture aggregation record counts are incorrect"
    );
    Expect(
        ValueAt<std::uint64_t>(*frames[0], 0) == 1 &&
            ValueAt<std::uint32_t>(*frames[0], 1) ==
                static_cast<std::uint32_t>(
                    GpuFrameCaptureStatus::Complete
                ) &&
            ValueAt<bool>(*frames[0], 2) &&
            ValueAt<std::uint64_t>(*frames[0], 3) == 4,
        "ordered raytracing frame payload is incorrect"
    );
    Expect(
        ValueAt<std::uint64_t>(*frames[1], 0) == 2 &&
            ValueAt<std::uint32_t>(*frames[1], 1) ==
                static_cast<std::uint32_t>(
                    GpuFrameCaptureStatus::Invalid
                ) &&
            !ValueAt<bool>(*frames[1], 2) &&
            ValueAt<std::uint64_t>(*frames[1], 3) == 4 &&
            ValueAt<std::uint64_t>(*frames[1], 5) == 2,
        "rejected raytracing frame payload is incorrect"
    );
    Expect(
        ValueAt<std::uint64_t>(*frames[2], 0) == 3 &&
            ValueAt<std::uint32_t>(*frames[2], 1) ==
                static_cast<std::uint32_t>(
                    GpuFrameCaptureStatus::Complete
                ) &&
            ValueAt<bool>(*frames[2], 2),
        "raytracing capture did not recover on the next frame"
    );

    const std::array<std::string_view, 9> expected_names{
        "RT.LinearOrGraph",
        "RT.ExportPrefix",
        "RT.ExportReadback",
        "RT.FrameTail",
        "RT.Invalid.LinearOrGraph",
        "RT.Invalid.ExportPrefix",
        "RT.Invalid.ExportReadback",
        "RT.Invalid.FrameTail",
        "RT.Recovered.FrameTail",
    };
    const std::array<std::uint64_t, 9> expected_source_orders{
        10, 10, 20, 30, 10, 10, 20, 30, 50
    };
    const std::array<std::uint64_t, 9> expected_local_orders{
        0, 1, 0, 0, 0, 1, 0, 0, 0
    };
    for (std::size_t index = 0;
         index < expected_names.size();
         ++index) {
        Expect(
            StringAt(*scopes[index], 8) == expected_names[index] &&
                ValueAt<std::uint64_t>(*scopes[index], 3) ==
                    expected_source_orders[index] &&
                ValueAt<std::uint64_t>(*scopes[index], 4) ==
                    expected_local_orders[index] &&
                ValueAt<std::uint32_t>(*scopes[index], 5) ==
                    static_cast<std::uint32_t>(
                        EQueueType::Graphics
                    ) &&
                ValueAt<std::uint32_t>(*scopes[index], 6) == 5 &&
                ValueAt<std::uint32_t>(*scopes[index], 7) == 11,
            "raytracing scope generation order or queue domain is incorrect"
        );
    }
    Expect(
        ValueAt<std::uint32_t>(*scopes[6], 9) ==
                static_cast<std::uint32_t>(
                    GpuScopeTerminalStatus::Error
                ) &&
            StringAt(*scopes[6], 17) ==
                "raytracing export readback submission rejected",
        "raytracing readback rejection diagnostic is incorrect"
    );
    Expect(
        ValueAt<std::uint32_t>(*scopes[7], 9) ==
                static_cast<std::uint32_t>(
                    GpuScopeTerminalStatus::Error
                ) &&
            StringAt(*scopes[7], 17) ==
                "raytracing frame tail query failed",
        "raytracing tail error diagnostic is incorrect"
    );
}

void AbortDetachesPendingCompletionWithoutBlocking() {
    ScopedOutput output("moer-render-profile-abort");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture capture(schemas.frame, schemas.scope);

    RenderProfileFrameToken frame = capture.BeginFrame();
    CommandList list(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            frame,
            list,
            Binding(EQueueType::Graphics, 0, 0),
            0
        ) == RenderProfileBindResult::Bound,
        "abort source bind failed"
    );
    list.PushScopeWithTimeScope("PendingAtAbort");
    list.PopScopeWithTimeScope();
    CmdSubmit submit = list.Submit();
    Expect(capture.Seal(frame), "abort frame seal failed");

    capture.Abort();
    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        stats.closed && !stats.accepting &&
            stats.shutdown_abandoned_frames == 1 &&
            stats.stream.resident_frames == 0,
        "Abort did not detach its pending frame"
    );
    Expect(
        QueryBackendAccess::ResolveTimestamp(
            submit.query_tokens.front(), Timestamp(1, 2)
        ),
        "outliving Completion ticket was not safe after Abort"
    );
    ReleaseSubmit(submit);
    FinishRuntime(output);
}

void ShutdownAfterRhiDrainPerformsFinalDrain() {
    ScopedOutput output("moer-render-profile-final-drain");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture capture(schemas.frame, schemas.scope);

    RenderProfileFrameToken frame = capture.BeginFrame();
    CommandList list(EQueueType::Graphics);
    Expect(
        capture.BindSource(
            frame,
            list,
            Binding(EQueueType::Graphics, 2, 5),
            11
        ) == RenderProfileBindResult::Bound,
        "final-drain source bind failed"
    );
    list.PushScopeWithTimeScope("FinalDrain.Scope");
    list.PopScopeWithTimeScope();
    CmdSubmit submit = list.Submit();
    Expect(capture.Seal(frame), "final-drain frame seal failed");
    ResolveTimestampAt(submit, 0, Timestamp(20, 30));

    // Engine relies on this terminal path after RHI Completion joins; no
    // ordinary per-frame drain is performed first.
    capture.ShutdownAfterRhiDrain();
    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        stats.closed && !stats.accepting &&
            stats.frames_emitted == 1 &&
            stats.scopes_emitted == 1 &&
            stats.shutdown_abandoned_frames == 0 &&
            stats.terminal_faults == 0,
        "ShutdownAfterRhiDrain did not perform a clean final drain"
    );
    ReleaseSubmit(submit);
    FinishRuntime(output);

    const ParsedDump dump = ParseDump(output.path);
    Expect(
        RecordsFor(dump, FindSchema(dump, "GpuFrame")).size() == 1 &&
            RecordsFor(dump, FindSchema(dump, "GpuScope")).size() == 1,
        "final drain did not persist its frame and scope records"
    );
}

void StaleGenerationFailsClosed() {
    ScopedOutput first_output("moer-render-profile-stale-a");
    StartRuntime(first_output);
    const RegisteredGpuSchemas old_schemas = RegisterGpuSchemas();
    RenderProfileCapture old_capture(
        old_schemas.frame, old_schemas.scope
    );
    Expect(old_capture.Valid(), "old capture was not valid");
    FinishRuntime(first_output);

    ScopedOutput second_output("moer-render-profile-stale-b");
    StartRuntime(second_output);
    Expect(
        !old_capture.Valid() &&
            !old_capture.BeginFrame().Valid(),
        "old capture crossed a ProfileDump generation"
    );
    RenderProfileCapture stale_handles(
        old_schemas.frame, old_schemas.scope
    );
    Expect(
        !stale_handles.Valid(),
        "stale schema handles constructed a live bridge"
    );

    const RegisteredGpuSchemas new_schemas = RegisterGpuSchemas();
    RenderProfileCapture new_capture(
        new_schemas.frame, new_schemas.scope
    );
    Expect(
        new_capture.Valid(),
        "current-generation schema handles were rejected"
    );
    new_capture.Abort();
    old_capture.Abort();
    FinishRuntime(second_output);
}

void QueueFullDropsRecordsWithoutDisablingCapture() {
    Testing::ClearHooks();
    Expect(Testing::ConfigureWriterPauseAfterStart(true), "writer pause hook could not be configured");
    WriterResumeGuard resume_guard;

    ScopedOutput  output("moer-render-profile-queue-full");
    RuntimeConfig config{};
    config.max_record_bytes    = 4096;
    config.tls_publish_records = 1;
    config.tls_publish_bytes   = 4096;
    config.tls_max_records     = 1;
    config.tls_max_bytes       = 4096;
    config.queue_max_chunks    = 1;
    config.queue_max_records   = 1;
    config.queue_max_bytes     = 4096;
    StartRuntime(output, config);
    Expect(Testing::WaitForWriterPaused(2000), "writer did not reach the deterministic pause");

    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture       capture(schemas.frame, schemas.scope);
    RenderProfileFrameToken    frame = capture.BeginFrame();
    CommandList                list(EQueueType::Graphics);
    Expect(
        capture.BindSource(frame, list, Binding(EQueueType::Graphics, 0, 0), 0) ==
            RenderProfileBindResult::Bound,
        "queue-full source bind failed"
    );
    list.PushScopeWithTimeScope("QueueFull.Scope");
    list.PopScopeWithTimeScope();
    CmdSubmit submit = list.Submit();
    Expect(capture.Seal(frame), "queue-full frame seal failed");
    ResolveTimestampAt(submit, 0, Timestamp(1, 2));
    Expect(capture.DrainReadyFrames() == 1, "queue-full frame did not materialize");
    RenderProfileCaptureStats pressured = capture.GetStats();
    Expect(
        capture.Valid() && pressured.frames_emitted == 1 && pressured.scope_records_dropped == 1 &&
            pressured.terminal_faults == 0,
        "QueueFull disabled capture or changed drop accounting"
    );

    resume_guard.Resume();
    const FlushResult drained = FlushAll();
    Expect(
        drained == FlushResult::Completed || drained == FlushResult::NothingPending,
        "queue-full recovery did not drain accepted work"
    );
    {
        RenderProfileFrameToken recovered = capture.BeginFrame();
        Expect(recovered.Valid(), "capture did not recover after QueueFull");
    }
    Expect(capture.DrainReadyFrames() == 1, "recovered empty frame did not drain");
    const RenderProfileCaptureStats recovered = capture.GetStats();
    Expect(
        capture.Valid() && recovered.frames_emitted == 2 && recovered.terminal_faults == 0,
        "capture did not remain active after QueueFull recovery"
    );

    capture.ShutdownAfterRhiDrain();
    Expect(
        capture.TryFinishSession() == RenderProfileSessionFinishResult::ClosedWithLoss,
        "QueueFull session did not expose its lossy terminal state"
    );
    ReleaseSubmit(submit);
    FinishRuntime(output);
    Testing::ClearHooks();
}

void WriterFaultFailsCaptureClosed() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureFault(Testing::FaultPoint::WritePacket, 2),
        "writer fault hook could not be configured"
    );

    ScopedOutput  output("moer-render-profile-writer-fault");
    RuntimeConfig config{};
    config.tls_publish_records = 1;
    StartRuntime(output, config);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    RenderProfileCapture       capture(schemas.frame, schemas.scope);

    {
        RenderProfileFrameToken frame = capture.BeginFrame();
        Expect(frame.Valid(), "fault trigger frame admission failed");
    }
    static_cast<void>(capture.DrainReadyFrames());
    Expect(FlushAll() == FlushResult::Faulted, "injected writer fault did not become observable");
    Expect(
        !capture.BeginFrame().Valid() && !capture.Valid(),
        "faulted ProfileDump runtime did not close the GPU bridge"
    );
    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        stats.closed && stats.terminal_faults == 1, "writer fault did not latch one terminal bridge fault"
    );
    Expect(
        capture.TryFinishSession() == RenderProfileSessionFinishResult::Faulted,
        "writer fault did not expose the faulted terminal state"
    );
    capture.Abort();
    Expect(Shutdown() == ShutdownResult::Faulted, "faulted ProfileDump shutdown reported success");
    Testing::ClearHooks();
}

void AdmissionRejectDoesNotOvertakeOrderedFrames() {
    ScopedOutput output("moer-render-profile-admission-order");
    StartRuntime(output);
    const RegisteredGpuSchemas schemas = RegisterGpuSchemas();
    GpuScopeStreamConfig       stream_config{};
    stream_config.max_resident_frames = 2;
    stream_config.max_pending_frames  = 1;
    RenderProfileCapture capture(schemas.frame, schemas.scope, stream_config);

    RenderProfileFrameToken first = capture.BeginFrame();
    Expect(first.Valid() && first.FrameId() == 1, "ordered admission first frame failed");
    RenderProfileFrameToken rejected = capture.BeginFrame();
    Expect(!rejected.Valid() && rejected.FrameId() == 2, "pending-frame limit did not reject frame 2");
    Expect(
        capture.Seal(first) && capture.DrainReadyFrames() == 1, "ordered admission first frame did not drain"
    );

    {
        RenderProfileFrameToken recovered = capture.BeginFrame();
        Expect(
            recovered.Valid() && recovered.FrameId() == 3, "capture did not recover after admission rejection"
        );
    }
    Expect(capture.DrainReadyFrames() == 1, "ordered admission recovered frame did not drain");
    Expect(
        capture.RequestStop() == RenderProfileSessionStopResult::StopRequested &&
            capture.TryFinishSession() == RenderProfileSessionFinishResult::ClosedWithLoss,
        "frame admission rejection did not close through the dynamic lossy finish path"
    );
    const RenderProfileCaptureStats stats = capture.GetStats();
    Expect(
        stats.frames_attempted == 3 && stats.frames_admitted == 2 && stats.frames_admission_rejected == 1 &&
            stats.frames_emitted == 2 && stats.shutdown_abandoned_frames == 0,
        "admission rejection accounting is incorrect"
    );
    FinishRuntime(output);

    const ParsedDump dump   = ParseDump(output.path);
    const auto       frames = RecordsFor(dump, FindSchema(dump, "GpuFrame"));
    Expect(
        frames.size() == 2 && ValueAt<std::uint64_t>(*frames[0], 0) == 1 &&
            ValueAt<std::uint64_t>(*frames[1], 0) == 3,
        "rejected frame overtook or polluted ordered frame output"
    );
}

} // namespace

int main() {
    try {
        DefaultSessionFacadeIsInactive();
        LegacyConstructorRejectsInvalidStreamConfiguration();
        StartValidationExceptionsReturnStableResults();
        FinishSessionOwnsStopAndTokenDestructorSeal();
        SessionGateStopsAdmissionDrainsAndRestartsNextGeneration();
        AbortedPendingQueryCannotPolluteNextRuntimeGeneration();
        DelayedStaleStartCannotFaultNewGeneration();
        DelayedStartCannotPublishStaleCandidate();
        ConcurrentSessionStartHasOneWinner();
        StaleSchemaHandlesCannotReplaceCurrentSession();
        SourceAndScopeDropsCloseWithLoss();
        HappyPathPreservesTopologyAndRawTimestampDomains();
        RaytracingCaptureAggregationWaitsForReverseCompletionAndRecovers();
        AbortDetachesPendingCompletionWithoutBlocking();
        ShutdownAfterRhiDrainPerformsFinalDrain();
        StaleGenerationFailsClosed();
        QueueFullDropsRecordsWithoutDisablingCapture();
        WriterFaultFailsCaptureClosed();
        AdmissionRejectDoesNotOvertakeOrderedFrames();
    } catch (const std::exception& error) {
        std::cerr << "RenderProfileCaptureContract: " << error.what() << '\n';
        return 1;
    }
    std::cout << "RenderProfileCaptureContract: all checks passed\n";
    return 0;
}
