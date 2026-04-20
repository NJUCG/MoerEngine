#pragma once

#include "API_Macro.h"
#include "misc/STL.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Moer::ProfileDump {

enum class EChannel : uint8_t {
    CPUThread = 0,
    GPUQueue,
};

enum class EKind : uint8_t {
    Scope = 0,
    Counter,
    Instant,
};

enum class EFieldType : uint8_t {
    Bool = 0,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float32,
    Float64,
    String,
};

inline constexpr uint32_t packet_magic = 0x5344504Du;
inline constexpr uint16_t packet_version = 1;

enum class EPacketType : uint16_t {
    Schema = 1,
    Record = 2,
};

struct PacketHeader {
    uint32_t    magic = packet_magic;
    uint16_t    version = packet_version;
    EPacketType type = EPacketType::Schema;
    uint32_t    payload_size = 0;
};

struct SchemaFieldDesc {
    std::string_view name;
    EFieldType       type;
};

struct DecodedFieldDesc {
    std::string name;
    EFieldType  type = EFieldType::Bool;
};

struct DecodedValue {
    EFieldType   type = EFieldType::Bool;
    bool         bool_value = false;
    int32_t      int32_value = 0;
    uint32_t     uint32_value = 0;
    int64_t      int64_value = 0;
    uint64_t     uint64_value = 0;
    float        float32_value = 0.0f;
    double       float64_value = 0.0;
    std::string  string_value;
};

struct DecodedSchema {
    uint32_t               schema_id = 0;
    std::string            name;
    EKind                  kind = EKind::Scope;
    EChannel               channel = EChannel::CPUThread;
    uint32_t               version = 0;
    Array<DecodedFieldDesc> fields;
};

struct DecodedRecord {
    uint32_t            schema_id = 0;
    uint64_t            sequence = 0;
    Array<DecodedValue> fields;
};

struct SchemaRuntimeDesc {
    uint32_t                        schema_id;
    std::string_view                name;
    EKind                           kind;
    EChannel                        channel;
    uint32_t                        version;
    std::span<const SchemaFieldDesc> fields;
};

struct RuntimeConfig {
    bool        enable_file = true;
    bool        enable_tcp = false;
    int         tls_max_records = 256;
    int         tls_max_bytes = 32768;
    int         auto_publish_records = 64;
    int         auto_publish_bytes = 8192;
    std::string file_path;
    std::string tcp_host = "127.0.0.1";
    int         tcp_port = 19090;
};

class StreamCommitToken {};

CORE_API void FlushThreadLocal();
CORE_API void FlushAll();
CORE_API void Shutdown();
CORE_API void OverrideOutputFileForCurrentSession(const std::filesystem::path& path);
CORE_API void OverrideConfigForTesting(const RuntimeConfig& config);
CORE_API void ClearTestingConfigOverride();
CORE_API RuntimeConfig GetRuntimeConfigSnapshot();

