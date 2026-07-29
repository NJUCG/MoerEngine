#include "ProfileDumpTesting.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpCodec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace Moer::ProfileDump;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

template<typename Predicate>
bool WaitUntil(Predicate _predicate, std::uint32_t _timeout_ms = 2000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(_timeout_ms);
    while (!_predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return _predicate();
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
                throw std::runtime_error("profile dump fixture directory could not be created");
            }
        }
        throw std::runtime_error("profile dump fixture could not reserve a unique directory");
    }

    ~ScopedOutput() {
        // Assertions can unwind while the singleton runtime is active. Reap
        // its writer before removing the fixture-owned directory so cleanup
        // never unlinks a live capture.
        static_cast<void>(Shutdown());
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    [[nodiscard]] std::vector<std::filesystem::path> InProgressPaths() const {
        std::vector<std::filesystem::path>        candidates;
        std::error_code                           error;
        const std::string                         suffix = ".inprogress";
        std::filesystem::directory_iterator       iterator(directory, error);
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end) {
            const std::string filename = iterator->path().filename().string();
            if (filename.ends_with(suffix)) {
                candidates.push_back(iterator->path());
            }
            iterator.increment(error);
        }
        std::ranges::sort(candidates);
        return candidates;
    }

    [[nodiscard]] std::filesystem::path InProgressPath() const {
        const std::vector<std::filesystem::path> candidates = InProgressPaths();
        Expect(candidates.size() == 1, "expected exactly one in-progress output");
        return candidates.front();
    }

    std::filesystem::path directory;
    std::filesystem::path path;
};

std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& _path) {
    std::ifstream stream(_path, std::ios::binary | std::ios::ate);
    Expect(stream.is_open(), "profile dump output could not be opened");
    const std::streamoff size = stream.tellg();
    Expect(size >= 0, "profile dump output size is invalid");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    Expect(stream.good() || stream.eof(), "profile dump output could not be read");
    return bytes;
}

struct ParsedDump {
    std::vector<PacketType>       packet_types;
    std::vector<std::uint64_t>    packet_indices;
    std::vector<SchemaDescriptor> schemas;
    std::vector<DecodedRecord>    records;
    std::vector<LossNotice>       losses;
    std::vector<SessionBeginInfo> session_begins;
    std::vector<SessionEndInfo>   session_ends;
};

ParsedDump ParseDump(const std::filesystem::path& _path, const CodecLimits& _limits) {
    const std::vector<std::uint8_t> bytes = ReadBinaryFile(_path);
    Expect(!bytes.empty(), "profile dump output is empty");

    ParsedDump  parsed;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        PacketView         packet{};
        std::size_t        consumed = 0;
        const DecodeStatus status =
            DecodePacket(std::span<const std::uint8_t>(bytes).subspan(offset), _limits, packet, consumed);
        Expect(status == DecodeStatus::Ok, "profile dump packet failed validation");
        Expect(consumed > 0, "successful packet decode consumed no input");
        parsed.packet_types.push_back(packet.header.type);
        parsed.packet_indices.push_back(packet.header.packet_index);

        switch (packet.header.type) {
            case PacketType::SessionBegin: {
                SessionBeginInfo session{};
                Expect(
                    DecodeSessionBeginPayload(packet, session) == DecodeStatus::Ok,
                    "session begin payload failed to decode"
                );
                parsed.session_begins.push_back(session);
                break;
            }
            case PacketType::Schema: {
                SchemaDescriptor schema{};
                Expect(
                    DecodeSchemaPayload(packet, _limits, schema) == DecodeStatus::Ok,
                    "schema payload failed to decode"
                );
                parsed.schemas.push_back(std::move(schema));
                break;
            }
            case PacketType::Record: {
                Expect(!parsed.schemas.empty(), "record packet preceded every schema packet");
                DecodedRecord record{};
                bool          decoded = false;
                for (const SchemaDescriptor& schema : parsed.schemas) {
                    if (DecodeRecordPayload(packet, schema, _limits, record) == DecodeStatus::Ok) {
                        decoded = true;
                        break;
                    }
                }
                Expect(decoded, "record payload did not match any preceding schema");
                parsed.records.push_back(std::move(record));
                break;
            }
            case PacketType::Loss: {
                LossNotice loss{};
                Expect(DecodeLossPayload(packet, loss) == DecodeStatus::Ok, "loss payload failed to decode");
                parsed.losses.push_back(loss);
                break;
            }
            case PacketType::SessionEnd: {
                SessionEndInfo session{};
                Expect(
                    DecodeSessionEndPayload(packet, session) == DecodeStatus::Ok,
                    "session end payload failed to decode"
                );
                parsed.session_ends.push_back(session);
                break;
            }
        }
        offset += consumed;
    }

    Expect(offset == bytes.size(), "profile dump ended with trailing bytes");
    for (std::size_t index = 0; index < parsed.packet_indices.size(); ++index) {
        Expect(parsed.packet_indices[index] == index, "packet indices must be contiguous and start at zero");
    }
    return parsed;
}

SchemaDescriptor MakeAllTypesSchema() {
    return {
        .name           = "AllTypes",
        .event_type     = "contract.all_types",
        .kind           = EventKind::Instant,
        .channel        = Channel::CpuThread,
        .schema_version = 7,
        .fields =
            {
                {"bool", FieldType::Bool},
                {"int32", FieldType::Int32},
                {"uint32", FieldType::UInt32},
                {"int64", FieldType::Int64},
                {"uint64", FieldType::UInt64},
                {"float32", FieldType::Float32},
                {"float64", FieldType::Float64},
                {"string", FieldType::String},
            },
    };
}

SchemaDescriptor MakeRuntimeSchema() {
    return {
        .name           = "RuntimeRecord",
        .event_type     = "contract.runtime_record",
        .kind           = EventKind::Instant,
        .channel        = Channel::CpuThread,
        .schema_version = 1,
        .fields =
            {
                {"producer", FieldType::UInt64},
                {"sequence", FieldType::UInt64},
                {"label", FieldType::String},
            },
    };
}

void TestV3LittleEndianGoldenVector() {
    constexpr std::array<std::uint8_t, 16> expected_payload = {
        0x08,
        0x07,
        0x06,
        0x05,
        0x04,
        0x03,
        0x02,
        0x01,
        0x18,
        0x17,
        0x16,
        0x15,
        0x14,
        0x13,
        0x12,
        0x11,
    };
    // Fixed protocol vector. CRC32 constants were generated independently
    // from the v3 wire specification, not decoded from codec output:
    // payload_crc32 = 0xcbd946cf, header_crc32 = 0x782b1023.
    constexpr std::array<std::uint8_t, 48> expected_packet = {
        0x4d, 0x50, 0x44, 0x53, 0x03, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00,
        0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 0xcf, 0x46, 0xd9, 0xcb, 0x23, 0x10, 0x2b, 0x78,
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    };
    const SessionBeginInfo source{
        .generation      = 0x0102030405060708ull,
        .started_unix_ns = 0x1112131415161718ull,
    };

    Moer::Array<std::uint8_t> payload;
    EncodeSessionBeginPayload(source, payload);
    Expect(
        std::equal(payload.begin(), payload.end(), expected_payload.begin(), expected_payload.end()),
        "session begin payload no longer uses the v3 explicit little-endian vector"
    );

    Moer::Array<std::uint8_t> packet;
    Expect(
        WrapPacket(PacketType::SessionBegin, 0x2122232425262728ull, payload, CodecLimits{}, packet) ==
            EncodeStatus::Ok,
        "golden session packet failed to wrap"
    );
    Expect(
        std::equal(packet.begin(), packet.end(), expected_packet.begin(), expected_packet.end()),
        "wrapped v3 header or CRC no longer matches the fixed golden vector"
    );

    PacketView  decoded{};
    std::size_t consumed = 0;
    Expect(
        DecodePacket(expected_packet, CodecLimits{}, decoded, consumed) == DecodeStatus::Ok,
        "fixed v3 golden vector failed to decode"
    );
    Expect(consumed == expected_packet.size(), "golden vector consumed the wrong byte count");
    Expect(decoded.header.payload_crc32 == 0xcbd946cfu, "golden payload CRC decoded incorrectly");
    Expect(decoded.header.header_crc32 == 0x782b1023u, "golden header CRC decoded incorrectly");
    SessionBeginInfo round_trip{};
    Expect(
        DecodeSessionBeginPayload(decoded, round_trip) == DecodeStatus::Ok &&
            round_trip.generation == source.generation &&
            round_trip.started_unix_ns == source.started_unix_ns,
        "golden session payload decoded with the wrong endianness"
    );
}

void TestLossDecodeHardening() {
    const LossNotice valid{
        .first_sequence = 10,
        .last_sequence  = 12,
        .record_count   = 3,
        .value_bytes    = 99,
        .reason_mask    = static_cast<std::uint32_t>(LossReason::QueueFull),
    };
    Moer::Array<std::uint8_t> valid_payload;
    EncodeLossPayload(valid, valid_payload);
    Expect(valid_payload.size() == 36, "v3 loss payload size changed");

    auto WriteU64 = [](Moer::Array<std::uint8_t>& _payload, std::size_t _offset, std::uint64_t _value) {
        for (std::size_t byte = 0; byte < sizeof(_value); ++byte) {
            _payload[_offset + byte] = static_cast<std::uint8_t>(_value >> (byte * 8));
        }
    };
    auto WriteU32 = [](Moer::Array<std::uint8_t>& _payload, std::size_t _offset, std::uint32_t _value) {
        for (std::size_t byte = 0; byte < sizeof(_value); ++byte) {
            _payload[_offset + byte] = static_cast<std::uint8_t>(_value >> (byte * 8));
        }
    };
    auto ExpectMalformed = [&](Moer::Array<std::uint8_t> _payload, std::string_view _message) {
        const PacketView packet{
            .header =
                PacketHeader{
                    .type          = PacketType::Loss,
                    .payload_bytes = static_cast<std::uint32_t>(_payload.size()),
                },
            .payload = _payload,
        };
        LossNotice decoded{};
        Expect(DecodeLossPayload(packet, decoded) == DecodeStatus::MalformedPayload, _message);
    };

    Moer::Array<std::uint8_t> malformed = valid_payload;
    WriteU64(malformed, 16, 0);
    ExpectMalformed(std::move(malformed), "zero-count loss notice was accepted");
    malformed = valid_payload;
    WriteU64(malformed, 0, 13);
    ExpectMalformed(std::move(malformed), "reversed loss sequence interval was accepted");
    malformed = valid_payload;
    WriteU64(malformed, 8, 10);
    WriteU64(malformed, 16, 2);
    ExpectMalformed(std::move(malformed), "loss count larger than its sequence interval was accepted");
    malformed = valid_payload;
    WriteU32(malformed, 32, 0);
    ExpectMalformed(std::move(malformed), "zero-reason loss notice was accepted");
    malformed = valid_payload;
    WriteU32(malformed, 32, 0x80000000u);
    ExpectMalformed(std::move(malformed), "unknown loss reason bit was accepted");
}

