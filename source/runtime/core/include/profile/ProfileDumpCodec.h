#ifndef MOER_ENGINE_PROFILE_DUMP_CODEC_H
#define MOER_ENGINE_PROFILE_DUMP_CODEC_H

#include "API_Macro.h"
#include "misc/STL.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace Moer::ProfileDump {

// ProfileDump values cross the Moer::Core DLL boundary. Use the engine
// allocator explicitly so an owning string allocated by the codec is released
// through Memory::Free in every consuming module.
using ProfileString = std::basic_string<char, std::char_traits<char>, MoerStlAllocator<char>>;

inline constexpr std::uint32_t kPacketMagic       = 0x5344504Du;
inline constexpr std::uint16_t kWireVersion       = 3;
inline constexpr std::uint16_t kPacketHeaderBytes = 32;

enum class Channel : std::uint8_t {
    CpuThread = 0,
    GpuQueue  = 1,
};

enum class EventKind : std::uint8_t {
    Scope   = 0,
    Counter = 1,
    Instant = 2,
};

enum class FieldType : std::uint8_t {
    Bool = 0,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    String,
};

enum class PacketType : std::uint16_t {
    SessionBegin = 1,
    Schema       = 2,
    Record       = 3,
    Loss         = 4,
    SessionEnd   = 5,
};

enum class EncodeStatus : std::uint8_t {
    Ok = 0,
    InvalidSchema,
    ValueCountMismatch,
    ValueTypeMismatch,
    StringTooLarge,
    PayloadTooLarge,
};

enum class DecodeStatus : std::uint8_t {
    Ok = 0,
    NeedMoreData,
    InvalidMagic,
    UnsupportedVersion,
    InvalidHeader,
    UnknownPacketType,
    UnsupportedFlags,
    PayloadTooLarge,
    ChecksumMismatch,
    MalformedPayload,
    LimitExceeded,
    SchemaHashMismatch,
    UnknownSchema,
};

struct CodecLimits {
    std::size_t   max_packet_payload_bytes{1024 * 1024};
    std::size_t   max_string_bytes{16 * 1024};
    std::uint32_t max_fields{64};
};

struct SchemaField {
    ProfileString name{};
    FieldType     type{FieldType::Bool};

    friend bool operator==(const SchemaField&, const SchemaField&) = default;
};

struct SchemaDescriptor {
    ProfileString      name{};
    ProfileString      event_type{};
    EventKind          kind{EventKind::Scope};
    Channel            channel{Channel::CpuThread};
    std::uint32_t      schema_version{1};
    Array<SchemaField> fields{};

    friend bool operator==(const SchemaDescriptor&, const SchemaDescriptor&) = default;
};

using FieldValue = std::
    variant<bool, std::int32_t, std::uint32_t, std::int64_t, std::uint64_t, float, double, ProfileString>;

using FieldValueView = std::
    variant<bool, std::int32_t, std::uint32_t, std::int64_t, std::uint64_t, float, double, std::string_view>;

struct PacketHeader {
    std::uint32_t magic{kPacketMagic};
    std::uint16_t wire_version{kWireVersion};
    std::uint16_t header_bytes{kPacketHeaderBytes};
    PacketType    type{PacketType::SessionBegin};
    std::uint16_t flags{0};
    std::uint32_t payload_bytes{0};
    std::uint64_t packet_index{0};
    std::uint32_t payload_crc32{0};
    std::uint32_t header_crc32{0};
};

struct PacketView {
    PacketHeader                  header{};
    std::span<const std::uint8_t> payload{};
};

struct DecodedRecord {
    std::uint64_t     schema_hash{0};
    std::uint64_t     sequence{0};
    Array<FieldValue> values{};
};

enum class LossReason : std::uint32_t {
    None            = 0,
    Oversized       = 1u << 0,
    QueueFull       = 1u << 1,
    StaleGeneration = 1u << 2,
    AfterShutdown   = 1u << 3,
    SinkFault       = 1u << 4,
};

struct LossNotice {
    std::uint64_t first_sequence{0};
    std::uint64_t last_sequence{0};
    std::uint64_t record_count{0};
    std::uint64_t value_bytes{0};
    std::uint32_t reason_mask{0};
};

struct SessionBeginInfo {
    std::uint64_t generation{0};
    std::uint64_t started_unix_ns{0};
};

struct SessionEndInfo {
    std::uint64_t generation{0};
    std::uint64_t records_written{0};
    std::uint64_t records_dropped{0};
};

[[nodiscard]] CORE_API std::uint64_t ComputeSchemaHash(const SchemaDescriptor& _schema) noexcept;

[[nodiscard]] CORE_API EncodeStatus EncodeSchemaPayload(
    const SchemaDescriptor& _schema,
    const CodecLimits&      _limits,
    Array<std::uint8_t>&    _payload
);

[[nodiscard]] CORE_API EncodeStatus EncodeRecordPayload(
    std::uint64_t                   _schema_hash,
    std::uint64_t                   _sequence,
    std::span<const SchemaField>    _fields,
    std::span<const FieldValueView> _values,
    const CodecLimits&              _limits,
    Array<std::uint8_t>&            _payload
);

CORE_API void EncodeLossPayload(const LossNotice& _loss, Array<std::uint8_t>& _payload);

CORE_API void EncodeSessionBeginPayload(const SessionBeginInfo& _session, Array<std::uint8_t>& _payload);

CORE_API void EncodeSessionEndPayload(const SessionEndInfo& _session, Array<std::uint8_t>& _payload);

[[nodiscard]] CORE_API EncodeStatus WrapPacket(
    PacketType                    _type,
    std::uint64_t                 _packet_index,
    std::span<const std::uint8_t> _payload,
    const CodecLimits&            _limits,
    Array<std::uint8_t>&          _packet
);

[[nodiscard]] CORE_API DecodeStatus DecodePacket(
    std::span<const std::uint8_t> _input,
    const CodecLimits&            _limits,
    PacketView&                   _packet,
    std::size_t&                  _consumed
) noexcept;

[[nodiscard]] CORE_API DecodeStatus
DecodeSchemaPayload(const PacketView& _packet, const CodecLimits& _limits, SchemaDescriptor& _schema);

[[nodiscard]] CORE_API DecodeStatus DecodeRecordPayload(
    const PacketView&       _packet,
    const SchemaDescriptor& _schema,
    const CodecLimits&      _limits,
    DecodedRecord&          _record
);

[[nodiscard]] CORE_API DecodeStatus DecodeLossPayload(const PacketView& _packet, LossNotice& _loss) noexcept;

[[nodiscard]] CORE_API DecodeStatus
DecodeSessionBeginPayload(const PacketView& _packet, SessionBeginInfo& _session) noexcept;

[[nodiscard]] CORE_API DecodeStatus
DecodeSessionEndPayload(const PacketView& _packet, SessionEndInfo& _session) noexcept;

} // namespace Moer::ProfileDump

#endif // MOER_ENGINE_PROFILE_DUMP_CODEC_H