namespace Detail {

template<typename T>
inline bool ReadPacketScalar(std::span<const uint8_t> bytes, size_t& offset, T& value) {
    if (offset + sizeof(T) > bytes.size()) {
        return false;
    }

    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

inline bool ReadPacketString(std::span<const uint8_t> bytes, size_t& offset, std::string& text) {
    uint32_t length = 0;
    if (!ReadPacketScalar(bytes, offset, length)) {
        return false;
    }
    if (offset + length > bytes.size()) {
        return false;
    }

    text.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
    offset += length;
    return true;
}

inline bool IsValidPacketType(EPacketType type) {
    return type == EPacketType::Schema || type == EPacketType::Record;
}

inline bool IsValidFieldType(EFieldType type) {
    return type == EFieldType::Bool ||
           type == EFieldType::Int32 ||
           type == EFieldType::UInt32 ||
           type == EFieldType::Int64 ||
           type == EFieldType::UInt64 ||
           type == EFieldType::Float32 ||
           type == EFieldType::Float64 ||
           type == EFieldType::String;
}

inline bool IsValidKind(EKind kind) {
    return kind == EKind::Scope || kind == EKind::Counter || kind == EKind::Instant;
}

inline bool IsValidChannel(EChannel channel) {
    return channel == EChannel::CPUThread || channel == EChannel::GPUQueue;
}

inline bool DeserializeRecordFields(
    std::span<const uint8_t>     bytes,
    std::span<const DecodedFieldDesc> schema_fields,
    Array<DecodedValue>&         fields
) {
    fields.clear();
    fields.reserve(schema_fields.size());

    size_t offset = 0;
    for (const DecodedFieldDesc& field : schema_fields) {
        if (!IsValidFieldType(field.type)) {
            return false;
        }

        DecodedValue decoded_value{};
        decoded_value.type = field.type;
        switch (field.type) {
            case EFieldType::Bool: {
                uint8_t value = 0;
                if (!ReadPacketScalar(bytes, offset, value)) {
                    return false;
                }
                decoded_value.bool_value = value != 0;
                break;
            }
            case EFieldType::Int32: {
                int32_t value = 0;
                if (!ReadPacketScalar(bytes, offset, value)) {
                    return false;
                }
                decoded_value.int32_value = value;
                break;
            }
            case EFieldType::UInt32: {
                uint32_t value = 0;
                if (!ReadPacketScalar(bytes, offset, value)) {
                    return false;
                }
                decoded_value.uint32_value = value;
                break;
            }
            case EFieldType::Int64: {
                int64_t value = 0;
                if (!ReadPacketScalar(bytes, offset, value)) {
                    return false;
                }
                decoded_value.int64_value = value;
                break;
            }
            case EFieldType::UInt64: {
                uint64_t value = 0;
                if (!ReadPacketScalar(bytes, offset, value)) {
                    return false;
                }
                decoded_value.uint64_value = value;
                break;
            }
            case EFieldType::Float32: {
                float value = 0.0f;
                if (!ReadPacketScalar(bytes, offset, value)) {
                    return false;
                }
                decoded_value.float32_value = value;
                break;
            }
            case EFieldType::Float64: {
                double value = 0.0;
                if (!ReadPacketScalar(bytes, offset, value)) {
                    return false;
                }
                decoded_value.float64_value = value;
                break;
            }
            case EFieldType::String: {
                if (!ReadPacketString(bytes, offset, decoded_value.string_value)) {
                    return false;
                }
                break;
            }
        }

        fields.emplace_back(std::move(decoded_value));
    }

    return offset == bytes.size();
}

} // namespace Detail

inline bool ReadNextPacket(
    std::span<const uint8_t> bytes,
    size_t&                  offset,
    PacketHeader&            header,
    std::span<const uint8_t>& payload
) {
    const size_t original_offset = offset;
    if (offset + sizeof(PacketHeader) > bytes.size()) {
        return false;
    }

    PacketHeader parsed_header{};
    std::memcpy(&parsed_header, bytes.data() + offset, sizeof(PacketHeader));
    offset += sizeof(PacketHeader);

    if (parsed_header.magic != packet_magic ||
        parsed_header.version != packet_version ||
        !Detail::IsValidPacketType(parsed_header.type) ||
        offset + parsed_header.payload_size > bytes.size()) {
        offset = original_offset;
        return false;
    }

    header = parsed_header;
    payload = bytes.subspan(offset, parsed_header.payload_size);
    offset += parsed_header.payload_size;
    return true;
}

inline bool DeserializeSchemaPacket(
    const PacketHeader&      header,
    std::span<const uint8_t> payload,
    DecodedSchema&           schema
) {
    if (header.type != EPacketType::Schema) {
        return false;
    }

    size_t offset = 0;
    DecodedSchema parsed_schema{};
    uint8_t parsed_kind = 0;
    uint8_t parsed_channel = 0;
    uint32_t field_count = 0;

    if (!Detail::ReadPacketScalar(payload, offset, parsed_schema.schema_id) ||
        !Detail::ReadPacketScalar(payload, offset, parsed_kind) ||
        !Detail::ReadPacketScalar(payload, offset, parsed_channel) ||
        !Detail::ReadPacketScalar(payload, offset, parsed_schema.version) ||
        !Detail::ReadPacketScalar(payload, offset, field_count) ||
        !Detail::ReadPacketString(payload, offset, parsed_schema.name)) {
        return false;
    }

    parsed_schema.kind = static_cast<EKind>(parsed_kind);
    parsed_schema.channel = static_cast<EChannel>(parsed_channel);
    if (!Detail::IsValidKind(parsed_schema.kind) || !Detail::IsValidChannel(parsed_schema.channel)) {
        return false;
    }

    parsed_schema.fields.clear();
    parsed_schema.fields.reserve(field_count);
    for (uint32_t field_index = 0; field_index < field_count; ++field_index) {
        uint8_t parsed_type = 0;
        DecodedFieldDesc field{};
        if (!Detail::ReadPacketScalar(payload, offset, parsed_type) ||
            !Detail::ReadPacketString(payload, offset, field.name)) {
            return false;
        }
        field.type = static_cast<EFieldType>(parsed_type);
        if (!Detail::IsValidFieldType(field.type)) {
            return false;
        }
        parsed_schema.fields.emplace_back(std::move(field));
    }

    if (offset != payload.size()) {
        return false;
    }

    schema = std::move(parsed_schema);
    return true;
}

inline bool DeserializeRecordPacket(
    const PacketHeader&      header,
    std::span<const uint8_t> payload,
    const DecodedSchema&     schema,
    DecodedRecord&           record
) {
    if (header.type != EPacketType::Record) {
        return false;
    }

    size_t offset = 0;
    DecodedRecord parsed_record{};
    uint32_t value_bytes = 0;
    if (!Detail::ReadPacketScalar(payload, offset, parsed_record.schema_id) ||
        !Detail::ReadPacketScalar(payload, offset, parsed_record.sequence) ||
        !Detail::ReadPacketScalar(payload, offset, value_bytes)) {
        return false;
    }
    if (parsed_record.schema_id != schema.schema_id || offset + value_bytes != payload.size()) {
        return false;
    }

    if (!Detail::DeserializeRecordFields(payload.subspan(offset, value_bytes), schema.fields, parsed_record.fields)) {
        return false;
    }

    record = std::move(parsed_record);
    return true;
}

namespace Detail {

template<typename T>
using RemoveCvRefT = std::remove_cv_t<std::remove_reference_t<T>>;

template<typename T>
using DecayedT = std::decay_t<T>;

template<typename T>
struct NormalizedFieldType {
    using type = RemoveCvRefT<T>;
};

template<typename T>
    requires std::is_enum_v<RemoveCvRefT<T>>
struct NormalizedFieldType<T> {
    using type = std::underlying_type_t<RemoveCvRefT<T>>;
};

template<typename T>
using NormalizedFieldTypeT = typename NormalizedFieldType<T>::type;

template<typename T>
inline constexpr bool is_string_like_v =
    std::is_same_v<DecayedT<T>, std::string> ||
    std::is_same_v<DecayedT<T>, std::string_view> ||
    std::is_same_v<DecayedT<T>, const char*> ||
    std::is_same_v<DecayedT<T>, char*>;

template<typename T>
struct FieldTypeTag;

template<>
struct FieldTypeTag<bool> {
    static constexpr EFieldType value = EFieldType::Bool;
};

template<>
struct FieldTypeTag<int32_t> {
    static constexpr EFieldType value = EFieldType::Int32;
};

template<>
struct FieldTypeTag<uint32_t> {
    static constexpr EFieldType value = EFieldType::UInt32;
};

template<>
struct FieldTypeTag<int64_t> {
    static constexpr EFieldType value = EFieldType::Int64;
};

template<>
struct FieldTypeTag<uint64_t> {
    static constexpr EFieldType value = EFieldType::UInt64;
};

template<>
struct FieldTypeTag<float> {
    static constexpr EFieldType value = EFieldType::Float32;
};

template<>
struct FieldTypeTag<double> {
    static constexpr EFieldType value = EFieldType::Float64;
};

template<>
struct FieldTypeTag<std::string> {
    static constexpr EFieldType value = EFieldType::String;
};

template<>
struct FieldTypeTag<std::string_view> {
    static constexpr EFieldType value = EFieldType::String;
};

template<typename ExpectedT, typename ValueT>
constexpr bool CanSerializeField() {
    using NormalizedExpected = NormalizedFieldTypeT<ExpectedT>;
    using NormalizedValue = NormalizedFieldTypeT<ValueT>;
    if constexpr (std::is_same_v<NormalizedExpected, bool>) {
        return std::is_same_v<RemoveCvRefT<ValueT>, bool>;
    } else if constexpr (std::is_same_v<NormalizedExpected, int32_t>) {
        return (std::is_integral_v<NormalizedValue> || std::is_enum_v<RemoveCvRefT<ValueT>>) &&
               !std::is_same_v<NormalizedValue, bool>;
    } else if constexpr (std::is_same_v<NormalizedExpected, uint32_t>) {
        return (std::is_integral_v<NormalizedValue> || std::is_enum_v<RemoveCvRefT<ValueT>>) &&
               !std::is_same_v<NormalizedValue, bool>;
    } else if constexpr (std::is_same_v<NormalizedExpected, int64_t>) {
        return (std::is_integral_v<NormalizedValue> || std::is_enum_v<RemoveCvRefT<ValueT>>) &&
               !std::is_same_v<NormalizedValue, bool>;
    } else if constexpr (std::is_same_v<NormalizedExpected, uint64_t>) {
        return (std::is_integral_v<NormalizedValue> || std::is_enum_v<RemoveCvRefT<ValueT>>) &&
               !std::is_same_v<NormalizedValue, bool>;
    } else if constexpr (std::is_same_v<NormalizedExpected, float>) {
        return std::is_arithmetic_v<NormalizedValue> && !std::is_same_v<NormalizedValue, bool>;
    } else if constexpr (std::is_same_v<NormalizedExpected, double>) {
        return std::is_arithmetic_v<NormalizedValue> && !std::is_same_v<NormalizedValue, bool>;
    } else if constexpr (std::is_same_v<NormalizedExpected, std::string>) {
        return is_string_like_v<ValueT>;
    } else if constexpr (std::is_same_v<NormalizedExpected, std::string_view>) {
        return is_string_like_v<ValueT>;
    } else {
        return false;
    }
}

CORE_API uint64_t ReserveSequence();
CORE_API const SchemaRuntimeDesc& RegisterSchemaRuntime(
    std::string_view                 name,
    EKind                            kind,
    EChannel                         channel,
    uint32_t                         version,
    std::span<const SchemaFieldDesc> fields
);
CORE_API void CommitSerializedRecord(uint32_t schema_id, uint64_t sequence, Array<uint8_t>&& payload);

template<typename T>
void AppendScalar(Array<uint8_t>& payload, const T& value) {
    const auto* begin = reinterpret_cast<const uint8_t*>(&value);
    payload.insert(payload.end(), begin, begin + sizeof(T));
}

inline std::string_view ToStringView(const std::string& value) {
    return value;
}

inline std::string_view ToStringView(std::string_view value) {
    return value;
}

inline std::string_view ToStringView(const char* value) {
    return value ? std::string_view(value) : std::string_view();
}

inline std::string_view ToStringView(char* value) {
    return value ? std::string_view(value) : std::string_view();
}

template<size_t N>
inline std::string_view ToStringView(const char (&value)[N]) {
    return std::string_view(value, N - 1);
}

template<typename ExpectedT, typename ValueT>
void SerializeField(Array<uint8_t>& payload, ValueT&& value) {
    using NormalizedExpected = NormalizedFieldTypeT<ExpectedT>;
    if constexpr (std::is_same_v<NormalizedExpected, bool>) {
        const uint8_t encoded = value ? 1u : 0u;
        AppendScalar(payload, encoded);
    } else if constexpr (std::is_same_v<NormalizedExpected, int32_t>) {
        const int32_t encoded = static_cast<int32_t>(value);
        AppendScalar(payload, encoded);
    } else if constexpr (std::is_same_v<NormalizedExpected, uint32_t>) {
        const uint32_t encoded = static_cast<uint32_t>(value);
        AppendScalar(payload, encoded);
    } else if constexpr (std::is_same_v<NormalizedExpected, int64_t>) {
        const int64_t encoded = static_cast<int64_t>(value);
        AppendScalar(payload, encoded);
    } else if constexpr (std::is_same_v<NormalizedExpected, uint64_t>) {
        const uint64_t encoded = static_cast<uint64_t>(value);
        AppendScalar(payload, encoded);
    } else if constexpr (std::is_same_v<NormalizedExpected, float>) {
        const float encoded = static_cast<float>(value);
        AppendScalar(payload, encoded);
    } else if constexpr (std::is_same_v<NormalizedExpected, double>) {
        const double encoded = static_cast<double>(value);
        AppendScalar(payload, encoded);
    } else if constexpr (
        std::is_same_v<NormalizedExpected, std::string> ||
        std::is_same_v<NormalizedExpected, std::string_view>) {
        const std::string_view text = ToStringView(std::forward<ValueT>(value));
        const uint32_t         length = static_cast<uint32_t>(text.size());
        AppendScalar(payload, length);
        payload.insert(payload.end(), text.begin(), text.end());
    }
}

template<typename Template, size_t... Indices>
constexpr auto BuildFieldDescArray(std::index_sequence<Indices...>) {
    using FieldTuple = typename Template::FieldTypes;
    return std::array<SchemaFieldDesc, sizeof...(Indices)>{
        SchemaFieldDesc{
            std::string_view(Template::field_names[Indices]),
            FieldTypeTag<NormalizedFieldTypeT<std::tuple_element_t<Indices, FieldTuple>>>::value,
        }...
    };
}

template<typename Template>
void ValidateTemplate() {
    using FieldTuple = typename Template::FieldTypes;
    static_assert(requires { Template::kName; }, "Profile dump template must define kName.");
    static_assert(requires { Template::kKind; }, "Profile dump template must define kKind.");
    static_assert(requires { Template::kChannel; }, "Profile dump template must define kChannel.");
    static_assert(requires { Template::kVersion; }, "Profile dump template must define kVersion.");
    static_assert(
        std::tuple_size_v<FieldTuple> == std::tuple_size_v<decltype(Template::field_names)>,
        "Profile dump template field count must match field names."
    );

    []<size_t... Indices>(std::index_sequence<Indices...>) {
        ((void)FieldTypeTag<NormalizedFieldTypeT<std::tuple_element_t<Indices, FieldTuple>>>::value, ...);
    }(std::make_index_sequence<std::tuple_size_v<FieldTuple>>{});
}

template<typename Template>
const SchemaRuntimeDesc& ResolveSchema() {
    ValidateTemplate<Template>();
    using FieldTuple = typename Template::FieldTypes;
    static constexpr auto field_descs = BuildFieldDescArray<Template>(
        std::make_index_sequence<std::tuple_size_v<FieldTuple>>{}
    );
    static const SchemaRuntimeDesc& schema = RegisterSchemaRuntime(
        Template::kName,
        Template::kKind,
        Template::kChannel,
        Template::kVersion,
        std::span<const SchemaFieldDesc>(field_descs.data(), field_descs.size())
    );
    return schema;
}

} // namespace Detail

template<typename Template, size_t Index>
class [[nodiscard("Profile dump chain must provide every field declared by the schema.")]] StreamBuilder {
public:
    StreamBuilder(const SchemaRuntimeDesc& schema, uint64_t sequence, Array<uint8_t>&& payload) :
        m_schema(schema),
        m_sequence(sequence),
        m_payload(std::move(payload)) {}