void TestCodecRoundTripAndChecksums() {
    const CodecLimits      limits{};
    const SchemaDescriptor schema = MakeAllTypesSchema();

    Moer::Array<std::uint8_t> schema_payload;
    Expect(
        EncodeSchemaPayload(schema, limits, schema_payload) == EncodeStatus::Ok,
        "all-types schema failed to encode"
    );
    Moer::Array<std::uint8_t> schema_packet;
    Expect(
        WrapPacket(PacketType::Schema, 0, schema_payload, limits, schema_packet) == EncodeStatus::Ok,
        "schema packet failed to wrap"
    );

    PacketView  decoded_schema_packet{};
    std::size_t consumed = 0;
    Expect(
        DecodePacket(schema_packet, limits, decoded_schema_packet, consumed) == DecodeStatus::Ok,
        "schema packet failed to decode"
    );
    Expect(consumed == schema_packet.size(), "schema packet consumed an unexpected byte count");
    Expect(decoded_schema_packet.header.magic == kPacketMagic, "packet magic changed");
    Expect(decoded_schema_packet.header.wire_version == kWireVersion, "wire version changed");
    Expect(decoded_schema_packet.header.header_bytes == kPacketHeaderBytes, "wire header size changed");

    SchemaDescriptor decoded_schema{};
    Expect(
        DecodeSchemaPayload(decoded_schema_packet, limits, decoded_schema) == DecodeStatus::Ok,
        "schema payload failed to round-trip"
    );
    Expect(decoded_schema == schema, "schema round-trip changed a descriptor field");

    const std::array<FieldValueView, 8> values = {
        FieldValueView{true},
        FieldValueView{std::int32_t{-17}},
        FieldValueView{std::uint32_t{23}},
        FieldValueView{std::int64_t{-1234567890123}},
        FieldValueView{std::uint64_t{12345678901234}},
        FieldValueView{1.25F},
        FieldValueView{-9.5},
        FieldValueView{std::string_view("round-trip-string")},
    };
    Moer::Array<std::uint8_t> record_payload;
    const std::uint64_t       schema_hash = ComputeSchemaHash(schema);
    Expect(schema_hash != 0, "valid schema produced the reserved zero hash");
    Expect(
        EncodeRecordPayload(0, 91, schema.fields, values, limits, record_payload) ==
            EncodeStatus::InvalidSchema,
        "reserved zero schema hash was accepted"
    );
    Expect(
        EncodeRecordPayload(schema_hash, 91, schema.fields, values, limits, record_payload) ==
            EncodeStatus::Ok,
        "all-types record failed to encode"
    );
    Moer::Array<std::uint8_t> record_packet;
    Expect(
        WrapPacket(PacketType::Record, 1, record_payload, limits, record_packet) == EncodeStatus::Ok,
        "record packet failed to wrap"
    );

    PacketView decoded_record_packet{};
    consumed = 0;
    Expect(
        DecodePacket(record_packet, limits, decoded_record_packet, consumed) == DecodeStatus::Ok,
        "record packet failed to decode"
    );
    DecodedRecord record{};
    Expect(
        DecodeRecordPayload(decoded_record_packet, schema, limits, record) == DecodeStatus::Ok,
        "record payload failed to round-trip"
    );
    Expect(record.schema_hash == schema_hash, "record schema hash changed");
    Expect(record.sequence == 91, "record sequence changed");
    Expect(record.values.size() == values.size(), "record value count changed");
    Expect(std::get<bool>(record.values[0]), "bool value changed");
    Expect(std::get<std::int32_t>(record.values[1]) == -17, "int32 value changed");
    Expect(std::get<std::uint32_t>(record.values[2]) == 23, "uint32 value changed");
    Expect(std::get<std::int64_t>(record.values[3]) == -1234567890123, "int64 value changed");
    Expect(std::get<std::uint64_t>(record.values[4]) == 12345678901234, "uint64 value changed");
    Expect(std::get<float>(record.values[5]) == 1.25F, "float32 value changed");
    Expect(std::get<double>(record.values[6]) == -9.5, "float64 value changed");
    Expect(std::get<ProfileString>(record.values[7]) == "round-trip-string", "string value changed");

    for (std::size_t prefix = 0; prefix < record_packet.size(); ++prefix) {
        PacketView truncated_packet{};
        consumed                  = 99;
        const DecodeStatus status = DecodePacket(
            std::span<const std::uint8_t>(record_packet).first(prefix), limits, truncated_packet, consumed
        );
        Expect(status == DecodeStatus::NeedMoreData, "truncated packet was not retryable");
        Expect(consumed == 0, "truncated packet consumed input");
    }

    Moer::Array<std::uint8_t> damaged_header = record_packet;
    damaged_header[kPacketHeaderBytes - 1] ^= 0x80;
    PacketView damaged_packet{};
    consumed = 99;
    Expect(
        DecodePacket(damaged_header, limits, damaged_packet, consumed) == DecodeStatus::ChecksumMismatch,
        "header CRC corruption was not detected"
    );
    Expect(consumed == 0, "bad header CRC consumed input");

    Moer::Array<std::uint8_t> damaged_payload = record_packet;
    damaged_payload[kPacketHeaderBytes] ^= 0x80;
    consumed = 99;
    Expect(
        DecodePacket(damaged_payload, limits, damaged_packet, consumed) == DecodeStatus::ChecksumMismatch,
        "payload CRC corruption was not detected"
    );
    Expect(consumed == 0, "bad payload CRC consumed input");
}

void TestSchemaHashCoverage() {
    const SchemaDescriptor baseline      = MakeAllTypesSchema();
    const std::uint64_t    baseline_hash = ComputeSchemaHash(baseline);
    auto                   ExpectChanged = [&](SchemaDescriptor _changed, std::string_view _message) {
        Expect(ComputeSchemaHash(_changed) != baseline_hash, _message);
    };

    SchemaDescriptor changed = baseline;
    changed.name += ".changed";
    ExpectChanged(changed, "schema name is absent from the schema hash");
    changed = baseline;
    changed.event_type += ".changed";
    ExpectChanged(changed, "event type is absent from the schema hash");
    changed      = baseline;
    changed.kind = EventKind::Counter;
    ExpectChanged(changed, "event kind is absent from the schema hash");
    changed         = baseline;
    changed.channel = Channel::GpuQueue;
    ExpectChanged(changed, "channel is absent from the schema hash");
    changed = baseline;
    ++changed.schema_version;
    ExpectChanged(changed, "schema version is absent from the schema hash");
    changed = baseline;
    changed.fields[0].name += ".changed";
    ExpectChanged(changed, "field name is absent from the schema hash");
    changed                = baseline;
    changed.fields[0].type = FieldType::UInt64;
    ExpectChanged(changed, "field type is absent from the schema hash");
    changed = baseline;
    changed.fields.pop_back();
    ExpectChanged(changed, "field count is absent from the schema hash");
}

RuntimeConfig MakeRuntimeConfig(const ScopedOutput& _output) {
    RuntimeConfig config{};
    config.output_path         = _output.path;
    config.replace_existing    = false;
    config.tls_publish_records = 8;
    config.tls_publish_bytes   = 1024;
    config.tls_max_records     = 64;
    config.tls_max_bytes       = 64 * 1024;
    config.queue_max_chunks    = 64;
    config.queue_max_records   = 4096;
    config.queue_max_bytes     = 4 * 1024 * 1024;
    return config;
}

void ExpectFlushSucceeded(FlushResult _result, std::string_view _message) {
    Expect(_result == FlushResult::Completed || _result == FlushResult::NothingPending, _message);
}

class ResumeWriterOnExit {
public:
    ResumeWriterOnExit()                                     = default;
    ResumeWriterOnExit(const ResumeWriterOnExit&)            = delete;
    ResumeWriterOnExit& operator=(const ResumeWriterOnExit&) = delete;

    ~ResumeWriterOnExit() {
        if (active) {
            Testing::ResumeWriter();
        }
    }

    void Resume() noexcept {
        if (active) {
            Testing::ResumeWriter();
            active = false;
        }
    }

private:
    bool active{true};
};

class ResumeEmitterOnExit {
public:
    ResumeEmitterOnExit()                                      = default;
    ResumeEmitterOnExit(const ResumeEmitterOnExit&)            = delete;
    ResumeEmitterOnExit& operator=(const ResumeEmitterOnExit&) = delete;

    ~ResumeEmitterOnExit() {
        if (active) {
            Testing::ResumeEmitter();
        }
    }

    void Resume() noexcept {
        if (active) {
            Testing::ResumeEmitter();
            active = false;
        }
    }

private:
    bool active{true};
};

class FlushWaitersOnExit {
public:
    FlushWaitersOnExit(std::atomic<bool>& _release, std::vector<std::jthread>& _waiters) noexcept :
        release(_release),
        waiters(_waiters) {}

