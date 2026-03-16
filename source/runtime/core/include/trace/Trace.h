#pragma once

#include "API_Macro.h"
#include "misc/STL.h"

#include <atomic>
#include <cstdint>
#include <span>
#include <string_view>

#ifndef MOER_TRACE_ENABLED
#if defined(NDEBUG)
#define MOER_TRACE_ENABLED 0
#else
#define MOER_TRACE_ENABLED 1
#endif
#endif

#ifndef MOER_TRACE_GPU_ENABLED
#define MOER_TRACE_GPU_ENABLED 1
#endif

#ifndef MOER_TRACE_CSV_ENABLED
#define MOER_TRACE_CSV_ENABLED 1
#endif

namespace Moer::Trace {

enum class TrackType : uint8_t {
    CPUThread = 0,
    GPUQueue  = 1
};

enum class EventType : uint8_t {
    Scope   = 0,
    Instant = 1,
    Counter = 2,
    Meta    = 3
};

struct Config {
    bool        enable_streaming = true;
    bool        enable_csv       = false;
    bool        start_recording  = false;
    std::string csv_path         = {};

    std::string host        = "127.0.0.1";
    uint16_t    port        = 19090;
    uint32_t    queue_limit = 1u << 15;

    std::string session_name = "MoerEditor";
};

struct SpanDesc {
    std::string_view name{};
    std::string_view category{"Default"};
    TrackType        track_type{TrackType::CPUThread};
    uint64_t         track_id{0};
    uint32_t         depth{0};
    std::string_view track_name{};
    std::string_view args{};
};

struct EmitScopeDesc {
    std::string_view name{};
    std::string_view category{"Default"};
    TrackType        track_type{TrackType::CPUThread};
    uint64_t         track_id{0};
    uint32_t         depth{0};
    uint64_t         ts_begin_ns{0};
    uint64_t         ts_end_ns{0};
    std::string_view track_name{};
    std::string_view args{};
};

struct SpanToken {
    bool     valid{false};
    uint64_t span_id{0};
};

struct Stats {
    bool     enabled{false};
    bool     recording{false};
    bool     connected{false};
    uint64_t dropped_events{0};
    uint64_t queued_events{0};
};

struct TraceEvent {
    uint64_t   event_id{0};
    uint64_t   session_id{0};
    EventType  type{EventType::Scope};
    TrackType  track_type{TrackType::CPUThread};
    uint64_t   track_id{0};
    uint32_t   depth{0};
    uint64_t   ts_begin_ns{0};
    uint64_t   ts_end_ns{0};
    double     counter_value{0.0};
    std::string name{};
    std::string category{};
    std::string track_name{};
    std::string args{};
};

struct PacketHeader {
    uint32_t magic{0x4D525443}; // 'MRTC'
    uint16_t version{1};
    uint16_t type{0}; // 1: metadata, 2: events
    uint32_t payload_size{0};
    uint32_t checksum{0};
};

struct SessionMetadata {
    uint64_t    session_id{0};
    std::string session_name{};
    uint64_t    start_ts_ns{0};
};

CORE_API bool Init(const Config& config = {});
CORE_API void Shutdown();
CORE_API void StartRecording();
CORE_API void StopRecording();
CORE_API bool IsRecording();

CORE_API void SetThreadName(std::string_view thread_name);
CORE_API uint64_t DefaultCpuTrackId();
CORE_API uint64_t MakeGpuQueueTrackId(uint32_t gpu_index, uint32_t queue_type);

CORE_API SpanToken BeginSpan(const SpanDesc& desc);
CORE_API void      EndSpan(SpanToken&& token, std::string_view end_args = {});
CORE_API void      EmitScope(const EmitScopeDesc& desc);
CORE_API void      EmitInstant(
         std::string_view name,
         std::string_view category = "Default",
         std::string_view args     = {}
     );
CORE_API void EmitCounter(
    std::string_view name,
    double           value,
    std::string_view category = "Counter",
    std::string_view args     = {}
);
CORE_API void EnableCsvExport(std::string_view csv_path);
CORE_API Stats GetStats();

CORE_API bool SerializeSessionMetadataPacket(
    const SessionMetadata& metadata,
    Array<std::byte>&      out_packet
);
CORE_API bool SerializeEventsPacket(const Array<TraceEvent>& events, Array<std::byte>& out_packet);
CORE_API bool DeserializeSessionMetadataPacket(
    const PacketHeader& header,
    std::span<const std::byte> payload,
    SessionMetadata& out_metadata
);
CORE_API bool DeserializeEventsPacket(
    const PacketHeader& header,
    std::span<const std::byte> payload,
    Array<TraceEvent>& out_events
);

class CORE_API Scope {
public:
    explicit Scope(std::string_view name, std::string_view category = "Default");
    explicit Scope(const SpanDesc& desc);
    ~Scope();

    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

private:
    SpanToken token_{};
};

} // namespace Moer::Trace

#if MOER_TRACE_ENABLED
#define MOER_TRACE_SCOPE_VAR_JOIN_IMPL(a, b) a##b
#define MOER_TRACE_SCOPE_VAR_JOIN(a, b) MOER_TRACE_SCOPE_VAR_JOIN_IMPL(a, b)
#define TRACE_SCOPE(name) \
    ::Moer::Trace::Scope MOER_TRACE_SCOPE_VAR_JOIN(_moer_trace_scope_, __LINE__)((name), "Default")
#define TRACE_SCOPE_CAT(name, category) \
    ::Moer::Trace::Scope MOER_TRACE_SCOPE_VAR_JOIN(_moer_trace_scope_, __LINE__)((name), (category))
#define TRACE_SCOPE_DESC(desc) \
    ::Moer::Trace::Scope MOER_TRACE_SCOPE_VAR_JOIN(_moer_trace_scope_, __LINE__)((desc))
#define TRACE_GPU_SCOPE(scope_name, scope_track_id, scope_depth, scope_track_name)                        \
    ::Moer::Trace::Scope MOER_TRACE_SCOPE_VAR_JOIN(_moer_trace_scope_, __LINE__)(                         \
        ::Moer::Trace::SpanDesc{                                                                           \
            .name = (scope_name),                                                                          \
            .category = "GPU",                                                                             \
            .track_type = ::Moer::Trace::TrackType::GPUQueue,                                              \
            .track_id = (scope_track_id),                                                                  \
            .depth = (scope_depth),                                                                        \
            .track_name = (scope_track_name)                                                               \
        }                                                                                                  \
    )
#define TRACE_INSTANT(name, category, args) ::Moer::Trace::EmitInstant((name), (category), (args))
#define TRACE_COUNTER(name, value, category, args) \
    ::Moer::Trace::EmitCounter((name), (value), (category), (args))
#else
#define TRACE_SCOPE(name) ((void)0)
#define TRACE_SCOPE_CAT(name, category) ((void)0)
#define TRACE_SCOPE_DESC(desc) ((void)0)
#define TRACE_GPU_SCOPE(name, track_id, depth, track_name) ((void)0)
#define TRACE_INSTANT(name, category, args) ((void)0)
#define TRACE_COUNTER(name, value, category, args) ((void)0)
#endif
