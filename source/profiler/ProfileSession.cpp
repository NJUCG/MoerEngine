#include "ProfileSession.h"

#include "profile/ProfileDumpTemplates.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace Moer::Profiler {
namespace {

const ProfileDump::DecodedValue* FindDecodedField(
    const ProfileDump::DecodedSchema& schema,
    const ProfileDump::DecodedRecord& record,
    std::string_view                  field_name,
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
    event.name = name->string_value;
    event.category = std::string(schema.event_type);
    event.track_name = std::string("CPU ") + std::to_string(event.track_id);
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
    event.track_id = static_cast<uint64_t>(std::hash<std::string>{}(queue_name->string_value));
    event.depth = depth->uint32_value;
    event.ts_begin_ns = start_ns->uint64_value;
    event.ts_end_ns = end_ns->uint64_value;
    event.name = name->string_value;
    event.category = std::string(schema.event_type);
    event.track_name = queue_name->string_value;
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

void ProfileStore::SetSessionName(std::string session_name) {
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
        auto& track = tracks[track_key];
        track.key = track_key;
        track.id = normalized_event.track_id;
        track.type = normalized_event.track_type;
        if (!normalized_event.track_name.empty()) {
            track.name = normalized_event.track_name;
        } else if (track.name == "Unknown") {
            track.name = std::string("Track ") + std::to_string(normalized_event.track_id);
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

bool LoadProfileDumpFile(const std::filesystem::path& path, ProfileStore& store, bool clear_before_load) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamsize file_size = file.tellg();
    if (file_size <= 0) {
        return false;
    }
    file.seekg(0, std::ios::beg);

    Array<uint8_t> bytes{};
    bytes.resize(static_cast<size_t>(file_size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), file_size)) {
        return false;
    }

    ProfileDumpSessionDecoder decoder{};
    Array<ProfileEvent> raw_events{};
    if (!decoder.DecodePayload(bytes, raw_events)) {
        return false;
    }

    if (clear_before_load) {
        store.Reset();
    }
    store.SetSessionName(path.filename().string());
    store.AppendEvents(raw_events);
    return true;
}

} // namespace Moer::Profiler