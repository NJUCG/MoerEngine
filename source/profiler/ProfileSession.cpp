#include "ProfileSession.h"

#include "file/File.h"
#include "profile/ProfileDumpTemplates.h"
#include "string/StringConvert.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace Moer::Profiler {
namespace {

static constexpr uint32_t trace_packet_magic = 0x4D525443u;
static constexpr uint16_t trace_packet_version = 1;
static constexpr uint16_t trace_packet_type_metadata = 1;
static constexpr uint16_t trace_packet_type_events = 2;

Utf8String ToProfilerString(Utf8StringView text) {
    return Utf8String(text);
}

Utf8String ToProfilerString(const std::string& text) {
    return Utf8String(Utf8String::native_view_type(text.data(), text.size()));
}

Utf8String ToProfilerString(const char* text) {
    return Utf8String(text ? text : "");
}

std::filesystem::path ToLocalPath(Utf8StringView path) {
#if defined(_WIN32) || defined(_WIN64)
    const WideString wide_path = Utf8ToWide(path);
    return std::filesystem::path(std::wstring_view(wide_path.data(), wide_path.size()));
#else
    return std::filesystem::path(std::string_view(path.data(), path.size()));
#endif
}

Utf8String PathFilenameToProfilerString(Utf8StringView path_text) {
    const std::filesystem::path path = ToLocalPath(path_text);
    const std::filesystem::path filename = path.filename();
#if defined(_WIN32) || defined(_WIN64)
    const std::wstring& native = filename.native();
    return WideToUtf8(WideStringView(native.data(), native.size()));
#else
    const std::string& native = filename.native();
    return ToProfilerString(native);
#endif
}

uint64_t HashUtf8(Utf8StringView text) {
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < text.size(); ++i) {
        hash ^= static_cast<uint8_t>(text[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

Utf8String MakeCpuTrackName(uint64_t track_id) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "CPU %llu", static_cast<unsigned long long>(track_id));
    return ToProfilerString(buffer);
}

Utf8String MakeFallbackTrackName(uint64_t track_id) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "Track %llu", static_cast<unsigned long long>(track_id));
    return ToProfilerString(buffer);
}

