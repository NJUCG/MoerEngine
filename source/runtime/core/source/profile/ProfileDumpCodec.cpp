#include "profile/ProfileDumpCodec.h"

#include "misc/Crc32.h"

#include <bit>
#include <limits>
#include <type_traits>
#include <utility>

namespace Moer::ProfileDump {

namespace {

constexpr std::uint64_t    kFnvOffsetBasis   = 14695981039346656037ull;
constexpr std::uint64_t    kFnvPrime         = 1099511628211ull;
constexpr std::string_view kSchemaHashDomain = "moer.profile.schema.v3";
constexpr std::size_t      kHeaderCrcBytes   = kPacketHeaderBytes - sizeof(std::uint32_t);
constexpr std::size_t      kRecordPrefixBytes =
    sizeof(std::uint64_t) + sizeof(std::uint64_t) + sizeof(std::uint32_t);
constexpr std::size_t kLossPayloadBytes         = sizeof(std::uint64_t) * 4 + sizeof(std::uint32_t);
constexpr std::size_t kSessionBeginPayloadBytes = sizeof(std::uint64_t) * 2;
constexpr std::size_t kSessionEndPayloadBytes   = sizeof(std::uint64_t) * 3;

static_assert(kPacketHeaderBytes == 32);
static_assert(kHeaderCrcBytes == 28);
static_assert(sizeof(float) == sizeof(std::uint32_t));
static_assert(sizeof(double) == sizeof(std::uint64_t));
static_assert(std::numeric_limits<float>::is_iec559);
static_assert(std::numeric_limits<double>::is_iec559);

bool IsValidChannel(Channel _channel) noexcept {
    return _channel == Channel::CpuThread || _channel == Channel::GpuQueue;
}

bool IsValidEventKind(EventKind _kind) noexcept {
    return _kind == EventKind::Scope || _kind == EventKind::Counter || _kind == EventKind::Instant;
}

bool IsValidFieldType(FieldType _type) noexcept {
    return _type == FieldType::Bool || _type == FieldType::Int32 || _type == FieldType::UInt32 ||
           _type == FieldType::Int64 || _type == FieldType::UInt64 || _type == FieldType::Float32 ||
           _type == FieldType::Float64 || _type == FieldType::String;
}

bool IsValidPacketType(PacketType _type) noexcept {
    return _type == PacketType::SessionBegin || _type == PacketType::Schema || _type == PacketType::Record ||
           _type == PacketType::Loss || _type == PacketType::SessionEnd;
}

template<typename T>
    requires(std::is_unsigned_v<T>)
void AppendLittleEndian(Array<std::uint8_t>& _output, T _value) {
    for (std::size_t byte_index = 0; byte_index < sizeof(T); ++byte_index) {
        _output.emplace_back(static_cast<std::uint8_t>(_value & T{0xff}));
        _value >>= 8;
    }
}

template<typename T>
    requires(std::is_signed_v<T>)
void AppendLittleEndian(Array<std::uint8_t>& _output, T _value) {
    using UnsignedT = std::make_unsigned_t<T>;
    AppendLittleEndian(_output, std::bit_cast<UnsignedT>(_value));
}

void AppendFloat(Array<std::uint8_t>& _output, float _value) {
    AppendLittleEndian(_output, std::bit_cast<std::uint32_t>(_value));
}

void AppendFloat(Array<std::uint8_t>& _output, double _value) {
    AppendLittleEndian(_output, std::bit_cast<std::uint64_t>(_value));
}

bool CanEncodeU32Length(std::size_t _size) noexcept {
    return _size <= std::numeric_limits<std::uint32_t>::max();
}

bool CanGrowPayload(
    std::size_t        _current_size,
    std::size_t        _additional_size,
    const CodecLimits& _limits
) noexcept {
    return _additional_size <= std::numeric_limits<std::size_t>::max() - _current_size &&
           _current_size + _additional_size <= _limits.max_packet_payload_bytes &&
           CanEncodeU32Length(_current_size + _additional_size);
}

void AppendString(Array<std::uint8_t>& _output, std::string_view _value) {
    AppendLittleEndian(_output, static_cast<std::uint32_t>(_value.size()));
    _output.insert(_output.end(), _value.begin(), _value.end());
}

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> _bytes) noexcept : m_bytes(_bytes) {}