    FlushWaitersOnExit(const FlushWaitersOnExit&)            = delete;
    FlushWaitersOnExit& operator=(const FlushWaitersOnExit&) = delete;

    ~FlushWaitersOnExit() {
        Drain();
    }

    void Drain() noexcept {
        if (!active) {
            return;
        }
        release.store(true, std::memory_order_release);
        Testing::ResumeWriter();
        waiters.clear();
        active = false;
    }

private:
    std::atomic<bool>&         release;
    std::vector<std::jthread>& waiters;
    bool                       active{true};
};

void TestRuntimeLifecycleAndRestart() {
    Expect(Shutdown() == ShutdownResult::AlreadyStopped, "runtime did not begin in the stopped state");

    const SchemaDescriptor schema = MakeRuntimeSchema();
    ScopedOutput           first_output("moer-profile-runtime");
    const RuntimeConfig    first_config = MakeRuntimeConfig(first_output);
    Expect(Start(first_config) == StartResult::Started, "runtime failed to start");
    Expect(GetRuntimeState() == RuntimeState::Running, "started runtime is not running");
    Expect(
        std::filesystem::exists(first_output.InProgressPath()), "start did not create the in-progress file"
    );
    Expect(!std::filesystem::exists(first_output.path), "final output became visible before shutdown");

    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(registration.status == SchemaStatus::Registered, "runtime schema was not registered");
    Expect(static_cast<bool>(registration.handle), "registered schema returned an invalid handle");
    const SchemaRegistration duplicate = RegisterSchema(schema);
    Expect(
        duplicate.status == SchemaStatus::AlreadyRegistered,
        "duplicate schema registration was not idempotent"
    );
    Expect(duplicate.handle == registration.handle, "duplicate registration changed the handle");

    const std::array<FieldValueView, 3> values = {
        FieldValueView{std::uint64_t{3}},
        FieldValueView{std::uint64_t{17}},
        FieldValueView{std::string_view("single")},
    };
    Expect(Emit(registration.handle, values) == EmitStatus::Accepted, "runtime rejected a valid record");
    ExpectFlushSucceeded(FlushThreadLocal(), "thread-local flush failed");
    ExpectFlushSucceeded(FlushAll(), "global flush failed");
    const RuntimeStats before_shutdown = GetRuntimeStats();
    Expect(before_shutdown.generation == registration.handle.generation, "handle generation drifted");
    Expect(Shutdown() == ShutdownResult::Completed, "runtime shutdown did not complete");
    Expect(GetRuntimeState() == RuntimeState::Stopped, "shutdown runtime is not stopped");
    Expect(std::filesystem::exists(first_output.path), "shutdown did not publish final output");
    Expect(first_output.InProgressPaths().empty(), "shutdown left the in-progress file behind");

    const ParsedDump first_dump = ParseDump(first_output.path, first_config.codec_limits);
    Expect(first_dump.packet_types.front() == PacketType::SessionBegin, "session begin is not first");
    Expect(first_dump.packet_types.back() == PacketType::SessionEnd, "session end is not last");
    Expect(first_dump.schemas.size() == 1, "runtime wrote an unexpected schema count");
    Expect(first_dump.schemas.front() == schema, "runtime schema changed on disk");
    Expect(first_dump.records.size() == 1, "runtime wrote an unexpected record count");
    Expect(first_dump.session_begins.size() == 1, "runtime wrote no unique session begin");
    Expect(first_dump.session_ends.size() == 1, "runtime wrote no unique session end");
    Expect(
        first_dump.session_begins.front().generation == registration.handle.generation,
        "session begin generation disagrees with the schema handle"
    );
    Expect(
        first_dump.session_ends.front().generation == registration.handle.generation,
        "session end generation disagrees with the schema handle"
    );
    auto schema_position =
        std::find(first_dump.packet_types.begin(), first_dump.packet_types.end(), PacketType::Schema);
    auto record_position =
        std::find(first_dump.packet_types.begin(), first_dump.packet_types.end(), PacketType::Record);
    Expect(schema_position < record_position, "runtime persisted a record before its schema");

    ScopedOutput        second_output("moer-profile-restart");
    const RuntimeConfig second_config = MakeRuntimeConfig(second_output);
    Expect(Start(second_config) == StartResult::Started, "runtime failed to restart");
    const RuntimeStats restarted_stats = GetRuntimeStats();
    Expect(
        restarted_stats.generation != registration.handle.generation,
        "runtime restart reused the previous generation"
    );
    Expect(
        Emit(registration.handle, values) == EmitStatus::InvalidHandle,
        "runtime accepted a stale schema handle after restart"
    );
    const SchemaRegistration restarted_registration = RegisterSchema(schema);
    Expect(
        restarted_registration.status == SchemaStatus::Registered, "schema registration failed after restart"
    );
    Expect(
        restarted_registration.handle.generation == restarted_stats.generation,
        "restarted schema handle has the wrong generation"
    );
    Expect(
        Emit(restarted_registration.handle, values) == EmitStatus::Accepted,
        "runtime rejected a valid record after restart"
    );
    ExpectFlushSucceeded(FlushThreadLocal(), "restart thread-local flush failed");
    Expect(Shutdown() == ShutdownResult::Completed, "restarted runtime did not shut down");
    const ParsedDump second_dump = ParseDump(second_output.path, second_config.codec_limits);
    Expect(second_dump.records.size() == 1, "restart output lost or duplicated its record");
    Expect(Shutdown() == ShutdownResult::AlreadyStopped, "successful shutdown was not idempotent");
}

void TestStartAllocationFailureIsContained() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureFault(Testing::FaultPoint::StartAllocation),
        "start allocation fault hook could not be configured while stopped"
    );

    ScopedOutput        output("moer-profile-start-allocation-fault");
    const RuntimeConfig config = MakeRuntimeConfig(output);
    Expect(
        Start(config) == StartResult::ResourceExhausted,
        "start allocation failure escaped the noexcept API boundary"
    );
    Expect(GetRuntimeState() == RuntimeState::Stopped, "start allocation failure changed runtime state");
    Expect(
        !std::filesystem::exists(output.path) && output.InProgressPaths().empty(),
        "start allocation failure left an output artifact"
    );
    Expect(
        Shutdown() == ShutdownResult::AlreadyStopped, "start allocation failure broke shutdown idempotence"
    );
    Testing::ClearHooks();
}

void TestShutdownFinalizeAllocationFailureClosesWriter() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureFault(Testing::FaultPoint::ShutdownFinalizeAllocation),
        "shutdown finalize allocation fault hook could not be configured while stopped"
    );

    ScopedOutput        output("moer-profile-shutdown-allocation-fault");
    const RuntimeConfig config = MakeRuntimeConfig(output);
    Expect(Start(config) == StartResult::Started, "shutdown allocation fault runtime failed to start");
    Expect(
        Shutdown() == ShutdownResult::Faulted, "shutdown finalize allocation failure did not report a fault"
    );
    const RuntimeStats stats = GetRuntimeStats();
    Expect(stats.last_fault == RuntimeFault::WriterException, "shutdown allocation fault identity changed");
    Expect(GetRuntimeState() == RuntimeState::Stopped, "shutdown allocation fault did not reap the runtime");
    Expect(!std::filesystem::exists(output.path), "shutdown allocation fault published a partial final");

    const std::filesystem::path session_temp = output.InProgressPath();
    std::filesystem::path       moved_temp   = output.directory / "closed-writer-forensic.inprogress";
    std::error_code             move_error;
    for (std::uint32_t attempt = 0; attempt < 8; ++attempt) {
        move_error.clear();
        std::filesystem::rename(session_temp, moved_temp, move_error);
        if (!move_error) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1U << std::min<std::uint32_t>(attempt, 5U)));
    }
    Expect(!move_error, "shutdown allocation fault left the writer file handle open");
    Expect(
        Shutdown() == ShutdownResult::AlreadyStopped,
        "shutdown allocation fault did not restore shutdown idempotence"
    );

    Testing::ClearHooks();
    RuntimeConfig recovery_config    = config;
    recovery_config.replace_existing = true;
    Expect(Start(recovery_config) == StartResult::Started, "runtime did not restart after shutdown OOM");
    Expect(Shutdown() == ShutdownResult::Completed, "shutdown OOM recovery did not publish a clean capture");
    Expect(std::filesystem::exists(moved_temp), "shutdown OOM recovery removed the previous forensic temp");
}

void TestExclusiveTempCreateRace() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureWriterPauseBeforeTempOpen(true),
        "pre-open pause hook could not be configured while stopped"
    );
    ScopedOutput        output("moer-profile-exclusive-temp");
    const RuntimeConfig config       = MakeRuntimeConfig(output);
    StartResult         start_result = StartResult::Busy;
    std::jthread        starter([&] {
        start_result = Start(config);
    });
    ResumeWriterOnExit  resume_guard;
    Expect(Testing::WaitForWriterPaused(2000), "writer did not reach the deterministic pre-open pause");

    const std::array<std::uint8_t, 9> competing_bytes = {
        0x45,
        0x58,
        0x54,
        0x45,
        0x52,
        0x4e,
        0x41,
        0x4c,
        0x21,
    };
    Expect(
        Testing::CreateActiveTempCollision(competing_bytes.data(), competing_bytes.size()),
        "competing temp fixture could not be created"
    );

    resume_guard.Resume();
    starter.join();
    Expect(start_result == StartResult::FileOpenFailed, "exclusive temp collision did not fail startup");
    Expect(
        ReadBinaryFile(output.InProgressPath()) ==
            std::vector<std::uint8_t>(competing_bytes.begin(), competing_bytes.end()),
        "exclusive temp collision truncated or removed the competing file"
    );
    Expect(!std::filesystem::exists(output.path), "exclusive temp collision exposed a final output");
    Expect(
        GetRuntimeState() == RuntimeState::Stopped && Shutdown() == ShutdownResult::AlreadyStopped,
        "exclusive temp collision did not restore the stopped lifecycle"
    );
    Testing::ClearHooks();
}

