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

class ScopedOutput {
public:
    explicit ScopedOutput(std::string_view _stem) {
        static std::atomic<std::uint64_t> next_id{0};
        const auto                        now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        path = std::filesystem::temp_directory_path() /
               (std::string(_stem) + "-" + std::to_string(now) + "-" +
                std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)) + ".mpds");
    }

    ~ScopedOutput() {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(InProgressPath(), error);
    }

    [[nodiscard]] std::filesystem::path InProgressPath() const {
        return std::filesystem::path(path.string() + ".inprogress");
    }

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
    Expect(
        !std::filesystem::exists(first_output.InProgressPath()), "shutdown left the in-progress file behind"
    );

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
        !std::filesystem::exists(output.path) && !std::filesystem::exists(output.InProgressPath()),
        "start allocation failure left an output artifact"
    );
    Expect(
        Shutdown() == ShutdownResult::AlreadyStopped, "start allocation failure broke shutdown idempotence"
    );
    Testing::ClearHooks();
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
    {
        std::ofstream stream(output.InProgressPath(), std::ios::binary | std::ios::trunc);
        Expect(stream.is_open(), "competing temp fixture could not be opened");
        stream.write(
            reinterpret_cast<const char*>(competing_bytes.data()),
            static_cast<std::streamsize>(competing_bytes.size())
        );
        Expect(stream.good(), "competing temp fixture could not be written");
    }

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

void TestBoundedQueueDropNewest() {
    Testing::ClearHooks();
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

    ScopedOutput  output("moer-profile-bounded");
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

void TestConcurrentFlushWaiters() {
    Testing::ClearHooks();
    Expect(
        Testing::ConfigureWriterPauseAfterStart(true),
        "flush waiter pause hook could not be configured while stopped"
    );
    ResumeWriterOnExit resume_guard;

    ScopedOutput  output("moer-profile-concurrent-flush");
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

    resume_guard.Resume();
    waiters.clear();
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
        TestExclusiveTempCreateRace();
        TestMultithreadedProducers();
        TestBoundedQueueDropNewest();
        TestConcurrentFlushWaiters();
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