    template<typename T>
        requires(std::is_unsigned_v<T>)
    bool ReadLittleEndian(T& _value) noexcept {
        if (Remaining() < sizeof(T)) {
            return false;
        }

        T value = 0;
        for (std::size_t byte_index = 0; byte_index < sizeof(T); ++byte_index) {
            value |= static_cast<T>(m_bytes[m_offset + byte_index]) << (byte_index * 8);
        }
        m_offset += sizeof(T);
        _value = value;
        return true;
    }

    template<typename T>
        requires(std::is_signed_v<T>)
    bool ReadLittleEndian(T& _value) noexcept {
        using UnsignedT   = std::make_unsigned_t<T>;
        UnsignedT encoded = 0;
        if (!ReadLittleEndian(encoded)) {
            return false;
        }
        _value = std::bit_cast<T>(encoded);
        return true;
    }

    bool ReadFloat(float& _value) noexcept {
        std::uint32_t bits = 0;
        if (!ReadLittleEndian(bits)) {
            return false;
        }
        _value = std::bit_cast<float>(bits);
        return true;
    }

    bool ReadFloat(double& _value) noexcept {
        std::uint64_t bits = 0;
        if (!ReadLittleEndian(bits)) {
            return false;
        }
        _value = std::bit_cast<double>(bits);
        return true;
    }

    DecodeStatus ReadString(ProfileString& _value, const CodecLimits& _limits) {
        std::uint32_t length = 0;
        if (!ReadLittleEndian(length)) {
            return DecodeStatus::MalformedPayload;
        }
        if (length > _limits.max_string_bytes) {
            return DecodeStatus::LimitExceeded;
        }
        if (static_cast<std::size_t>(length) > Remaining()) {
            return DecodeStatus::MalformedPayload;
        }

        _value.assign(reinterpret_cast<const char*>(m_bytes.data() + m_offset), length);
        m_offset += length;
        return DecodeStatus::Ok;
    }

    bool ReadSpan(std::size_t _size, std::span<const std::uint8_t>& _value) noexcept {
        if (_size > Remaining()) {
            return false;
        }
        _value = m_bytes.subspan(m_offset, _size);
        m_offset += _size;
        return true;
    }

    [[nodiscard]] std::size_t Remaining() const noexcept {
        return m_bytes.size() - m_offset;
    }

    [[nodiscard]] bool AtEnd() const noexcept {
        return m_offset == m_bytes.size();
    }

private:
    std::span<const std::uint8_t> m_bytes{};
    std::size_t                   m_offset{0};
};

class StableHasher {
public:
    void AppendByte(std::uint8_t _value) noexcept {
        m_hash ^= _value;
        m_hash *= kFnvPrime;
    }

    template<typename T>
        requires(std::is_unsigned_v<T>)
    void AppendLittleEndian(T _value) noexcept {
        for (std::size_t byte_index = 0; byte_index < sizeof(T); ++byte_index) {
            AppendByte(static_cast<std::uint8_t>(_value & T{0xff}));
            _value >>= 8;
        }
    }

    void AppendString(std::string_view _value) noexcept {
        AppendLittleEndian(static_cast<std::uint32_t>(_value.size()));
        for (const char character : _value) {
            AppendByte(static_cast<std::uint8_t>(static_cast<unsigned char>(character)));
        }
    }

    [[nodiscard]] std::uint64_t Finish() const noexcept {
        return m_hash;
    }

private:
    std::uint64_t m_hash{kFnvOffsetBasis};
};

bool HasDuplicateFieldNames(const SchemaDescriptor& _schema) noexcept {
    for (std::size_t field_index = 0; field_index < _schema.fields.size(); ++field_index) {
        for (std::size_t other_index = field_index + 1; other_index < _schema.fields.size(); ++other_index) {
            if (_schema.fields[field_index].name == _schema.fields[other_index].name) {
                return true;
            }
        }
    }
    return false;
}

bool IsSchemaDescriptorHashable(const SchemaDescriptor& _schema) noexcept {
    if (_schema.name.empty() || _schema.event_type.empty() || !IsValidEventKind(_schema.kind) ||
        !IsValidChannel(_schema.channel) || !CanEncodeU32Length(_schema.name.size()) ||
        !CanEncodeU32Length(_schema.event_type.size()) ||
        _schema.fields.size() > std::numeric_limits<std::uint16_t>::max() ||
        _schema.fields.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }

    for (const SchemaField& field : _schema.fields) {
        if (field.name.empty() || !IsValidFieldType(field.type) || !CanEncodeU32Length(field.name.size())) {
            return false;
        }
    }
    return !HasDuplicateFieldNames(_schema);
}