void TestReplacementPreservesForeignTemp() {
    Testing::ClearHooks();
    ScopedOutput  output("moer-profile-replacement-foreign-temp");
    RuntimeConfig config    = MakeRuntimeConfig(output);
    config.replace_existing = true;

    // Recreate the exact fixed path used by the old implementation. A
    // replacement start must treat it as another session's live/forensic
    // artifact instead of unlinking it.
    std::filesystem::path foreign_temp = output.path;
    foreign_temp += ".inprogress";
    const std::array<std::uint8_t, 7> foreign_bytes = {
        0x46,
        0x4f,
        0x52,
        0x45,
        0x49,
        0x47,
        0x4e,
    };
    {
        std::ofstream stream(foreign_temp, std::ios::binary | std::ios::trunc);
        Expect(stream.is_open(), "foreign temp fixture could not be opened");
        stream.write(
            reinterpret_cast<const char*>(foreign_bytes.data()),
            static_cast<std::streamsize>(foreign_bytes.size())
        );
        Expect(stream.good(), "foreign temp fixture could not be written");
    }

    Expect(Start(config) == StartResult::Started, "replacement runtime failed to start");
    Expect(
        output.InProgressPaths().size() == 2,
        "replacement runtime did not keep its temp separate from the foreign temp"
    );
    Expect(
        ReadBinaryFile(foreign_temp) == std::vector<std::uint8_t>(foreign_bytes.begin(), foreign_bytes.end()),
        "replacement startup modified the foreign temp"
    );
    Expect(Shutdown() == ShutdownResult::Completed, "replacement runtime did not shut down cleanly");
    const std::vector<std::filesystem::path> remaining = output.InProgressPaths();
    Expect(
        remaining.size() == 1 && remaining.front() == foreign_temp,
        "replacement finalization removed or retained the wrong temp"
    );
    Expect(
        ReadBinaryFile(foreign_temp) == std::vector<std::uint8_t>(foreign_bytes.begin(), foreign_bytes.end()),
        "replacement finalization modified the foreign temp"
    );
}

void TestFinalPublicationRaces() {
    Testing::ClearHooks();
    const std::array<std::uint8_t, 8> competing_final = {
        0x45,
        0x58,
        0x54,
        0x45,
        0x52,
        0x4e,
        0x41,
        0x4c,
    };

    {
        ScopedOutput        output("moer-profile-final-no-replace-race");
        const RuntimeConfig config = MakeRuntimeConfig(output);
        Expect(Start(config) == StartResult::Started, "no-replace race runtime failed to start");
        const std::filesystem::path session_temp = output.InProgressPath();
        {
            std::ofstream stream(output.path, std::ios::binary | std::ios::trunc);
            Expect(stream.is_open(), "competing final fixture could not be opened");
            stream.write(
                reinterpret_cast<const char*>(competing_final.data()),
                static_cast<std::streamsize>(competing_final.size())
            );
            Expect(stream.good(), "competing final fixture could not be written");
        }

        Expect(
            Shutdown() == ShutdownResult::Faulted,
            "no-replace publication overwrote a final created after Start"
        );
        const RuntimeStats stats = GetRuntimeStats();
        Expect(stats.last_fault == RuntimeFault::RenameFinal, "final-name race fault identity changed");
        Expect(
            ReadBinaryFile(output.path) ==
                std::vector<std::uint8_t>(competing_final.begin(), competing_final.end()),
            "no-replace publication modified the competing final"
        );
        Expect(
            std::filesystem::exists(session_temp),
            "no-replace publication race removed the forensic session temp"
        );
        Expect(
            Shutdown() == ShutdownResult::AlreadyStopped,
            "no-replace publication race did not restore the stopped lifecycle"
        );
    }

    {
        ScopedOutput  output("moer-profile-final-replace-race");
        RuntimeConfig config    = MakeRuntimeConfig(output);
        config.replace_existing = true;
        Expect(Start(config) == StartResult::Started, "replace race runtime failed to start");
        {
            std::ofstream stream(output.path, std::ios::binary | std::ios::trunc);
            Expect(stream.is_open(), "replace race final fixture could not be opened");
            stream.write(
                reinterpret_cast<const char*>(competing_final.data()),
                static_cast<std::streamsize>(competing_final.size())
            );
            Expect(stream.good(), "replace race final fixture could not be written");
        }

        Expect(
            Shutdown() == ShutdownResult::Completed,
            "replacement publication did not atomically replace the competing final"
        );
        Expect(output.InProgressPaths().empty(), "replacement publication retained its session temp");
        const ParsedDump dump = ParseDump(output.path, config.codec_limits);
        Expect(
            dump.packet_types.front() == PacketType::SessionBegin &&
                dump.packet_types.back() == PacketType::SessionEnd,
            "replacement publication exposed an incomplete capture"
        );
    }
}

void TestMultithreadedProducers() {
    ScopedOutput  output("moer-profile-multithread");
    RuntimeConfig config       = MakeRuntimeConfig(output);
    config.tls_publish_records = 7;
    Expect(Start(config) == StartResult::Started, "multithread runtime failed to start");

    const SchemaDescriptor   schema       = MakeRuntimeSchema();
    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(registration.status == SchemaStatus::Registered, "multithread schema failed to register");

    constexpr std::uint64_t    producer_count       = 6;
    constexpr std::uint64_t    records_per_producer = 41;
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> producer_flushes{0};
    std::vector<std::jthread>  producers;
    producers.reserve(producer_count);
    for (std::uint64_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (std::uint64_t sequence = 0; sequence < records_per_producer; ++sequence) {
                const std::array<FieldValueView, 3> values = {
                    FieldValueView{producer},
                    FieldValueView{sequence},
                    FieldValueView{std::string_view("parallel")},
                };
                if (Emit(registration.handle, values) == EmitStatus::Accepted) {
                    accepted.fetch_add(1, std::memory_order_relaxed);
                } else {
                    rejected.fetch_add(1, std::memory_order_relaxed);
                }
            }
            const FlushResult flush = FlushThreadLocal();
            producer_flushes.fetch_add(1, std::memory_order_relaxed);
            if (flush != FlushResult::Completed && flush != FlushResult::NothingPending) {
                rejected.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    producers.clear();

    const std::uint64_t expected_records = producer_count * records_per_producer;
    Expect(rejected.load(std::memory_order_relaxed) == 0, "a producer operation failed");
    Expect(
        producer_flushes.load(std::memory_order_relaxed) == producer_count,
        "a producer exited without flushing its thread-local shard"
    );
    Expect(
        accepted.load(std::memory_order_relaxed) == expected_records, "producers did not accept every record"
    );
    ExpectFlushSucceeded(FlushAll(), "multithread global flush failed");
    Expect(Shutdown() == ShutdownResult::Completed, "multithread runtime did not shut down");

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.schemas.size() == 1, "multithread output duplicated its schema");
    Expect(dump.records.size() == expected_records, "multithread output lost or duplicated records");
    std::vector<bool> seen(expected_records, false);
    for (const DecodedRecord& record : dump.records) {
        Expect(record.values.size() == 3, "multithread record value count changed");
        const std::uint64_t producer = std::get<std::uint64_t>(record.values[0]);
        const std::uint64_t sequence = std::get<std::uint64_t>(record.values[1]);
        Expect(producer < producer_count, "multithread record producer is out of range");
        Expect(sequence < records_per_producer, "multithread record sequence is out of range");
        const std::size_t key = static_cast<std::size_t>(producer * records_per_producer + sequence);
        Expect(!seen[key], "multithread output duplicated a producer record");
        seen[key] = true;
    }
    for (bool was_seen : seen) {
        Expect(was_seen, "multithread output omitted a producer record");
    }

    const RuntimeStats stats = GetRuntimeStats();
    Expect(stats.state == RuntimeState::Stopped, "multithread runtime remained active");
    Expect(stats.records_written == expected_records, "runtime stats disagree with decoded record count");
    Expect(stats.records_dropped_queue_full == 0, "bounded queue unexpectedly dropped records");
    Expect(stats.records_dropped_oversized == 0, "valid records were treated as oversized");
}

void TestFlushAllHarvestsParkedLiveShard() {
    ScopedOutput  output("moer-profile-live-flush");
    RuntimeConfig config = MakeRuntimeConfig(output);
    Expect(Start(config) == StartResult::Started, "live-shard flush runtime failed to start");

    const SchemaRegistration registration = RegisterSchema(MakeRuntimeSchema());
    Expect(registration.status == SchemaStatus::Registered, "live-shard flush schema failed to register");

    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    EmitStatus        worker_status{EmitStatus::Disabled};
    std::jthread      worker([&](std::stop_token _stop) {
        const std::array<FieldValueView, 3> values = {
            FieldValueView{std::uint64_t{71}},
            FieldValueView{std::uint64_t{1}},
            FieldValueView{std::string_view("parked-flush")},
        };
        worker_status = Emit(registration.handle, values);
        ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire) && !_stop.stop_requested()) {
            std::this_thread::yield();
        }
    });

    Expect(
        WaitUntil([&] {
            return ready.load(std::memory_order_acquire);
        }),
        "live-shard flush worker did not park"
    );
    Expect(worker_status == EmitStatus::Accepted, "live-shard flush worker record was rejected");
    const RuntimeStats parked_stats = GetRuntimeStats();
    Expect(parked_stats.records_committed == 1, "parked record was not committed");
    Expect(parked_stats.records_enqueued == 0, "sub-threshold parked record published before harvest");

    ExpectFlushSucceeded(FlushAll(), "owner FlushAll did not harvest a parked live shard");
    const RuntimeStats flushed_stats = GetRuntimeStats();
    Expect(
        flushed_stats.records_enqueued == 1 && flushed_stats.records_written == 1,
        "owner FlushAll did not write the parked live record"
    );
    Expect(!release.load(std::memory_order_acquire), "live-shard worker was released before verification");

    release.store(true, std::memory_order_release);
    worker.join();
    Expect(Shutdown() == ShutdownResult::Completed, "live-shard flush runtime did not shut down");

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 1, "TLS destruction duplicated or lost the harvested live record");
    Expect(dump.losses.empty(), "live-shard flush fabricated a loss notice");
}

