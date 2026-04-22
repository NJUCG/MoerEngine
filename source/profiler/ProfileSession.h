#pragma once

#include "misc/STL.h"
#include "profile/ProfileDump.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <span>
#include <string>

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
    std::string session_name{};
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
    std::string      name{};
    std::string      category{};
    std::string      track_name{};
    std::string      args{};
};

struct TrackInfo {
    uint64_t         key{0};
    ProfileTrackType type{ProfileTrackType::CPUThread};
    uint64_t         id{0};
    std::string      name{"Unknown"};
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
    void SetSessionName(std::string session_name);
    void AppendEvents(const Array<ProfileEvent>& raw_events);
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

bool LoadProfileDumpFile(const std::filesystem::path& path, ProfileStore& store, bool clear_before_load);

} // namespace Moer::Profiler