uint32_t HashBytes(const uint8_t* data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

template<typename T>
bool ReadPod(std::span<const uint8_t> payload, size_t& offset, T& out) {
    if (offset + sizeof(T) > payload.size()) {
        return false;
    }
    std::memcpy(&out, payload.data() + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

bool ReadTraceString(std::span<const uint8_t> payload, size_t& offset, Utf8String& out) {
    uint16_t len = 0;
    if (!ReadPod(payload, offset, len)) {
        return false;
    }
    if (offset + len > payload.size()) {
        return false;
    }
    out = Utf8String(Utf8String::native_view_type(reinterpret_cast<const char*>(payload.data() + offset), len));
    offset += len;
    return true;
}

Array<std::string_view> SplitCsvLine(std::string_view line) {
    Array<std::string_view> fields{};
    size_t                  field_start = 0;
    bool                    quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (c == ',' && !quoted) {
            fields.emplace_back(line.data() + field_start, i - field_start);
            field_start = i + 1;
        }
    }
    fields.emplace_back(line.data() + field_start, line.size() - field_start);
    return fields;
}

std::string_view TrimCsvField(std::string_view field) {
    while (!field.empty() && (field.front() == ' ' || field.front() == '\t' || field.front() == '\r')) {
        field.remove_prefix(1);
    }
    while (!field.empty() && (field.back() == ' ' || field.back() == '\t' || field.back() == '\r')) {
        field.remove_suffix(1);
    }
    if (field.size() >= 2 && field.front() == '"' && field.back() == '"') {
        field.remove_prefix(1);
        field.remove_suffix(1);
    }
    return field;
}

bool ParseUint64(std::string_view text, uint64_t& out) {
    const std::string value(text);
    char*             end = nullptr;
    out = std::strtoull(value.c_str(), &end, 10);
    return end == value.c_str() + value.size();
}

bool ParseUint32(std::string_view text, uint32_t& out) {
    uint64_t value = 0;
    if (!ParseUint64(text, value) || value > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

bool ParseDouble(std::string_view text, double& out) {
    const std::string value(text);
    char*             end = nullptr;
    out = std::strtod(value.c_str(), &end);
    return end == value.c_str() + value.size();
}

bool DecodeTraceCsvRow(std::string_view line, ProfileEvent& event) {
    Array<std::string_view> fields = SplitCsvLine(line);
    if (fields.size() != 13) {
        return false;
    }
    for (std::string_view& field : fields) {
        field = TrimCsvField(field);
    }

    uint32_t type = 0;
    uint32_t track_type = 0;
    if (!ParseUint64(fields[0], event.event_id) || !ParseUint64(fields[1], event.session_id) ||
        !ParseUint32(fields[2], type) || !ParseUint32(fields[3], track_type) ||
        !ParseUint64(fields[4], event.track_id) || !ParseUint32(fields[5], event.depth) ||
        !ParseUint64(fields[6], event.ts_begin_ns) || !ParseUint64(fields[7], event.ts_end_ns) ||
        !ParseDouble(fields[8], event.counter_value)) {
        return false;
    }

    switch (static_cast<Trace::EventType>(type)) {
        case Trace::EventType::Scope:
            event.type = ProfileEventType::Scope;
            break;
        case Trace::EventType::Counter:
            event.type = ProfileEventType::Counter;
            break;
        case Trace::EventType::Instant:
            event.type = ProfileEventType::Instant;
            break;
        case Trace::EventType::Meta:
            return false;
        default:
            return false;
    }
    switch (static_cast<Trace::TrackType>(track_type)) {
        case Trace::TrackType::CPUThread:
            event.track_type = ProfileTrackType::CPUThread;
            break;
        case Trace::TrackType::GPUQueue:
            event.track_type = ProfileTrackType::GPUQueue;
            break;
        default:
            return false;
    }
    if (event.ts_end_ns < event.ts_begin_ns) {
        return false;
    }
    event.name = Utf8String(Utf8String::native_view_type(fields[9].data(), fields[9].size()));
    event.category = Utf8String(Utf8String::native_view_type(fields[10].data(), fields[10].size()));
    event.track_name = Utf8String(Utf8String::native_view_type(fields[11].data(), fields[11].size()));
    event.args = Utf8String(Utf8String::native_view_type(fields[12].data(), fields[12].size()));
    if (event.track_name.empty()) {
        event.track_name = event.track_type == ProfileTrackType::CPUThread ? MakeCpuTrackName(event.track_id) :
                                                                            MakeFallbackTrackName(event.track_id);
    }
    return true;
}

const ProfileDump::DecodedValue* FindDecodedField(
    const ProfileDump::DecodedSchema& schema,
    const ProfileDump::DecodedRecord& record,
    const char*                       field_name,
    ProfileDump::EFieldType           expected_type
) {
    if (schema.fields.size() != record.fields.size()) {
        return nullptr;
    }

    for (size_t field_index = 0; field_index < schema.fields.size(); ++field_index) {
        const ProfileDump::DecodedFieldDesc& field = schema.fields[field_index];
        if (field.name != field_name) {
            continue;
        }

        const ProfileDump::DecodedValue& value = record.fields[field_index];
        if (value.type != expected_type) {
            return nullptr;
        }
        return &value;
    }

    return nullptr;
}

bool DecodeCpuScopeRecord(
    const ProfileDump::DecodedSchema& schema,
    const ProfileDump::DecodedRecord& record,
    ProfileEvent&                     event
) {
    const auto* thread_id = FindDecodedField(schema, record, "thread_id", ProfileDump::EFieldType::UInt64);
    const auto* name = FindDecodedField(schema, record, "name", ProfileDump::EFieldType::String);
    const auto* start_us = FindDecodedField(schema, record, "start_us", ProfileDump::EFieldType::Int64);
    const auto* duration_us = FindDecodedField(schema, record, "duration_us", ProfileDump::EFieldType::Int64);
    const auto* depth = FindDecodedField(schema, record, "depth", ProfileDump::EFieldType::UInt32);
    if (thread_id == nullptr || name == nullptr || start_us == nullptr || duration_us == nullptr || depth == nullptr) {
        return false;
    }
    if (start_us->int64_value < 0 || duration_us->int64_value < 0) {
        return false;
    }

    event = {};
    event.event_id = record.sequence;
    event.type = ProfileEventType::Scope;
    event.track_type = ProfileTrackType::CPUThread;
    event.track_id = thread_id->uint64_value;
    event.depth = depth->uint32_value;
    event.ts_begin_ns = static_cast<uint64_t>(start_us->int64_value) * 1000ull;
    event.ts_end_ns = static_cast<uint64_t>(start_us->int64_value + duration_us->int64_value) * 1000ull;
    event.name = ToProfilerString(name->string_value);
    event.category = ToProfilerString(schema.event_type);
    event.track_name = MakeCpuTrackName(event.track_id);
    return true;
}

bool DecodeGpuScopeRecord(
    const ProfileDump::DecodedSchema& schema,
    const ProfileDump::DecodedRecord& record,
    ProfileEvent&                     event
) {
    const auto* frame_index = FindDecodedField(schema, record, "frame_index", ProfileDump::EFieldType::UInt64);
    const auto* queue_name = FindDecodedField(schema, record, "queue_name", ProfileDump::EFieldType::String);
    const auto* name = FindDecodedField(schema, record, "name", ProfileDump::EFieldType::String);
    const auto* start_ns = FindDecodedField(schema, record, "start_ns", ProfileDump::EFieldType::UInt64);
    const auto* end_ns = FindDecodedField(schema, record, "end_ns", ProfileDump::EFieldType::UInt64);
    const auto* depth = FindDecodedField(schema, record, "depth", ProfileDump::EFieldType::UInt32);
    if (frame_index == nullptr || queue_name == nullptr || name == nullptr ||
        start_ns == nullptr || end_ns == nullptr || depth == nullptr) {
        return false;
    }
    if (end_ns->uint64_value < start_ns->uint64_value) {
        return false;
    }

    event = {};
    event.event_id = record.sequence;
    event.session_id = frame_index->uint64_value;
    event.type = ProfileEventType::Scope;
    event.track_type = ProfileTrackType::GPUQueue;
    event.track_id = HashUtf8(ToProfilerString(queue_name->string_value));
    event.depth = depth->uint32_value;
    event.ts_begin_ns = start_ns->uint64_value;
    event.ts_end_ns = end_ns->uint64_value;
    event.name = ToProfilerString(name->string_value);
    event.category = ToProfilerString(schema.event_type);
    event.track_name = ToProfilerString(queue_name->string_value);
    return true;
}

bool DecodeProfileDumpRecord(
    const ProfileDump::DecodedSchema& schema,
    const ProfileDump::DecodedRecord& record,
    ProfileEvent&                     event
) {
    if (schema.event_type == ProfileDump::Templates::CpuScopeTemplate::kEventType) {
        return DecodeCpuScopeRecord(schema, record, event);
    }
    if (schema.event_type == ProfileDump::Templates::GpuScopeTemplate::kEventType) {
        return DecodeGpuScopeRecord(schema, record, event);
    }
    return false;
}

void ShiftExistingEvents(Array<ProfileEvent>& events, uint64_t delta_ns) {
    if (delta_ns == 0) {
        return;
    }

    for (ProfileEvent& event : events) {
        event.ts_begin_ns += delta_ns;
        event.ts_end_ns += delta_ns;
    }
}

} // namespace

uint64_t MakeTrackKey(ProfileTrackType type, uint64_t id) {
    return (uint64_t(static_cast<uint8_t>(type)) << 56u) ^ id;
}

void ProfileStore::Reset() {
    std::lock_guard<std::mutex> lock(mutex);
    metadata = {};
    events.clear();
    tracks.clear();
    min_ts = 0;
    max_ts = 0;
    ++generation;
}

void ProfileStore::SetSessionName(Utf8String session_name) {
    std::lock_guard<std::mutex> lock(mutex);
    metadata.session_name = std::move(session_name);
    ++generation;
}

void ProfileStore::AppendEvents(const Array<ProfileEvent>& raw_events) {
    if (raw_events.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex);

    uint64_t incoming_origin = UINT64_MAX;
    for (const ProfileEvent& raw_event : raw_events) {
        incoming_origin = std::min(incoming_origin, raw_event.ts_begin_ns);
    }
    if (incoming_origin == UINT64_MAX) {
        return;
    }

    if (!metadata.has_time_origin) {
        metadata.time_origin_ns = incoming_origin;
        metadata.has_time_origin = true;
    } else if (incoming_origin < metadata.time_origin_ns) {
        const uint64_t shift_delta = metadata.time_origin_ns - incoming_origin;
        ShiftExistingEvents(events, shift_delta);
        metadata.time_origin_ns = incoming_origin;
        max_ts += shift_delta;
    }

    events.reserve(events.size() + raw_events.size());
    for (const ProfileEvent& raw_event : raw_events) {
        ProfileEvent normalized_event = raw_event;
        normalized_event.ts_begin_ns -= metadata.time_origin_ns;
        normalized_event.ts_end_ns -= metadata.time_origin_ns;
        events.emplace_back(normalized_event);

        if (events.size() == 1) {
            min_ts = normalized_event.ts_begin_ns;
            max_ts = normalized_event.ts_end_ns;
        } else {
            min_ts = std::min(min_ts, normalized_event.ts_begin_ns);
            max_ts = std::max(max_ts, normalized_event.ts_end_ns);
        }

        const uint64_t track_key = MakeTrackKey(normalized_event.track_type, normalized_event.track_id);
        auto&          track = tracks[track_key];
        track.key = track_key;
        track.id = normalized_event.track_id;
        track.type = normalized_event.track_type;
        if (!normalized_event.track_name.empty()) {
            track.name = normalized_event.track_name;
        } else if (Utf8StringView(track.name) == Utf8StringView("Unknown")) {
            track.name = MakeFallbackTrackName(normalized_event.track_id);
        }
        track.max_depth = std::max(track.max_depth, static_cast<int>(normalized_event.depth));
    }

    if (!events.empty()) {
        min_ts = 0;
    }

    ++generation;
}

void ProfileDumpSessionDecoder::Reset() {
    m_schemas.clear();
}

bool ProfileDumpSessionDecoder::ConsumePacket(
    const ProfileDump::PacketHeader& header,
    std::span<const uint8_t>         payload,
    Array<ProfileEvent>&             out_events
) {
    out_events.clear();

    if (header.type == ProfileDump::EPacketType::Schema) {
        ProfileDump::DecodedSchema schema{};
        if (!ProfileDump::DeserializeSchemaPacket(header, payload, schema)) {
            return false;
        }
        m_schemas[schema.schema_id] = std::move(schema);
        return true;
    }

    if (header.type != ProfileDump::EPacketType::Record || payload.size() < sizeof(uint32_t)) {
        return false;
    }

    uint32_t schema_id = 0;
    std::memcpy(&schema_id, payload.data(), sizeof(schema_id));
    const auto schema_it = m_schemas.find(schema_id);
    if (schema_it == m_schemas.end()) {
        return false;
    }

    ProfileDump::DecodedRecord record{};
    if (!ProfileDump::DeserializeRecordPacket(header, payload, schema_it->second, record)) {
        return false;
    }

    ProfileEvent event{};
    if (!DecodeProfileDumpRecord(schema_it->second, record, event)) {
        return true;
    }

    out_events.emplace_back(std::move(event));
    return true;
}

bool ProfileDumpSessionDecoder::DecodePayload(std::span<const uint8_t> bytes, Array<ProfileEvent>& out_events) {
    Reset();
    out_events.clear();

    size_t offset = 0;
    while (offset < bytes.size()) {
        ProfileDump::PacketHeader header{};
        std::span<const uint8_t> payload{};
        if (!ProfileDump::ReadNextPacket(bytes, offset, header, payload)) {
            return false;
        }

        Array<ProfileEvent> packet_events{};
        if (!ConsumePacket(header, payload, packet_events)) {
            return false;
        }
        out_events.insert(out_events.end(), packet_events.begin(), packet_events.end());
    }

    return true;
}

void TraceSessionDecoder::Reset() {
    m_session_id = 0;
}

bool TraceSessionDecoder::ConsumePacket(
    const Trace::PacketHeader& header,
    std::span<const uint8_t>   payload,
    Array<ProfileEvent>&       out_events,
    Utf8String*                out_session_name
) {
    out_events.clear();
    if (header.magic != trace_packet_magic || header.version != trace_packet_version) {
        return false;
    }
    if (header.payload_size != payload.size() || HashBytes(payload.data(), payload.size()) != header.checksum) {
        return false;
    }

    size_t offset = 0;
    if (header.type == trace_packet_type_metadata) {
        uint64_t start_ts_ns = 0;
        Utf8String session_name{};
        if (!ReadPod(payload, offset, m_session_id) || !ReadPod(payload, offset, start_ts_ns) ||
            !ReadTraceString(payload, offset, session_name)) {
            return false;
        }
        (void)start_ts_ns;
        if (out_session_name) {
            *out_session_name = std::move(session_name);
        }
        return offset == payload.size();
    }

    if (header.type != trace_packet_type_events) {
        return false;
    }

    uint32_t count = 0;
    if (!ReadPod(payload, offset, count)) {
        return false;
    }
    out_events.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        ProfileEvent event{};
        uint8_t type = 0;
        uint8_t track_type = 0;
        if (!ReadPod(payload, offset, event.event_id) || !ReadPod(payload, offset, event.session_id) ||
            !ReadPod(payload, offset, type) || !ReadPod(payload, offset, track_type) ||
            !ReadPod(payload, offset, event.track_id) || !ReadPod(payload, offset, event.depth) ||
            !ReadPod(payload, offset, event.ts_begin_ns) || !ReadPod(payload, offset, event.ts_end_ns) ||
            !ReadPod(payload, offset, event.counter_value) || !ReadTraceString(payload, offset, event.name) ||
            !ReadTraceString(payload, offset, event.category) || !ReadTraceString(payload, offset, event.track_name) ||
            !ReadTraceString(payload, offset, event.args)) {
            return false;
        }

        switch (static_cast<Trace::EventType>(type)) {
            case Trace::EventType::Scope:
                event.type = ProfileEventType::Scope;
                break;
            case Trace::EventType::Counter:
                event.type = ProfileEventType::Counter;
                break;
            case Trace::EventType::Instant:
                event.type = ProfileEventType::Instant;
                break;
            case Trace::EventType::Meta:
                continue;
            default:
                return false;
        }
        switch (static_cast<Trace::TrackType>(track_type)) {
            case Trace::TrackType::CPUThread:
                event.track_type = ProfileTrackType::CPUThread;
                break;
            case Trace::TrackType::GPUQueue:
                event.track_type = ProfileTrackType::GPUQueue;
                break;
            default:
                return false;
        }
        if (event.track_name.empty()) {
            event.track_name = event.track_type == ProfileTrackType::CPUThread ? MakeCpuTrackName(event.track_id) :
                                                                                MakeFallbackTrackName(event.track_id);
        }
        if (event.ts_end_ns < event.ts_begin_ns) {
            return false;
        }
        out_events.emplace_back(std::move(event));
    }
    return offset == payload.size();
}

bool TraceSessionDecoder::DecodePayload(
    std::span<const uint8_t> bytes,
    Array<ProfileEvent>&     out_events,
    Utf8String*              out_session_name
) {
    Reset();
    out_events.clear();

    size_t offset = 0;
    while (offset < bytes.size()) {
        Trace::PacketHeader header{};
        if (offset + sizeof(header) > bytes.size()) {
            return false;
        }
        std::memcpy(&header, bytes.data() + offset, sizeof(header));
        offset += sizeof(header);
        if (offset + header.payload_size > bytes.size()) {
            return false;
        }
        std::span<const uint8_t> payload(bytes.data() + offset, header.payload_size);
        offset += header.payload_size;

        Array<ProfileEvent> packet_events{};
        Utf8String packet_session_name{};
        if (!ConsumePacket(header, payload, packet_events, &packet_session_name)) {
            return false;
        }
        if (out_session_name && !packet_session_name.empty()) {
            *out_session_name = std::move(packet_session_name);
        }
        out_events.insert(out_events.end(), packet_events.begin(), packet_events.end());
    }

    return true;
}

std::span<const uint8_t> AsBytes(std::span<const std::byte> data) {
    return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

bool DecodeProfileDumpCapture(std::span<const uint8_t> bytes, Utf8StringView path, ProfileStore& store, bool clear_before_load) {
    if (bytes.empty()) {
        return false;
    }

    ProfileDumpSessionDecoder decoder{};
    Array<ProfileEvent>       raw_events{};
    if (!decoder.DecodePayload(bytes, raw_events)) {
        return false;
    }

    if (clear_before_load) {
        store.Reset();
    }
    store.SetSessionName(PathFilenameToProfilerString(path));
    store.AppendEvents(raw_events);
    return true;
}

bool DecodeTraceCapture(std::span<const uint8_t> bytes, Utf8StringView path, ProfileStore& store, bool clear_before_load) {
    if (bytes.empty()) {
        return false;
    }

    TraceSessionDecoder decoder{};
    Array<ProfileEvent> raw_events{};
    Utf8String          session_name{};
    if (!decoder.DecodePayload(bytes, raw_events, &session_name)) {
        return false;
    }

    if (clear_before_load) {
        store.Reset();
    }
    store.SetSessionName(session_name.empty() ? PathFilenameToProfilerString(path) : std::move(session_name));
    store.AppendEvents(raw_events);
    return true;
}

bool DecodeTraceCsvCapture(std::span<const uint8_t> bytes, Utf8StringView path, ProfileStore& store, bool clear_before_load) {
    if (bytes.empty()) {
        return false;
    }

    std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Array<ProfileEvent> raw_events{};
    size_t              cursor = 0;
    bool                consumed_header = false;
    while (cursor <= text.size()) {
        const size_t line_end = text.find('\n', cursor);
        std::string_view line = line_end == std::string_view::npos ? text.substr(cursor) : text.substr(cursor, line_end - cursor);
        cursor = line_end == std::string_view::npos ? text.size() + 1 : line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (!consumed_header) {
            consumed_header = true;
            if (line.starts_with("event_id,")) {
                continue;
            }
        }

        ProfileEvent event{};
        if (!DecodeTraceCsvRow(line, event)) {
            return false;
        }
        raw_events.emplace_back(std::move(event));
    }

    if (raw_events.empty()) {
        return false;
    }
    if (clear_before_load) {
        store.Reset();
    }
    store.SetSessionName(PathFilenameToProfilerString(path));
    store.AppendEvents(raw_events);
    return true;
}

struct LoadCaptureContext {
    ProfileStore*  store{nullptr};
    Utf8StringView path{};
    bool           clear_before_load{false};
    bool           loaded{false};
};

bool LoadProfileDumpFile(Utf8StringView path, ProfileStore& store, bool clear_before_load) {
    LoadCaptureContext context{.store = &store, .path = path, .clear_before_load = clear_before_load};
    const File::EReadFileStatus status = File::ReadBinaryFile(File::ReadBinaryRequest{
        .path = path,
        .callback = [](std::span<const std::byte> data, void* user_data) {
            auto& context = *static_cast<LoadCaptureContext*>(user_data);
            context.loaded = DecodeProfileDumpCapture(
                AsBytes(data),
                context.path,
                *context.store,
                context.clear_before_load
            );
        },
        .user_data = &context,
    });
    return status == File::EReadFileStatus::Success && context.loaded;
}

bool LoadTraceFile(Utf8StringView path, ProfileStore& store, bool clear_before_load) {
    LoadCaptureContext context{.store = &store, .path = path, .clear_before_load = clear_before_load};
    const File::EReadFileStatus status = File::ReadBinaryFile(File::ReadBinaryRequest{
        .path = path,
        .callback = [](std::span<const std::byte> data, void* user_data) {
            auto& context = *static_cast<LoadCaptureContext*>(user_data);
            const std::span<const uint8_t> bytes = AsBytes(data);
            context.loaded = DecodeTraceCapture(bytes, context.path, *context.store, context.clear_before_load) ||
                             DecodeTraceCsvCapture(bytes, context.path, *context.store, context.clear_before_load);
        },
        .user_data = &context,
    });
    return status == File::EReadFileStatus::Success && context.loaded;
}

bool LoadProfilerCaptureFile(Utf8StringView path, ProfileStore& store, bool clear_before_load) {
    LoadCaptureContext context{.store = &store, .path = path, .clear_before_load = clear_before_load};
    const File::EReadFileStatus status = File::ReadBinaryFile(File::ReadBinaryRequest{
        .path = path,
        .callback = [](std::span<const std::byte> data, void* user_data) {
            auto& context = *static_cast<LoadCaptureContext*>(user_data);
            const std::span<const uint8_t> bytes = AsBytes(data);
            if (bytes.size() < sizeof(uint32_t)) {
                return;
            }

            uint32_t magic = 0;
            std::memcpy(&magic, bytes.data(), sizeof(magic));
            if (magic == ProfileDump::packet_magic) {
                context.loaded = DecodeProfileDumpCapture(
                    bytes,
                    context.path,
                    *context.store,
                    context.clear_before_load
                );
            } else if (magic == trace_packet_magic) {
                context.loaded = DecodeTraceCapture(bytes, context.path, *context.store, context.clear_before_load);
            } else {
                context.loaded = DecodeTraceCsvCapture(bytes, context.path, *context.store, context.clear_before_load);
            }
        },
        .user_data = &context,
    });
    return status == File::EReadFileStatus::Success && context.loaded;
}

} // namespace Moer::Profiler