EncodeStatus ValidateSchemaForEncoding(const SchemaDescriptor& _schema, const CodecLimits& _limits) noexcept {
    if (_schema.name.empty() || _schema.event_type.empty() || !IsValidEventKind(_schema.kind) ||
        !IsValidChannel(_schema.channel) || _schema.fields.size() > _limits.max_fields ||
        _schema.fields.size() > std::numeric_limits<std::uint16_t>::max() ||
        HasDuplicateFieldNames(_schema)) {
        return EncodeStatus::InvalidSchema;
    }
    if (!CanEncodeU32Length(_schema.name.size()) || !CanEncodeU32Length(_schema.event_type.size()) ||
        _schema.name.size() > _limits.max_string_bytes ||
        _schema.event_type.size() > _limits.max_string_bytes) {
        return EncodeStatus::StringTooLarge;
    }

    for (const SchemaField& field : _schema.fields) {
        if (field.name.empty() || !IsValidFieldType(field.type)) {
            return EncodeStatus::InvalidSchema;
        }
        if (!CanEncodeU32Length(field.name.size()) || field.name.size() > _limits.max_string_bytes) {
            return EncodeStatus::StringTooLarge;
        }
    }

    return EncodeStatus::Ok;
}

std::uint32_t ComputeCrc32(std::span<const std::uint8_t> _bytes) noexcept {
    return crc32_fast(_bytes.data(), _bytes.size());
}

bool ValidatePayloadView(const PacketView& _packet, PacketType _expected_type) noexcept {
    return _packet.header.type == _expected_type && _packet.header.payload_bytes == _packet.payload.size();
}

template<typename T>
bool HoldsFieldValue(const FieldValueView& _value) noexcept {
    return std::holds_alternative<T>(_value);
}