void TestShutdownHarvestsParkedLiveShard() {
    ScopedOutput  output("moer-profile-live-shutdown");
    RuntimeConfig config = MakeRuntimeConfig(output);
    Expect(Start(config) == StartResult::Started, "live-shard shutdown runtime failed to start");

    const SchemaRegistration registration = RegisterSchema(MakeRuntimeSchema());
    Expect(registration.status == SchemaStatus::Registered, "live-shard shutdown schema failed to register");

    std::atomic<bool> ready{false};
    std::atomic<bool> release{false};
    EmitStatus        worker_status{EmitStatus::Disabled};
    std::jthread      worker([&](std::stop_token _stop) {
        const std::array<FieldValueView, 3> values = {
            FieldValueView{std::uint64_t{72}},
            FieldValueView{std::uint64_t{1}},
            FieldValueView{std::string_view("parked-shutdown")},
        };
        worker_status = Emit(registration.handle, values);
        ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire) && !_stop.stop_requested()) {
            std::this_thread::yield();
        }
    });

    Expect(
        WaitUntil([&] {
            return ready.load(std::memory_order_acquire);
        }),
        "live-shard shutdown worker did not park"
    );
    Expect(worker_status == EmitStatus::Accepted, "live-shard shutdown worker record was rejected");
    Expect(GetRuntimeStats().records_enqueued == 0, "parked shutdown record published too early");

    Expect(
        Shutdown() == ShutdownResult::Completed,
        "owner Shutdown did not complete while the producer thread remained alive"
    );
    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 1, "owner Shutdown lost the parked live record");
    const RuntimeStats before_worker_exit = GetRuntimeStats();

    release.store(true, std::memory_order_release);
    worker.join();
    const RuntimeStats after_worker_exit = GetRuntimeStats();
    Expect(
        after_worker_exit.records_dropped_stopped == before_worker_exit.records_dropped_stopped,
        "TLS destruction re-published an already harvested stopped-generation record"
    );
}

void TestShutdownWaitsForAdmittedEmitterBeforeHarvest() {
    Testing::ClearHooks();
    ScopedOutput output("moer-profile-live-active-shutdown");
    Expect(
        Testing::ConfigureEmitterPauseAfterAdmission(true),
        "active-emitter pause hook could not be configured while stopped"
    );

    std::jthread        worker;
    std::jthread        shutdown_thread;
    ResumeEmitterOnExit resume_guard;

    RuntimeConfig config = MakeRuntimeConfig(output);
    Expect(Start(config) == StartResult::Started, "active-emitter shutdown runtime failed to start");
    const SchemaDescriptor   schema       = MakeRuntimeSchema();
    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(
        registration.status == SchemaStatus::Registered, "active-emitter shutdown schema failed to register"
    );

    EmitStatus        emit_status{EmitStatus::Disabled};
    ShutdownResult    shutdown_result{ShutdownResult::Faulted};
    std::atomic<bool> shutdown_done{false};
    worker = std::jthread([&] {
        const std::array<FieldValueView, 3> values = {
            FieldValueView{std::uint64_t{74}},
            FieldValueView{std::uint64_t{1}},
            FieldValueView{std::string_view("active-shutdown")},
        };
        emit_status = Emit(registration.handle, values);
    });
    Expect(Testing::WaitForEmitterPaused(2000), "Emit did not pause after acquiring admission");

    shutdown_thread = std::jthread([&] {
        shutdown_result = Shutdown();
        shutdown_done.store(true, std::memory_order_release);
    });
    Expect(
        WaitUntil([&] {
            return RegisterSchema(schema).status == SchemaStatus::NotRunning;
        }),
        "Shutdown did not close admission while the accepted Emit was paused"
    );
    Expect(
        !shutdown_done.load(std::memory_order_acquire),
        "Shutdown finalized before its already-admitted Emit completed"
    );

    resume_guard.Resume();
    worker.join();
    shutdown_thread.join();
    Expect(emit_status == EmitStatus::Accepted, "already-admitted Emit failed during Shutdown");
    Expect(shutdown_result == ShutdownResult::Completed, "active-emitter Shutdown did not complete");

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 1, "active-emitter Shutdown lost or duplicated its accepted record");
    Expect(dump.losses.empty(), "active-emitter Shutdown fabricated a loss notice");
    Testing::ClearHooks();
}

void TestParkedWorkerRebindsAcrossGenerations() {
    ScopedOutput  first_output("moer-profile-live-generation-a");
    ScopedOutput  second_output("moer-profile-live-generation-b");
    RuntimeConfig first_config = MakeRuntimeConfig(first_output);
    Expect(Start(first_config) == StartResult::Started, "first live generation failed to start");

    const SchemaDescriptor   schema             = MakeRuntimeSchema();
    const SchemaRegistration first_registration = RegisterSchema(schema);
    Expect(
        first_registration.status == SchemaStatus::Registered,
        "first live generation schema failed to register"
    );

    std::array<SchemaHandle, 2> handles{};
    handles[0] = first_registration.handle;
    std::array<EmitStatus, 2>  statuses{EmitStatus::Disabled, EmitStatus::Disabled};
    std::atomic<std::uint32_t> command{0};
    std::atomic<std::uint32_t> completed{0};
    std::atomic<bool>          release{false};
    std::jthread               worker([&](std::stop_token _stop) {
        for (std::uint32_t phase = 1; phase <= 2; ++phase) {
            while (command.load(std::memory_order_acquire) < phase && !_stop.stop_requested()) {
                std::this_thread::yield();
            }
            if (_stop.stop_requested()) {
                return;
            }
            const std::array<FieldValueView, 3> values = {
                FieldValueView{std::uint64_t{73}},
                FieldValueView{std::uint64_t{phase}},
                FieldValueView{std::string_view("parked-generation")},
            };
            statuses[phase - 1] = Emit(handles[phase - 1], values);
            completed.store(phase, std::memory_order_release);
        }
        while (!release.load(std::memory_order_acquire) && !_stop.stop_requested()) {
            std::this_thread::yield();
        }
    });

    command.store(1, std::memory_order_release);
    Expect(
        WaitUntil([&] {
            return completed.load(std::memory_order_acquire) >= 1;
        }),
        "parked worker did not emit in the first generation"
    );
    Expect(statuses[0] == EmitStatus::Accepted, "first parked-generation record was rejected");
    Expect(Shutdown() == ShutdownResult::Completed, "first live generation did not shut down");
    const ParsedDump first_dump = ParseDump(first_output.path, first_config.codec_limits);
    Expect(first_dump.records.size() == 1, "first live generation lost or duplicated its record");

    RuntimeConfig second_config = MakeRuntimeConfig(second_output);
    Expect(Start(second_config) == StartResult::Started, "second live generation failed to start");
    const SchemaRegistration second_registration = RegisterSchema(schema);
    Expect(
        second_registration.status == SchemaStatus::Registered,
        "second live generation schema failed to register"
    );
    Expect(
        second_registration.handle.generation != first_registration.handle.generation,
        "parked worker restart reused a generation"
    );
    handles[1] = second_registration.handle;
    command.store(2, std::memory_order_release);
    Expect(
        WaitUntil([&] {
            return completed.load(std::memory_order_acquire) >= 2;
        }),
        "parked worker did not emit in the second generation"
    );
    Expect(statuses[1] == EmitStatus::Accepted, "second parked-generation record was rejected");
    const RuntimeStats second_parked_stats = GetRuntimeStats();
    Expect(second_parked_stats.records_committed == 1, "second generation inherited committed records");
    Expect(second_parked_stats.records_enqueued == 0, "second parked record published before shutdown");
    Expect(
        second_parked_stats.records_dropped_stale_generation == 0,
        "old parked data polluted second-generation stale accounting"
    );

    Expect(Shutdown() == ShutdownResult::Completed, "second live generation did not shut down");
    const ParsedDump second_dump = ParseDump(second_output.path, second_config.codec_limits);
    Expect(second_dump.records.size() == 1, "second live generation lost or duplicated its record");
    Expect(
        std::get<std::uint64_t>(first_dump.records.front().values[1]) == 1 &&
            std::get<std::uint64_t>(second_dump.records.front().values[1]) == 2,
        "parked worker records crossed generation boundaries"
    );

    release.store(true, std::memory_order_release);
    worker.join();
}

