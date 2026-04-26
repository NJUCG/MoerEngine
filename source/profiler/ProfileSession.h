#pragma once

#include "misc/STL.h"
#include "profile/ProfileDump.h"
#include "string/String.h"
#include "trace/Trace.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <span>

namespace Moer::Profiler {

enum class ProfileTrackType : uint8_t {
    CPUThread = 0,
    GPUQueue,
};

enum class ProfileEventType : uint8_t {
    Scope = 0,
    Counter,
    Instant,
};

struct ProfileSessionMetadata {
    Utf8String session_name{};
    uint64_t    time_origin_ns{0};
    bool        has_time_origin = false;
};

struct ProfileEvent {
    uint64_t         event_id{0};
    uint64_t         session_id{0};
    ProfileEventType type{ProfileEventType::Scope};
    ProfileTrackType track_type{ProfileTrackType::CPUThread};
    uint64_t         track_id{0};
    uint32_t         depth{0};
    uint64_t         ts_begin_ns{0};
    uint64_t         ts_end_ns{0};
    double           counter_value{0.0};
    Utf8String       name{};
    Utf8String       category{};
    Utf8String       track_name{};
    Utf8String       args{};
};

struct TrackInfo {
    uint64_t         key{0};
    ProfileTrackType type{ProfileTrackType::CPUThread};
    uint64_t         id{0};
    Utf8String       name{"Unknown"};
    int              max_depth{0};
};

uint64_t MakeTrackKey(ProfileTrackType type, uint64_t id);

struct ProfileStore {
    std::mutex                        mutex{};
    ProfileSessionMetadata            metadata{};
    Array<ProfileEvent>               events{};
    UnorderedMap<uint64_t, TrackInfo> tracks{};
    uint64_t                          min_ts{0};
    uint64_t                          max_ts{0};
    uint64_t                          generation{1};

    void Reset();
    void SetSessionName(Utf8String session_name);
    void AppendEvents(const Array<ProfileEvent>& raw_events, std::optional<uint64_t> source_time_origin_ns = std::nullopt);
};

class ProfileDumpSessionDecoder {
public:
    void Reset();

    bool ConsumePacket(
        const ProfileDump::PacketHeader& header,
        std::span<const uint8_t>         payload,
        Array<ProfileEvent>&             out_events
    );

    bool DecodePayload(std::span<const uint8_t> bytes, Array<ProfileEvent>& out_events);

private:
    UnorderedMap<uint32_t, ProfileDump::DecodedSchema> m_schemas{};
};

class TraceSessionDecoder {
public:
    void Reset();

    bool ConsumePacket(
        const Trace::PacketHeader& header,
        std::span<const uint8_t>   payload,
        Array<ProfileEvent>&       out_events,
        Utf8String*                out_session_name = nullptr
    );

    bool DecodePayload(
        std::span<const uint8_t> bytes,
        Array<ProfileEvent>&     out_events,
        Utf8String*              out_session_name = nullptr
    );

    std::optional<uint64_t> TimeOriginNs() const;

private:
    uint64_t                m_session_id{0};
    std::optional<uint64_t> m_time_origin_ns{};
};

bool LoadProfileDumpFile(Utf8StringView path, ProfileStore& store, bool clear_before_load);
bool LoadTraceFile(Utf8StringView path, ProfileStore& store, bool clear_before_load);
bool LoadProfilerCaptureFile(Utf8StringView path, ProfileStore& store, bool clear_before_load);

} // namespace Moer::Profiler