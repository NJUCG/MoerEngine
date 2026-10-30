#ifndef MOER_ENGINE_PROFILE_DUMP_CODEC_H
#define MOER_ENGINE_PROFILE_DUMP_CODEC_H

#include "API_Macro.h"
#include "misc/STL.h"

#include "misc/Types.h"
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace Moer::ProfileDump {

// ProfileDump values cross the Moer::Core DLL boundary. Use the engine
// allocator explicitly so an owning string allocated by the codec is released
// through Memory::Free in every consuming module.
using ProfileString = std::basic_string<char, std::char_traits<char>, MoerStlAllocator<char>>;

inline constexpr uint32 kPacketMagic       = 0x5344504Du;
inline constexpr uint16 kWireVersion       = 3;
inline constexpr uint16 kPacketHeaderBytes = 32;

enum class Channel : uint8 {
    CpuThread = 0,
    GpuQueue  = 1,
};

enum class EventKind : uint8 {
    Scope   = 0,
    Counter = 1,
    Instant = 2,
};

enum class FieldType : uint8 {
    Bool = 0,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    String,
};

enum class PacketType : uint16 {
    SessionBegin = 1,
    Schema       = 2,
    Record       = 3,
    Loss         = 4,
    SessionEnd   = 5,
};

enum class EncodeStatus : uint8 {
    Ok = 0,
    InvalidSchema,
    ValueCountMismatch,
    ValueTypeMismatch,
    StringTooLarge,
    PayloadTooLarge,
};

enum class DecodeStatus : uint8 {
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
    uint32        max_fields{64};
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
    uint32             schema_version{1};
    Array<SchemaField> fields{};

    friend bool operator==(const SchemaDescriptor&, const SchemaDescriptor&) = default;
};

using FieldValue = std::
    variant<bool, int32, uint32, int64, uint64, float, double, ProfileString>;

using FieldValueView = std::
    variant<bool, int32, uint32, int64, uint64, float, double, std::string_view>;

struct PacketHeader {
    uint32        magic{kPacketMagic};
    uint16        wire_version{kWireVersion};
    uint16        header_bytes{kPacketHeaderBytes};
    PacketType    type{PacketType::SessionBegin};
    uint16        flags{0};
    uint32        payload_bytes{0};
    uint64        packet_index{0};
    uint32        payload_crc32{0};
    uint32        header_crc32{0};
};

struct PacketView {
    PacketHeader                  header{};
    std::span<const uint8>        payload{};
};

struct DecodedRecord {
    uint64            schema_hash{0};
    uint64            sequence{0};
    Array<FieldValue> values{};
};

enum class LossReason : uint32 {
    None            = 0,
    Oversized       = 1u << 0,
    QueueFull       = 1u << 1,
    StaleGeneration = 1u << 2,
    AfterShutdown   = 1u << 3,
    SinkFault       = 1u << 4,
};

struct LossNotice {
    uint64 first_sequence{0};
    uint64 last_sequence{0};
    uint64 record_count{0};
    uint64 value_bytes{0};
    uint32 reason_mask{0};
};

struct SessionBeginInfo {
    uint64 generation{0};
    uint64 started_unix_ns{0};
};

struct SessionEndInfo {
    uint64 generation{0};
    uint64 records_written{0};
    uint64 records_dropped{0};
};

[[nodiscard]] CORE_API uint64 ComputeSchemaHash(const SchemaDescriptor& _schema) noexcept;

[[nodiscard]] CORE_API EncodeStatus EncodeSchemaPayload(
    const SchemaDescriptor& _schema,
    const CodecLimits&      _limits,
    Array<uint8>&           _payload
);

[[nodiscard]] CORE_API EncodeStatus EncodeRecordPayload(
    uint64                          _schema_hash,
    uint64                          _sequence,
    std::span<const SchemaField>    _fields,
    std::span<const FieldValueView> _values,
    const CodecLimits&              _limits,
    Array<uint8>&                   _payload
);

CORE_API void EncodeLossPayload(const LossNotice& _loss, Array<uint8>& _payload);

CORE_API void EncodeSessionBeginPayload(const SessionBeginInfo& _session, Array<uint8>& _payload);

CORE_API void EncodeSessionEndPayload(const SessionEndInfo& _session, Array<uint8>& _payload);

[[nodiscard]] CORE_API EncodeStatus WrapPacket(
    PacketType             _type,
    uint64                 _packet_index,
    std::span<const uint8> _payload,
    const CodecLimits&     _limits,
    Array<uint8>&          _packet
);

[[nodiscard]] CORE_API DecodeStatus DecodePacket(
    std::span<const uint8> _input,
    const CodecLimits&     _limits,
    PacketView&            _packet,
    std::size_t&           _consumed
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