void TestTlsDestructorRacesFlushAllExactlyOnce() {
    ScopedOutput  output("moer-profile-live-destructor-race");
    RuntimeConfig config       = MakeRuntimeConfig(output);
    config.tls_publish_records = 64;
    Expect(Start(config) == StartResult::Started, "TLS destructor race runtime failed to start");

    const SchemaRegistration registration = RegisterSchema(MakeRuntimeSchema());
    Expect(registration.status == SchemaStatus::Registered, "TLS destructor race schema failed to register");

    constexpr std::size_t    worker_count = 96;
    std::atomic<std::size_t> accepted{0};
    std::atomic<bool>        workers_joined{false};
    std::atomic<bool>        flush_failed{false};
    std::jthread             flusher([&](std::stop_token _stop) {
        while (!workers_joined.load(std::memory_order_acquire) && !_stop.stop_requested()) {
            const FlushResult result = FlushAll();
            if (result == FlushResult::Rejected || result == FlushResult::Faulted) {
                flush_failed.store(true, std::memory_order_release);
            }
            std::this_thread::yield();
        }
    });

    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back([&, index] {
            const std::array<FieldValueView, 3> values = {
                FieldValueView{static_cast<std::uint64_t>(index)},
                FieldValueView{std::uint64_t{1}},
                FieldValueView{std::string_view("destructor-race")},
            };
            if (Emit(registration.handle, values) == EmitStatus::Accepted) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    workers.clear();
    workers_joined.store(true, std::memory_order_release);
    flusher.join();

    Expect(!flush_failed.load(std::memory_order_acquire), "TLS destructor race rejected a flush interval");
    Expect(accepted.load(std::memory_order_relaxed) == worker_count, "TLS destructor race rejected a record");
    ExpectFlushSucceeded(FlushAll(), "final TLS destructor race flush failed");
    Expect(Shutdown() == ShutdownResult::Completed, "TLS destructor race runtime did not shut down");

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == worker_count, "TLS destructor race lost or duplicated records");
    std::vector<bool> seen(worker_count, false);
    for (const DecodedRecord& record : dump.records) {
        const std::uint64_t producer = std::get<std::uint64_t>(record.values[0]);
        Expect(producer < worker_count, "TLS destructor race persisted an invalid producer");
        Expect(!seen[producer], "TLS destructor race duplicated a producer record");
        seen[producer] = true;
    }
    Expect(
        std::ranges::all_of(
            seen,
            [](bool _seen) {
                return _seen;
            }
        ),
        "TLS destructor race omitted a producer record"
    );
    Expect(dump.losses.empty(), "TLS destructor race fabricated a loss notice");
}

void TestBoundedQueueDropNewest() {
    Testing::ClearHooks();
    ScopedOutput output("moer-profile-bounded");
    Expect(
        Testing::ConfigureWriterPauseAfterStart(true),
        "writer pause hook could not be configured while stopped"
    );
    ResumeWriterOnExit resume_guard;

    const SchemaDescriptor              schema = MakeRuntimeSchema();
    const std::array<FieldValueView, 3> values = {
        FieldValueView{std::uint64_t{7}},
        FieldValueView{std::uint64_t{11}},
        FieldValueView{std::string_view("bounded")},
    };
    const std::string                   oversized_label(80, 'x');
    const std::array<FieldValueView, 3> oversized_values = {
        FieldValueView{std::uint64_t{7}},
        FieldValueView{std::uint64_t{12}},
        FieldValueView{std::string_view(oversized_label)},
    };
    Moer::Array<std::uint8_t> encoded_record;
    const CodecLimits         limits{};
    Expect(
        EncodeRecordPayload(ComputeSchemaHash(schema), 1, schema.fields, values, limits, encoded_record) ==
            EncodeStatus::Ok,
        "bounded queue test could not size its record"
    );
    const std::size_t record_bytes = encoded_record.size();
    Expect(record_bytes > 0, "bounded queue test produced an empty record");

    RuntimeConfig config       = MakeRuntimeConfig(output);
    config.max_record_bytes    = record_bytes * 2;
    config.tls_publish_records = 1;
    config.tls_publish_bytes   = record_bytes * 2;
    config.tls_max_records     = 2;
    config.tls_max_bytes       = record_bytes * 2;
    config.queue_max_chunks    = 2;
    config.queue_max_records   = 2;
    config.queue_max_bytes     = record_bytes * 2;

    Expect(Start(config) == StartResult::Started, "bounded queue runtime failed to start");
    Expect(Testing::WaitForWriterPaused(2000), "writer did not reach the deterministic pause point");
    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(registration.status == SchemaStatus::Registered, "bounded queue schema failed to register");

    // The writer is paused, so completion of these calls proves producers do
    // not wait for sink progress. The first two records fill all three queue
    // budgets exactly; the limit+1 record must be rejected as the newest item.
    Expect(
        Emit(registration.handle, values) == EmitStatus::Accepted, "record below the queue limit was rejected"
    );
    Expect(
        Emit(registration.handle, values) == EmitStatus::Accepted, "record at the queue limit was rejected"
    );
    Expect(
        Emit(registration.handle, values) == EmitStatus::QueueFull,
        "limit+1 record did not use drop-newest admission"
    );
    Expect(
        Emit(registration.handle, oversized_values) == EmitStatus::RecordTooLarge,
        "oversized record did not fail before queue admission"
    );

    const RuntimeStats paused_stats = GetRuntimeStats();
    Expect(paused_stats.resident_chunks == 2, "resident chunk accounting missed queued work");
    Expect(paused_stats.resident_records == 2, "resident record accounting missed queued work");
    Expect(paused_stats.resident_bytes == record_bytes * 2, "resident byte accounting missed queued work");
    Expect(
        paused_stats.high_water_chunks == 2 && paused_stats.high_water_chunks <= config.queue_max_chunks,
        "chunk high-water exceeded its hard limit"
    );
    Expect(
        paused_stats.high_water_records == 2 && paused_stats.high_water_records <= config.queue_max_records,
        "record high-water exceeded its hard limit"
    );
    Expect(
        paused_stats.high_water_bytes == record_bytes * 2 &&
            paused_stats.high_water_bytes <= config.queue_max_bytes,
        "byte high-water exceeded its hard limit"
    );
    Expect(paused_stats.records_committed == 3, "committed record accounting changed");
    Expect(paused_stats.records_enqueued == 2, "accepted queue record accounting changed");
    Expect(paused_stats.records_dropped_queue_full == 1, "drop-newest rejection was not accounted");
    Expect(paused_stats.records_dropped_oversized == 1, "oversized rejection was not accounted");
    Expect(paused_stats.chunks_enqueued == 2, "accepted chunk accounting changed");
    Expect(paused_stats.chunks_dropped == 1, "rejected chunk accounting changed");

    resume_guard.Resume();
    // Finalize itself is the drain fence here: queued records and the fixed
    // loss slot must reach disk before SessionEnd and the final rename.
    Expect(Shutdown() == ShutdownResult::Completed, "bounded queue runtime did not shut down");
    const RuntimeStats drained_stats = GetRuntimeStats();
    Expect(
        drained_stats.resident_chunks == 0 && drained_stats.resident_records == 0 &&
            drained_stats.resident_bytes == 0,
        "resident accounting did not release drained and in-flight chunks"
    );
    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 2, "drop-newest persisted the rejected record");
    Expect(dump.losses.size() == 1, "bounded losses were not coalesced into one disk notice");
    const LossNotice& loss = dump.losses.front();
    Expect(loss.first_sequence == 3, "loss aggregate has the wrong first sequence");
    Expect(loss.last_sequence == 4, "loss aggregate has the wrong last sequence");
    Expect(loss.record_count == 2, "loss aggregate has the wrong record count");
    // Two uint64 values plus one length-prefixed string: 8 + 8 + 4 + N.
    constexpr std::uint64_t queue_full_value_bytes = 8 + 8 + 4 + 7;
    constexpr std::uint64_t oversized_value_bytes  = 8 + 8 + 4 + 80;
    Expect(
        loss.value_bytes == queue_full_value_bytes + oversized_value_bytes,
        "loss aggregate has the wrong attempted value byte count"
    );
    const std::uint32_t required_reasons =
        static_cast<std::uint32_t>(LossReason::QueueFull) | static_cast<std::uint32_t>(LossReason::Oversized);
    Expect(
        (loss.reason_mask & required_reasons) == required_reasons,
        "loss aggregate omitted queue-full or oversized reason"
    );
    Expect(
        dump.session_ends.size() == 1 && dump.session_ends.front().records_written == dump.records.size() &&
            dump.session_ends.front().records_dropped == loss.record_count,
        "SessionEnd totals disagree with persisted records and loss"
    );
    Expect(
        drained_stats.records_written == dump.records.size() &&
            drained_stats.records_dropped_queue_full + drained_stats.records_dropped_oversized ==
                loss.record_count,
        "runtime stats disagree with the persisted loss aggregate"
    );
    Testing::ClearHooks();
}