    template<typename ValueT>
    auto operator<<(ValueT&& value) && {
        using FieldTuple = typename Template::FieldTypes;
        using ExpectedT = std::tuple_element_t<Index, FieldTuple>;
        static_assert(
            Detail::CanSerializeField<ExpectedT, ValueT>(),
            "Profile dump field type does not match template schema."
        );

        Detail::SerializeField<ExpectedT>(m_payload, std::forward<ValueT>(value));
        if constexpr (Index + 1 == std::tuple_size_v<FieldTuple>) {
            Detail::CommitSerializedRecord(m_schema.schema_id, m_sequence, std::move(m_payload));
            return StreamCommitToken{};
        } else {
            return StreamBuilder<Template, Index + 1>(m_schema, m_sequence, std::move(m_payload));
        }
    }

private:
    const SchemaRuntimeDesc& m_schema;
    uint64_t                 m_sequence;
    Array<uint8_t>           m_payload;
};

template<typename Template>
auto BeginStream() {
    Detail::ValidateTemplate<Template>();
    const SchemaRuntimeDesc& schema = Detail::ResolveSchema<Template>();
    Array<uint8_t> payload;
    payload.reserve(128);
    return StreamBuilder<Template, 0>(schema, Detail::ReserveSequence(), std::move(payload));
}

#define MOER_PROFILE_DUMP_TEMPLATE(Name, KindValue, ChannelValue, VersionValue) \
    struct Name { \
        static constexpr std::string_view kName = #Name; \
        static constexpr ::Moer::ProfileDump::EKind kKind = KindValue; \
        static constexpr ::Moer::ProfileDump::EChannel kChannel = ChannelValue; \
        static constexpr uint32_t kVersion = VersionValue;

#define DUMP_STREAM(TemplateName) ::Moer::ProfileDump::BeginStream<TemplateName>()

namespace Templates {

MOER_PROFILE_DUMP_TEMPLATE(CpuScopeTemplate, EKind::Scope, EChannel::CPUThread, 1)
    using FieldTypes = std::tuple<uint64_t, std::string_view, int64_t, int64_t, uint32_t>;
    static constexpr std::array field_names{"thread_id", "name", "start_us", "duration_us", "depth"};
};

MOER_PROFILE_DUMP_TEMPLATE(GpuScopeTemplate, EKind::Scope, EChannel::GPUQueue, 1)
    using FieldTypes = std::tuple<
        uint64_t,
        std::string_view,
        std::string_view,
        uint64_t,
        uint64_t,
        uint32_t,
        uint64_t,
        uint64_t
    >;
    static constexpr std::array field_names{
        "frame_index",
        "queue_name",
        "name",
        "start_ns",
        "end_ns",
        "depth",
        "total_busy_ns",
        "exclusive_ns"
    };
};

} // namespace Templates

} // namespace Moer::ProfileDump