EncodeStatus AppendFieldValue(
    Array<std::uint8_t>&  _encoded,
    const SchemaField&    _field,
    const FieldValueView& _value,
    const CodecLimits&    _limits
) {
    switch (_field.type) {
        case FieldType::Bool:
            if (!HoldsFieldValue<bool>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            _encoded.emplace_back(std::get<bool>(_value) ? 1u : 0u);
            return EncodeStatus::Ok;
        case FieldType::Int32:
            if (!HoldsFieldValue<std::int32_t>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            AppendLittleEndian(_encoded, std::get<std::int32_t>(_value));
            return EncodeStatus::Ok;
        case FieldType::UInt32:
            if (!HoldsFieldValue<std::uint32_t>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            AppendLittleEndian(_encoded, std::get<std::uint32_t>(_value));
            return EncodeStatus::Ok;
        case FieldType::Int64:
            if (!HoldsFieldValue<std::int64_t>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            AppendLittleEndian(_encoded, std::get<std::int64_t>(_value));
            return EncodeStatus::Ok;
        case FieldType::UInt64:
            if (!HoldsFieldValue<std::uint64_t>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            AppendLittleEndian(_encoded, std::get<std::uint64_t>(_value));
            return EncodeStatus::Ok;
        case FieldType::Float32:
            if (!HoldsFieldValue<float>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            AppendFloat(_encoded, std::get<float>(_value));
            return EncodeStatus::Ok;
        case FieldType::Float64:
            if (!HoldsFieldValue<double>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            AppendFloat(_encoded, std::get<double>(_value));
            return EncodeStatus::Ok;
        case FieldType::String: {
            if (!HoldsFieldValue<std::string_view>(_value)) {
                return EncodeStatus::ValueTypeMismatch;
            }
            const std::string_view text = std::get<std::string_view>(_value);
            if (!CanEncodeU32Length(text.size()) || text.size() > _limits.max_string_bytes) {
                return EncodeStatus::StringTooLarge;
            }
            AppendString(_encoded, text);
            return EncodeStatus::Ok;
        }
    }
    return EncodeStatus::ValueTypeMismatch;
}

DecodeStatus DecodeFieldValue(
    ByteReader&        _reader,
    const SchemaField& _field,
    const CodecLimits& _limits,
    FieldValue&        _value
) {
    switch (_field.type) {
        case FieldType::Bool: {
            std::uint8_t value = 0;
            if (!_reader.ReadLittleEndian(value) || value > 1) {
                return DecodeStatus::MalformedPayload;
            }
            _value = value != 0;
            return DecodeStatus::Ok;
        }
        case FieldType::Int32: {
            std::int32_t value = 0;
            if (!_reader.ReadLittleEndian(value)) {
                return DecodeStatus::MalformedPayload;
            }
            _value = value;
            return DecodeStatus::Ok;
        }
        case FieldType::UInt32: {
            std::uint32_t value = 0;
            if (!_reader.ReadLittleEndian(value)) {
                return DecodeStatus::MalformedPayload;
            }
            _value = value;
            return DecodeStatus::Ok;
        }
        case FieldType::Int64: {
            std::int64_t value = 0;
            if (!_reader.ReadLittleEndian(value)) {
                return DecodeStatus::MalformedPayload;
            }
            _value = value;
            return DecodeStatus::Ok;
        }
        case FieldType::UInt64: {
            std::uint64_t value = 0;
            if (!_reader.ReadLittleEndian(value)) {
                return DecodeStatus::MalformedPayload;
            }
            _value = value;
            return DecodeStatus::Ok;
        }
        case FieldType::Float32: {
            float value = 0.0f;
            if (!_reader.ReadFloat(value)) {
                return DecodeStatus::MalformedPayload;
            }
            _value = value;
            return DecodeStatus::Ok;
        }
        case FieldType::Float64: {
            double value = 0.0;
            if (!_reader.ReadFloat(value)) {
                return DecodeStatus::MalformedPayload;
            }
            _value = value;
            return DecodeStatus::Ok;
        }
        case FieldType::String: {
            ProfileString      value;
            const DecodeStatus status = _reader.ReadString(value, _limits);
            if (status != DecodeStatus::Ok) {
                return status;
            }
            _value = std::move(value);
            return DecodeStatus::Ok;
        }
    }
    return DecodeStatus::MalformedPayload;
}

} // namespace

std::uint64_t ComputeSchemaHash(const SchemaDescriptor& _schema) noexcept {
    if (!IsSchemaDescriptorHashable(_schema)) {
        return 0;
    }

    StableHasher hasher;
    hasher.AppendString(kSchemaHashDomain);
    hasher.AppendString(_schema.name);
    hasher.AppendString(_schema.event_type);
    hasher.AppendByte(static_cast<std::uint8_t>(_schema.kind));
    hasher.AppendByte(static_cast<std::uint8_t>(_schema.channel));
    hasher.AppendLittleEndian(_schema.schema_version);
    hasher.AppendLittleEndian(static_cast<std::uint32_t>(_schema.fields.size()));
    for (const SchemaField& field : _schema.fields) {
        hasher.AppendString(field.name);
        hasher.AppendByte(static_cast<std::uint8_t>(field.type));
    }
    return hasher.Finish();
}

EncodeStatus EncodeSchemaPayload(
    const SchemaDescriptor& _schema,
    const CodecLimits&      _limits,
    Array<std::uint8_t>&    _payload
) {
    _payload.clear();
    const EncodeStatus validation = ValidateSchemaForEncoding(_schema, _limits);
    if (validation != EncodeStatus::Ok) {
        return validation;
    }

    std::size_t encoded_size =
        sizeof(std::uint64_t) + sizeof(std::uint32_t) + sizeof(std::uint8_t) * 2 + sizeof(std::uint16_t);
    const auto include_string = [&](std::string_view text) {
        const std::size_t additional_size = sizeof(std::uint32_t) + text.size();
        if (!CanGrowPayload(encoded_size, additional_size, _limits)) {
            return false;
        }
        encoded_size += additional_size;
        return true;
    };
    if (!include_string(_schema.name) || !include_string(_schema.event_type)) {
        return EncodeStatus::PayloadTooLarge;
    }
    for (const SchemaField& field : _schema.fields) {
        if (!CanGrowPayload(encoded_size, sizeof(std::uint8_t), _limits)) {
            return EncodeStatus::PayloadTooLarge;
        }
        encoded_size += sizeof(std::uint8_t);
        if (!include_string(field.name)) {
            return EncodeStatus::PayloadTooLarge;
        }
    }

    Array<std::uint8_t> encoded;
    encoded.reserve(encoded_size);
    AppendLittleEndian(encoded, ComputeSchemaHash(_schema));
    AppendLittleEndian(encoded, _schema.schema_version);
    encoded.emplace_back(static_cast<std::uint8_t>(_schema.kind));
    encoded.emplace_back(static_cast<std::uint8_t>(_schema.channel));
    AppendLittleEndian(encoded, static_cast<std::uint16_t>(_schema.fields.size()));
    AppendString(encoded, _schema.name);
    AppendString(encoded, _schema.event_type);
    for (const SchemaField& field : _schema.fields) {
        encoded.emplace_back(static_cast<std::uint8_t>(field.type));
        AppendString(encoded, field.name);
    }

    if (encoded.size() > _limits.max_packet_payload_bytes || !CanEncodeU32Length(encoded.size())) {
        return EncodeStatus::PayloadTooLarge;
    }

    _payload = std::move(encoded);
    return EncodeStatus::Ok;
}

EncodeStatus EncodeRecordPayload(
    std::uint64_t                   _schema_hash,
    std::uint64_t                   _sequence,
    std::span<const SchemaField>    _fields,
    std::span<const FieldValueView> _values,
    const CodecLimits&              _limits,
    Array<std::uint8_t>&            _payload
) {
    _payload.clear();
    if (_schema_hash == 0) {
        return EncodeStatus::InvalidSchema;
    }
    if (_fields.size() != _values.size()) {
        return EncodeStatus::ValueCountMismatch;
    }
    if (_fields.size() > _limits.max_fields) {
        return EncodeStatus::PayloadTooLarge;
    }

    Array<std::uint8_t> encoded_values;
    encoded_values.reserve(_limits.max_packet_payload_bytes < 128 ? _limits.max_packet_payload_bytes : 128);
    for (std::size_t field_index = 0; field_index < _fields.size(); ++field_index) {
        if (!IsValidFieldType(_fields[field_index].type)) {
            return EncodeStatus::InvalidSchema;
        }
        std::size_t encoded_field_size = 0;
        switch (_fields[field_index].type) {
            case FieldType::Bool:
                encoded_field_size = sizeof(std::uint8_t);
                break;
            case FieldType::Int32:
            case FieldType::UInt32:
            case FieldType::Float32:
                encoded_field_size = sizeof(std::uint32_t);
                break;
            case FieldType::Int64:
            case FieldType::UInt64:
            case FieldType::Float64:
                encoded_field_size = sizeof(std::uint64_t);
                break;
            case FieldType::String:
                if (HoldsFieldValue<std::string_view>(_values[field_index])) {
                    const std::string_view text = std::get<std::string_view>(_values[field_index]);
                    if (!CanEncodeU32Length(text.size()) || text.size() > _limits.max_string_bytes) {
                        return EncodeStatus::StringTooLarge;
                    }
                    encoded_field_size = sizeof(std::uint32_t) + text.size();
                }
                break;
        }
        if (encoded_field_size != 0 && !CanGrowPayload(encoded_values.size(), encoded_field_size, _limits)) {
            return EncodeStatus::PayloadTooLarge;
        }
        const EncodeStatus status =
            AppendFieldValue(encoded_values, _fields[field_index], _values[field_index], _limits);
        if (status != EncodeStatus::Ok) {
            return status;
        }
        if (encoded_values.size() > _limits.max_packet_payload_bytes ||
            encoded_values.size() > std::numeric_limits<std::uint32_t>::max()) {
            return EncodeStatus::PayloadTooLarge;
        }
    }

    if (encoded_values.size() > std::numeric_limits<std::size_t>::max() - kRecordPrefixBytes ||
        encoded_values.size() + kRecordPrefixBytes > _limits.max_packet_payload_bytes) {
        return EncodeStatus::PayloadTooLarge;
    }

    Array<std::uint8_t> encoded;
    encoded.reserve(kRecordPrefixBytes + encoded_values.size());
    AppendLittleEndian(encoded, _schema_hash);
    AppendLittleEndian(encoded, _sequence);
    AppendLittleEndian(encoded, static_cast<std::uint32_t>(encoded_values.size()));
    encoded.insert(encoded.end(), encoded_values.begin(), encoded_values.end());
    _payload = std::move(encoded);
    return EncodeStatus::Ok;
}

void EncodeLossPayload(const LossNotice& _loss, Array<std::uint8_t>& _payload) {
    Array<std::uint8_t> encoded;
    encoded.reserve(kLossPayloadBytes);
    AppendLittleEndian(encoded, _loss.first_sequence);
    AppendLittleEndian(encoded, _loss.last_sequence);
    AppendLittleEndian(encoded, _loss.record_count);
    AppendLittleEndian(encoded, _loss.value_bytes);
    AppendLittleEndian(encoded, _loss.reason_mask);
    _payload = std::move(encoded);
}

void EncodeSessionBeginPayload(const SessionBeginInfo& _session, Array<std::uint8_t>& _payload) {
    Array<std::uint8_t> encoded;
    encoded.reserve(kSessionBeginPayloadBytes);
    AppendLittleEndian(encoded, _session.generation);
    AppendLittleEndian(encoded, _session.started_unix_ns);
    _payload = std::move(encoded);
}

void EncodeSessionEndPayload(const SessionEndInfo& _session, Array<std::uint8_t>& _payload) {
    Array<std::uint8_t> encoded;
    encoded.reserve(kSessionEndPayloadBytes);
    AppendLittleEndian(encoded, _session.generation);
    AppendLittleEndian(encoded, _session.records_written);
    AppendLittleEndian(encoded, _session.records_dropped);
    _payload = std::move(encoded);
}

EncodeStatus WrapPacket(
    PacketType                    _type,
    std::uint64_t                 _packet_index,
    std::span<const std::uint8_t> _payload,
    const CodecLimits&            _limits,
    Array<std::uint8_t>&          _packet
) {
    if (!IsValidPacketType(_type)) {
        _packet.clear();
        return EncodeStatus::InvalidSchema;
    }
    if (_payload.size() > _limits.max_packet_payload_bytes || !CanEncodeU32Length(_payload.size())) {
        _packet.clear();
        return EncodeStatus::PayloadTooLarge;
    }

    Array<std::uint8_t> encoded;
    encoded.reserve(kPacketHeaderBytes + _payload.size());
    AppendLittleEndian(encoded, kPacketMagic);
    AppendLittleEndian(encoded, kWireVersion);
    AppendLittleEndian(encoded, kPacketHeaderBytes);
    AppendLittleEndian(encoded, static_cast<std::uint16_t>(_type));
    AppendLittleEndian(encoded, std::uint16_t{0});
    AppendLittleEndian(encoded, static_cast<std::uint32_t>(_payload.size()));
    AppendLittleEndian(encoded, _packet_index);
    AppendLittleEndian(encoded, ComputeCrc32(_payload));
    AppendLittleEndian(encoded, ComputeCrc32(std::span<const std::uint8_t>(encoded.data(), kHeaderCrcBytes)));
    encoded.insert(encoded.end(), _payload.begin(), _payload.end());

    _packet = std::move(encoded);
    return EncodeStatus::Ok;
}

DecodeStatus DecodePacket(
    std::span<const std::uint8_t> _input,
    const CodecLimits&            _limits,
    PacketView&                   _packet,
    std::size_t&                  _consumed
) noexcept {
    _consumed = 0;
    _packet   = {};

    if (_input.size() < kPacketHeaderBytes) {
        return DecodeStatus::NeedMoreData;
    }

    ByteReader    reader(_input.first(kPacketHeaderBytes));
    PacketHeader  header{};
    std::uint16_t packet_type = 0;
    if (!reader.ReadLittleEndian(header.magic) || !reader.ReadLittleEndian(header.wire_version) ||
        !reader.ReadLittleEndian(header.header_bytes) || !reader.ReadLittleEndian(packet_type) ||
        !reader.ReadLittleEndian(header.flags) || !reader.ReadLittleEndian(header.payload_bytes) ||
        !reader.ReadLittleEndian(header.packet_index) || !reader.ReadLittleEndian(header.payload_crc32) ||
        !reader.ReadLittleEndian(header.header_crc32) || !reader.AtEnd()) {
        return DecodeStatus::InvalidHeader;
    }
    header.type = static_cast<PacketType>(packet_type);

    if (header.magic != kPacketMagic) {
        return DecodeStatus::InvalidMagic;
    }
    if (header.wire_version != kWireVersion) {
        return DecodeStatus::UnsupportedVersion;
    }
    if (header.header_bytes != kPacketHeaderBytes) {
        return DecodeStatus::InvalidHeader;
    }
    if (ComputeCrc32(_input.first(kHeaderCrcBytes)) != header.header_crc32) {
        return DecodeStatus::ChecksumMismatch;
    }
    if (!IsValidPacketType(header.type)) {
        return DecodeStatus::UnknownPacketType;
    }
    if (header.flags != 0) {
        return DecodeStatus::UnsupportedFlags;
    }
    if (header.payload_bytes > _limits.max_packet_payload_bytes) {
        return DecodeStatus::PayloadTooLarge;
    }

    const std::size_t payload_size = header.payload_bytes;
    if (payload_size > _input.size() - kPacketHeaderBytes) {
        return DecodeStatus::NeedMoreData;
    }

    const std::span<const std::uint8_t> payload = _input.subspan(kPacketHeaderBytes, payload_size);
    if (ComputeCrc32(payload) != header.payload_crc32) {
        return DecodeStatus::ChecksumMismatch;
    }

    _packet.header  = header;
    _packet.payload = payload;
    _consumed       = kPacketHeaderBytes + payload_size;
    return DecodeStatus::Ok;
}

DecodeStatus
DecodeSchemaPayload(const PacketView& _packet, const CodecLimits& _limits, SchemaDescriptor& _schema) {
    if (!ValidatePayloadView(_packet, PacketType::Schema)) {
        return DecodeStatus::MalformedPayload;
    }
    if (_packet.payload.size() > _limits.max_packet_payload_bytes) {
        return DecodeStatus::LimitExceeded;
    }

    ByteReader       reader(_packet.payload);
    std::uint64_t    encoded_hash    = 0;
    std::uint8_t     encoded_kind    = 0;
    std::uint8_t     encoded_channel = 0;
    std::uint16_t    field_count     = 0;
    SchemaDescriptor decoded{};
    if (!reader.ReadLittleEndian(encoded_hash) || !reader.ReadLittleEndian(decoded.schema_version) ||
        !reader.ReadLittleEndian(encoded_kind) || !reader.ReadLittleEndian(encoded_channel) ||
        !reader.ReadLittleEndian(field_count)) {
        return DecodeStatus::MalformedPayload;
    }
    if (field_count > _limits.max_fields) {
        return DecodeStatus::LimitExceeded;
    }

    decoded.kind    = static_cast<EventKind>(encoded_kind);
    decoded.channel = static_cast<Channel>(encoded_channel);
    if (!IsValidEventKind(decoded.kind) || !IsValidChannel(decoded.channel)) {
        return DecodeStatus::MalformedPayload;
    }

    DecodeStatus status = reader.ReadString(decoded.name, _limits);
    if (status != DecodeStatus::Ok) {
        return status;
    }
    status = reader.ReadString(decoded.event_type, _limits);
    if (status != DecodeStatus::Ok) {
        return status;
    }
    if (decoded.name.empty() || decoded.event_type.empty()) {
        return DecodeStatus::MalformedPayload;
    }

    decoded.fields.reserve(field_count);
    for (std::uint16_t field_index = 0; field_index < field_count; ++field_index) {
        std::uint8_t encoded_type = 0;
        SchemaField  field{};
        if (!reader.ReadLittleEndian(encoded_type)) {
            return DecodeStatus::MalformedPayload;
        }
        field.type = static_cast<FieldType>(encoded_type);
        if (!IsValidFieldType(field.type)) {
            return DecodeStatus::MalformedPayload;
        }
        status = reader.ReadString(field.name, _limits);
        if (status != DecodeStatus::Ok) {
            return status;
        }
        if (field.name.empty()) {
            return DecodeStatus::MalformedPayload;
        }
        for (const SchemaField& existing : decoded.fields) {
            if (existing.name == field.name) {
                return DecodeStatus::MalformedPayload;
            }
        }
        decoded.fields.emplace_back(std::move(field));
    }
    if (!reader.AtEnd()) {
        return DecodeStatus::MalformedPayload;
    }
    if (ComputeSchemaHash(decoded) != encoded_hash) {
        return DecodeStatus::SchemaHashMismatch;
    }

    _schema = std::move(decoded);
    return DecodeStatus::Ok;
}

DecodeStatus DecodeRecordPayload(
    const PacketView&       _packet,
    const SchemaDescriptor& _schema,
    const CodecLimits&      _limits,
    DecodedRecord&          _record
) {
    if (!ValidatePayloadView(_packet, PacketType::Record)) {
        return DecodeStatus::MalformedPayload;
    }
    if (_packet.payload.size() > _limits.max_packet_payload_bytes ||
        _schema.fields.size() > _limits.max_fields) {
        return DecodeStatus::LimitExceeded;
    }

    ByteReader    reader(_packet.payload);
    DecodedRecord decoded{};
    std::uint32_t value_bytes = 0;
    if (!reader.ReadLittleEndian(decoded.schema_hash) || !reader.ReadLittleEndian(decoded.sequence) ||
        !reader.ReadLittleEndian(value_bytes)) {
        return DecodeStatus::MalformedPayload;
    }
    if (decoded.schema_hash != ComputeSchemaHash(_schema)) {
        return DecodeStatus::UnknownSchema;
    }

    std::span<const std::uint8_t> encoded_values;
    if (!reader.ReadSpan(value_bytes, encoded_values) || !reader.AtEnd()) {
        return DecodeStatus::MalformedPayload;
    }

    ByteReader value_reader(encoded_values);
    decoded.values.reserve(_schema.fields.size());
    for (const SchemaField& field : _schema.fields) {
        if (!IsValidFieldType(field.type)) {
            return DecodeStatus::MalformedPayload;
        }
        FieldValue         value{};
        const DecodeStatus status = DecodeFieldValue(value_reader, field, _limits, value);
        if (status != DecodeStatus::Ok) {
            return status;
        }
        decoded.values.emplace_back(std::move(value));
    }
    if (!value_reader.AtEnd()) {
        return DecodeStatus::MalformedPayload;
    }

    _record = std::move(decoded);
    return DecodeStatus::Ok;
}

DecodeStatus DecodeLossPayload(const PacketView& _packet, LossNotice& _loss) noexcept {
    if (!ValidatePayloadView(_packet, PacketType::Loss) || _packet.payload.size() != kLossPayloadBytes) {
        return DecodeStatus::MalformedPayload;
    }

    ByteReader reader(_packet.payload);
    LossNotice decoded{};
    if (!reader.ReadLittleEndian(decoded.first_sequence) || !reader.ReadLittleEndian(decoded.last_sequence) ||
        !reader.ReadLittleEndian(decoded.record_count) || !reader.ReadLittleEndian(decoded.value_bytes) ||
        !reader.ReadLittleEndian(decoded.reason_mask) || !reader.AtEnd()) {
        return DecodeStatus::MalformedPayload;
    }

    constexpr std::uint32_t known_reason_mask = static_cast<std::uint32_t>(LossReason::Oversized) |
                                                static_cast<std::uint32_t>(LossReason::QueueFull) |
                                                static_cast<std::uint32_t>(LossReason::StaleGeneration) |
                                                static_cast<std::uint32_t>(LossReason::AfterShutdown) |
                                                static_cast<std::uint32_t>(LossReason::SinkFault);
    if (decoded.record_count == 0 || decoded.first_sequence > decoded.last_sequence ||
        decoded.record_count - 1 > decoded.last_sequence - decoded.first_sequence ||
        decoded.reason_mask == 0 || (decoded.reason_mask & ~known_reason_mask) != 0) {
        return DecodeStatus::MalformedPayload;
    }

    _loss = decoded;
    return DecodeStatus::Ok;
}

DecodeStatus DecodeSessionBeginPayload(const PacketView& _packet, SessionBeginInfo& _session) noexcept {
    if (!ValidatePayloadView(_packet, PacketType::SessionBegin) ||
        _packet.payload.size() != kSessionBeginPayloadBytes) {
        return DecodeStatus::MalformedPayload;
    }

    ByteReader       reader(_packet.payload);
    SessionBeginInfo decoded{};
    if (!reader.ReadLittleEndian(decoded.generation) || !reader.ReadLittleEndian(decoded.started_unix_ns) ||
        !reader.AtEnd()) {
        return DecodeStatus::MalformedPayload;
    }

    _session = decoded;
    return DecodeStatus::Ok;
}

DecodeStatus DecodeSessionEndPayload(const PacketView& _packet, SessionEndInfo& _session) noexcept {
    if (!ValidatePayloadView(_packet, PacketType::SessionEnd) ||
        _packet.payload.size() != kSessionEndPayloadBytes) {
        return DecodeStatus::MalformedPayload;
    }

    ByteReader     reader(_packet.payload);
    SessionEndInfo decoded{};
    if (!reader.ReadLittleEndian(decoded.generation) || !reader.ReadLittleEndian(decoded.records_written) ||
        !reader.ReadLittleEndian(decoded.records_dropped) || !reader.AtEnd()) {
        return DecodeStatus::MalformedPayload;
    }

    _session = decoded;
    return DecodeStatus::Ok;
}

} // namespace Moer::ProfileDump