void TestMultiShardHarvestHonorsQueueLimitsAndLoss() {
    Testing::ClearHooks();
    ScopedOutput output("moer-profile-live-bounded-harvest");
    Expect(
        Testing::ConfigureWriterPauseAfterStart(true),
        "multi-shard harvest pause hook could not be configured while stopped"
    );

    std::jthread       flusher;
    ResumeWriterOnExit resume_guard;

    const SchemaDescriptor              schema = MakeRuntimeSchema();
    constexpr std::string_view          label{"multi-shard"};
    const std::array<FieldValueView, 3> sizing_values = {
        FieldValueView{std::uint64_t{1}},
        FieldValueView{std::uint64_t{1}},
        FieldValueView{label},
    };
    Moer::Array<std::uint8_t> encoded_record;
    const CodecLimits         limits{};
    Expect(
        EncodeRecordPayload(
            ComputeSchemaHash(schema), 1, schema.fields, sizing_values, limits, encoded_record
        ) == EncodeStatus::Ok,
        "multi-shard harvest could not size its record"
    );
    const std::size_t record_bytes = encoded_record.size();

    RuntimeConfig config       = MakeRuntimeConfig(output);
    config.max_record_bytes    = record_bytes * 2;
    config.tls_publish_records = 2;
    config.tls_publish_bytes   = record_bytes * 2;
    config.tls_max_records     = 2;
    config.tls_max_bytes       = record_bytes * 2;
    config.queue_max_chunks    = 2;
    config.queue_max_records   = 2;
    config.queue_max_bytes     = record_bytes * 2;
    Expect(Start(config) == StartResult::Started, "multi-shard harvest runtime failed to start");
    Expect(Testing::WaitForWriterPaused(2000), "multi-shard harvest writer did not pause");

    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(registration.status == SchemaStatus::Registered, "multi-shard harvest schema failed to register");

    constexpr std::size_t     worker_count = 4;
    std::atomic<std::size_t>  ready{0};
    std::atomic<std::size_t>  accepted{0};
    std::atomic<bool>         release{false};
    std::vector<std::jthread> workers;
    workers.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        workers.emplace_back([&, index](std::stop_token _stop) {
            const std::array<FieldValueView, 3> values = {
                FieldValueView{static_cast<std::uint64_t>(index)},
                FieldValueView{std::uint64_t{1}},
                FieldValueView{label},
            };
            if (Emit(registration.handle, values) == EmitStatus::Accepted) {
                accepted.fetch_add(1, std::memory_order_relaxed);
            }
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire) && !_stop.stop_requested()) {
                std::this_thread::yield();
            }
        });
    }
    Expect(
        WaitUntil([&] {
            return ready.load(std::memory_order_acquire) == worker_count;
        }),
        "multi-shard workers did not park"
    );
    Expect(accepted.load(std::memory_order_relaxed) == worker_count, "a multi-shard record was rejected");
    Expect(GetRuntimeStats().records_enqueued == 0, "multi-shard records published before owner harvest");

    FlushResult flush_result{FlushResult::NothingPending};
    flusher = std::jthread([&] {
        flush_result = FlushAll();
    });
    Expect(
        WaitUntil([&] {
            const RuntimeStats stats = GetRuntimeStats();
            return stats.records_enqueued + stats.records_dropped_queue_full == worker_count;
        }),
        "multi-shard harvest did not account every parked record"
    );

    const RuntimeStats paused_stats = GetRuntimeStats();
    Expect(
        paused_stats.records_committed == worker_count,
        "multi-shard harvest changed committed record accounting"
    );
    Expect(
        paused_stats.resident_chunks == 2 && paused_stats.resident_records == 2,
        "multi-shard harvest exceeded or underfilled the bounded queue"
    );
    Expect(
        paused_stats.resident_bytes == record_bytes * 2,
        "multi-shard harvest resident bytes disagree with the queue limit"
    );
    Expect(
        paused_stats.records_enqueued == 2 && paused_stats.chunks_enqueued == 2,
        "multi-shard harvest accepted the wrong number of chunks"
    );
    Expect(
        paused_stats.records_dropped_queue_full == 2 && paused_stats.chunks_dropped == 2,
        "multi-shard harvest did not drop-newest beyond the queue limit"
    );
    Expect(
        paused_stats.high_water_chunks <= config.queue_max_chunks &&
            paused_stats.high_water_records <= config.queue_max_records &&
            paused_stats.high_water_bytes <= config.queue_max_bytes,
        "multi-shard harvest exceeded a hard queue high-water limit"
    );

    release.store(true, std::memory_order_release);
    workers.clear();
    resume_guard.Resume();
    flusher.join();
    Expect(
        flush_result == FlushResult::Rejected,
        "partial multi-shard queue rejection was not surfaced after flushing accepted data"
    );
    Expect(Shutdown() == ShutdownResult::Completed, "multi-shard harvest runtime did not shut down");

    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 2, "multi-shard harvest persisted the wrong record count");
    Expect(dump.losses.size() == 1, "multi-shard queue losses were not coalesced");
    const LossNotice& loss = dump.losses.front();
    Expect(loss.record_count == 2, "multi-shard loss notice has the wrong record count");
    Expect(
        (loss.reason_mask & static_cast<std::uint32_t>(LossReason::QueueFull)) != 0,
        "multi-shard loss notice omitted QueueFull"
    );
    constexpr std::uint64_t value_bytes_per_record = 8 + 8 + 4 + label.size();
    Expect(
        loss.value_bytes == value_bytes_per_record * 2,
        "multi-shard loss notice has the wrong value byte count"
    );
    Expect(
        dump.session_ends.size() == 1 && dump.session_ends.front().records_written == 2 &&
            dump.session_ends.front().records_dropped == 2,
        "multi-shard SessionEnd totals disagree with records and Loss"
    );
    Testing::ClearHooks();
}

void TestConcurrentFlushWaiters() {
    Testing::ClearHooks();
    ScopedOutput output("moer-profile-concurrent-flush");
    Expect(
        Testing::ConfigureWriterPauseAfterStart(true),
        "flush waiter pause hook could not be configured while stopped"
    );
    ResumeWriterOnExit resume_guard;

    RuntimeConfig config       = MakeRuntimeConfig(output);
    config.tls_publish_records = 1;
    Expect(Start(config) == StartResult::Started, "flush waiter runtime failed to start");
    Expect(Testing::WaitForWriterPaused(2000), "flush waiter writer did not reach its pause point");

    const SchemaDescriptor   schema       = MakeRuntimeSchema();
    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(registration.status == SchemaStatus::Registered, "flush waiter schema failed to register");
    const std::array<FieldValueView, 3> values = {
        FieldValueView{std::uint64_t{34}},
        FieldValueView{std::uint64_t{55}},
        FieldValueView{std::string_view("flush-waiters")},
    };
    Expect(
        Emit(registration.handle, values) == EmitStatus::Accepted, "flush waiter setup record was rejected"
    );

    constexpr std::size_t                 waiter_count = 12;
    std::array<FlushResult, waiter_count> results{};
    results.fill(FlushResult::Rejected);
    std::atomic<std::size_t>  ready{0};
    std::atomic<bool>         release{false};
    std::vector<std::jthread> waiters;
    waiters.reserve(waiter_count);
    FlushWaitersOnExit waiters_guard(release, waiters);
    for (std::size_t index = 0; index < waiter_count; ++index) {
        waiters.emplace_back([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[index] = FlushAll();
        });
    }
    while (ready.load(std::memory_order_acquire) != waiter_count) {
        std::this_thread::yield();
    }
    release.store(true, std::memory_order_release);
    // Give every released waiter a scheduling turn while sink progress is
    // structurally impossible. This is not a wall-clock performance check.
    for (std::size_t turn = 0; turn < 4096; ++turn) {
        std::this_thread::yield();
    }

    waiters_guard.Drain();
    resume_guard.Resume();
    for (FlushResult result : results) {
        Expect(
            result == FlushResult::Completed || result == FlushResult::NothingPending,
            "a concurrent flush waiter did not observe the shared drain"
        );
    }
    Expect(
        GetRuntimeStats().flush_completed == 1,
        "concurrent waiters created more than one physical flush interval"
    );
    Expect(Shutdown() == ShutdownResult::Completed, "flush waiter runtime did not shut down");
    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 1, "concurrent flush lost or duplicated queued data");
    Expect(dump.losses.empty(), "concurrent flush fabricated a loss notice");
    Testing::ClearHooks();
}

void TestFaultedRestartDiscardsParkedStaleShard() {
    Testing::ClearHooks();
    ScopedOutput output("moer-profile-live-fault-restart");
    Expect(
        Testing::ConfigureFault(Testing::FaultPoint::WritePacket, 2),
        "parked fault-restart hook could not be configured"
    );

    RuntimeConfig config       = MakeRuntimeConfig(output);
    config.tls_publish_records = 2;
    Expect(Start(config) == StartResult::Started, "parked fault generation failed to start");

    const SchemaDescriptor   schema             = MakeRuntimeSchema();
    const SchemaRegistration first_registration = RegisterSchema(schema);
    Expect(
        first_registration.status == SchemaStatus::Registered,
        "parked fault generation schema failed to register"
    );

    std::array<SchemaHandle, 2> handles{};
    handles[0] = first_registration.handle;
    std::array<EmitStatus, 2>  statuses{EmitStatus::Disabled, EmitStatus::Disabled};
    std::atomic<std::uint32_t> command{0};
    std::atomic<std::uint32_t> completed{0};
    std::atomic<bool>          release{false};
    std::jthread               worker([&](std::stop_token _stop) {
        for (std::uint32_t phase = 1; phase <= 2; ++phase) {
            while (command.load(std::memory_order_acquire) < phase && !_stop.stop_requested()) {
                std::this_thread::yield();
            }
            if (_stop.stop_requested()) {
                return;
            }
            const std::array<FieldValueView, 3> values = {
                FieldValueView{std::uint64_t{88}},
                FieldValueView{std::uint64_t{phase}},
                FieldValueView{std::string_view("fault-restart-worker")},
            };
            statuses[phase - 1] = Emit(handles[phase - 1], values);
            completed.store(phase, std::memory_order_release);
        }
        while (!release.load(std::memory_order_acquire) && !_stop.stop_requested()) {
            std::this_thread::yield();
        }
    });

    command.store(1, std::memory_order_release);
    Expect(
        WaitUntil([&] {
            return completed.load(std::memory_order_acquire) >= 1;
        }),
        "worker did not park data before the writer fault"
    );
    Expect(statuses[0] == EmitStatus::Accepted, "parked pre-fault record was rejected");
    Expect(GetRuntimeStats().records_enqueued == 0, "parked pre-fault record published too early");

    const std::array<FieldValueView, 3> trigger_values = {
        FieldValueView{std::uint64_t{99}},
        FieldValueView{std::uint64_t{1}},
        FieldValueView{std::string_view("fault-trigger")},
    };
    const EmitStatus first_trigger  = Emit(first_registration.handle, trigger_values);
    const EmitStatus second_trigger = Emit(first_registration.handle, trigger_values);
    Expect(
        first_trigger == EmitStatus::Accepted,
        "first fault-trigger record failed before filling the main TLS shard"
    );
    Expect(
        second_trigger == EmitStatus::Accepted || second_trigger == EmitStatus::SinkFault,
        "second fault-trigger record returned an unexpected status"
    );
    Expect(
        WaitUntil([] {
            return GetRuntimeState() == RuntimeState::Faulted;
        }),
        "writer did not enter Faulted after the deterministic packet failure"
    );
    Expect(Shutdown() == ShutdownResult::Faulted, "faulted parked generation did not report failure");
    Expect(GetRuntimeState() == RuntimeState::Stopped, "faulted parked generation did not stop");
    const RuntimeStats faulted_stats = GetRuntimeStats();
    Expect(
        faulted_stats.records_dropped_after_fault == 3,
        "faulted Shutdown did not account the queued chunk and parked live shard"
    );
    Expect(
        faulted_stats.chunks_dropped == 2, "faulted Shutdown did not account both queued and parked chunks"
    );

    Testing::ClearHooks();
    config.replace_existing = true;
    Expect(Start(config) == StartResult::Started, "parked worker did not restart after writer fault");
    const SchemaRegistration second_registration = RegisterSchema(schema);
    Expect(
        second_registration.status == SchemaStatus::Registered, "parked restart schema failed to register"
    );
    handles[1] = second_registration.handle;
    command.store(2, std::memory_order_release);
    Expect(
        WaitUntil([&] {
            return completed.load(std::memory_order_acquire) >= 2;
        }),
        "parked worker did not emit after fault restart"
    );
    Expect(statuses[1] == EmitStatus::Accepted, "parked post-fault record was rejected");
    const RuntimeStats restarted_stats = GetRuntimeStats();
    Expect(restarted_stats.records_committed == 1, "fault restart inherited old committed records");
    Expect(restarted_stats.records_enqueued == 0, "post-fault parked record published before shutdown");
    Expect(
        restarted_stats.records_dropped_stale_generation == 0,
        "old faulted shard polluted restarted stale-generation accounting"
    );

    Expect(Shutdown() == ShutdownResult::Completed, "parked fault restart did not shut down cleanly");
    const ParsedDump dump = ParseDump(output.path, config.codec_limits);
    Expect(dump.records.size() == 1, "fault restart output retained old or duplicated new parked data");
    Expect(
        std::get<std::uint64_t>(dump.records.front().values[0]) == 88 &&
            std::get<std::uint64_t>(dump.records.front().values[1]) == 2,
        "fault restart output contains the wrong worker generation"
    );
    Expect(dump.losses.empty(), "fault restart output inherited an old-generation Loss");

    release.store(true, std::memory_order_release);
    worker.join();
    Testing::ClearHooks();
}

void RecoverAfterFault(const ScopedOutput& _output, RuntimeConfig _config, const SchemaDescriptor& _schema) {
    Testing::ClearHooks();
    _config.replace_existing = true;
    Expect(Start(_config) == StartResult::Started, "runtime did not restart after sink fault");
    const SchemaRegistration registration = RegisterSchema(_schema);
    Expect(
        registration.status == SchemaStatus::Registered, "schema did not register after sink-fault restart"
    );
    const std::array<FieldValueView, 3> values = {
        FieldValueView{std::uint64_t{1}},
        FieldValueView{std::uint64_t{2}},
        FieldValueView{std::string_view("recovered")},
    };
    Expect(
        Emit(registration.handle, values) == EmitStatus::Accepted, "producer did not recover after sink fault"
    );
    Expect(Shutdown() == ShutdownResult::Completed, "recovered runtime did not shut down");
    const ParsedDump dump = ParseDump(_output.path, _config.codec_limits);
    Expect(dump.records.size() == 1, "recovered output has the wrong record count");
    Expect(Shutdown() == ShutdownResult::AlreadyStopped, "recovered shutdown was not idempotent");
}

void TestWriterFaultReleasesFlushWaiter(
    Testing::FaultPoint _fault_point,
    std::uint64_t       _trigger_hit,
    RuntimeFault        _expected_fault,
    std::string_view    _output_stem,
    bool                _recover
) {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureFault(_fault_point, _trigger_hit),
        "writer fault hook could not be configured while stopped"
    );

    ScopedOutput  output(_output_stem);
    RuntimeConfig config          = MakeRuntimeConfig(output);
    config.tls_publish_records    = 1;
    const SchemaDescriptor schema = MakeRuntimeSchema();
    Expect(Start(config) == StartResult::Started, "fault test runtime failed to start");
    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(registration.status == SchemaStatus::Registered, "fault test schema failed to register");
    const std::array<FieldValueView, 3> values = {
        FieldValueView{std::uint64_t{5}},
        FieldValueView{std::uint64_t{8}},
        FieldValueView{std::string_view("fault")},
    };
    const EmitStatus initial_emit = Emit(registration.handle, values);
    Expect(
        initial_emit == EmitStatus::Accepted || initial_emit == EmitStatus::SinkFault,
        "fault test producer returned an unrelated status"
    );

    // This call is also the waiter contract: the writer fault must signal the
    // queued fence and return Faulted rather than leaving the caller blocked.
    Expect(FlushAll() == FlushResult::Faulted, "writer fault did not release the flush waiter as faulted");
    Expect(GetRuntimeState() == RuntimeState::Faulted, "writer fault did not latch state");
    const RuntimeStats fault_stats = GetRuntimeStats();
    Expect(fault_stats.last_fault == _expected_fault, "writer fault identity changed");
    Expect(fault_stats.io_faults == 1, "I/O fault was not counted exactly once");
    Expect(
        fault_stats.resident_chunks == 0 && fault_stats.resident_records == 0 &&
            fault_stats.resident_bytes == 0,
        "faulted writer retained admitted or in-flight capacity"
    );
    Expect(
        Emit(registration.handle, values) == EmitStatus::SinkFault,
        "noexcept producer did not fail closed after sink fault"
    );
    Expect(
        std::filesystem::exists(output.InProgressPath()), "sink fault removed forensic in-progress output"
    );
    Expect(!std::filesystem::exists(output.path), "sink fault published a partial final output");
    Expect(Shutdown() == ShutdownResult::Faulted, "faulted runtime shutdown did not report the fault");
    Expect(GetRuntimeState() == RuntimeState::Stopped, "fault shutdown was not reaped");
    Expect(Shutdown() == ShutdownResult::AlreadyStopped, "fault shutdown was not idempotent");
    if (_recover) {
        RecoverAfterFault(output, config, schema);
    } else {
        Testing::ClearHooks();
    }
}

void TestRenameFinalFault() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureFault(Testing::FaultPoint::RenameFinal),
        "rename fault hook could not be configured while stopped"
    );

    ScopedOutput  output("moer-profile-rename-fault");
    RuntimeConfig config                        = MakeRuntimeConfig(output);
    config.replace_existing                     = true;
    config.tls_publish_records                  = 1;
    const std::array<std::uint8_t, 8> old_final = {
        0x4d,
        0x4f,
        0x45,
        0x52,
        0x2d,
        0x4f,
        0x4c,
        0x44,
    };
    {
        std::ofstream stream(output.path, std::ios::binary | std::ios::trunc);
        Expect(stream.is_open(), "old final fixture could not be opened");
        stream.write(
            reinterpret_cast<const char*>(old_final.data()), static_cast<std::streamsize>(old_final.size())
        );
        Expect(stream.good(), "old final fixture could not be written");
    }
    const SchemaDescriptor schema = MakeRuntimeSchema();
    Expect(Start(config) == StartResult::Started, "rename fault runtime failed to start");
    Expect(
        ReadBinaryFile(output.path) == std::vector<std::uint8_t>(old_final.begin(), old_final.end()),
        "replacement start removed the previous valid final"
    );
    const SchemaRegistration registration = RegisterSchema(schema);
    Expect(registration.status == SchemaStatus::Registered, "rename fault schema failed to register");
    const std::array<FieldValueView, 3> values = {
        FieldValueView{std::uint64_t{13}},
        FieldValueView{std::uint64_t{21}},
        FieldValueView{std::string_view("rename")},
    };
    Expect(
        Emit(registration.handle, values) == EmitStatus::Accepted, "rename fault setup record was rejected"
    );
    Expect(Shutdown() == ShutdownResult::Faulted, "rename failure did not fail finalization");
    const RuntimeStats stats = GetRuntimeStats();
    Expect(stats.last_fault == RuntimeFault::RenameFinal, "rename fault identity changed");
    Expect(stats.io_faults == 1, "rename fault was not counted exactly once");
    Expect(
        std::filesystem::exists(output.InProgressPath()), "rename fault removed forensic in-progress output"
    );
    Expect(
        ReadBinaryFile(output.path) == std::vector<std::uint8_t>(old_final.begin(), old_final.end()),
        "rename fault destroyed the previous valid final"
    );
    Expect(Shutdown() == ShutdownResult::AlreadyStopped, "rename fault shutdown was not idempotent");
    RecoverAfterFault(output, config, schema);
}

} // namespace

int main() {
    try {
        TestV3LittleEndianGoldenVector();
        TestLossDecodeHardening();
        TestCodecRoundTripAndChecksums();
        TestSchemaHashCoverage();
        TestRuntimeLifecycleAndRestart();
        TestStartAllocationFailureIsContained();
        TestShutdownFinalizeAllocationFailureClosesWriter();
        TestExclusiveTempCreateRace();
        TestReplacementPreservesForeignTemp();
        TestFinalPublicationRaces();
        TestMultithreadedProducers();
        TestFlushAllHarvestsParkedLiveShard();
        TestShutdownHarvestsParkedLiveShard();
        TestShutdownWaitsForAdmittedEmitterBeforeHarvest();
        TestParkedWorkerRebindsAcrossGenerations();
        TestTlsDestructorRacesFlushAllExactlyOnce();
        TestBoundedQueueDropNewest();
        TestMultiShardHarvestHonorsQueueLimitsAndLoss();
        TestConcurrentFlushWaiters();
        TestFaultedRestartDiscardsParkedStaleShard();
        TestWriterFaultReleasesFlushWaiter(
            Testing::FaultPoint::WritePacket, 2, RuntimeFault::WritePacket, "moer-profile-write-fault", true
        );
        TestWriterFaultReleasesFlushWaiter(
            Testing::FaultPoint::FlushFile, 1, RuntimeFault::FlushFile, "moer-profile-flush-fault", false
        );
        TestRenameFinalFault();
        std::cout << "ProfileDump core contract tests passed." << std::endl;
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        const RuntimeState state = GetRuntimeState();
        if (state != RuntimeState::Stopped) {
            static_cast<void>(Shutdown());
        }
        std::cerr << "ProfileDump core contract test failed: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
}
