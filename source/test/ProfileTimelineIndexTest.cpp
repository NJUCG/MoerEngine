#include "profile_consumer/ProfileTimelineIndex.h"
#include "profile/ProfileDump.h"
#include "profile/ProfileDumpCodec.h"
#include "profile/ProfileDumpTemplates.h"
#include "profile_consumer/ProfileSession.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace Moer::ProfileDump;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

class ScopedTimelineCapture final {
public:
    ScopedTimelineCapture() {
        static std::atomic_uint64_t next_id{0};
        for (std::uint32_t attempt = 0; attempt < 64; ++attempt) {
            const auto now =
                static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
            directory_ = std::filesystem::temp_directory_path() /
                         ("moer-profile-timeline-index-" + std::to_string(now) + "-" +
                          std::to_string(next_id.fetch_add(1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory(directory_, error)) {
                path_ = directory_ / "timeline.mpd";
                return;
            }
            if (error && error != std::errc::file_exists) {
                break;
            }
        }
        throw std::runtime_error("timeline index test could not reserve a temp directory");
    }

    ~ScopedTimelineCapture() {
        static_cast<void>(Shutdown());
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path directory_{};
    std::filesystem::path path_{};
};

void EmitCpuScope(
    SchemaHandle     _schema,
    std::uint64_t    _thread_id,
    std::string_view _name,
    std::uint64_t    _begin_ns,
    std::uint64_t    _end_ns,
    std::uint32_t    _depth
) {
    const std::array<FieldValueView, 5> values{
        _thread_id,
        _name,
        _begin_ns,
        _end_ns,
        _depth,
    };
    Expect(Emit(_schema, values) == EmitStatus::Accepted, "CPU scope fixture emission failed");
}

void EmitGpuFrame(
    SchemaHandle          _schema,
    std::uint64_t         _frame_id,
    ProfileGpuFrameStatus _status,
    bool                  _valid,
    std::uint64_t         _admitted,
    std::uint64_t         _dropped,
    std::uint64_t         _errors,
    std::string_view      _reason
) {
    const std::array<FieldValueView, 7> values{
        _frame_id,
        static_cast<std::uint32_t>(_status),
        _valid,
        _admitted,
        _dropped,
        _errors,
        _reason,
    };
    Expect(Emit(_schema, values) == EmitStatus::Accepted, "GPU frame fixture emission failed");
}

void EmitGpuScope(
    SchemaHandle          _schema,
    std::uint64_t         _frame_id,
    std::uint64_t         _scope_id,
    std::uint64_t         _parent_scope_id,
    std::uint64_t         _source_order,
    std::uint64_t         _local_order,
    ProfileLogicalQueue   _logical_queue,
    std::uint32_t         _native_queue_id,
    std::uint32_t         _family_id,
    std::string_view      _name,
    ProfileGpuScopeStatus _status,
    std::uint64_t         _begin_tick,
    std::uint64_t         _end_tick,
    std::uint32_t         _valid_bits,
    double                _tick_period_ns,
    double                _total_duration_ns,
    double                _exclusive_duration_ns,
    std::uint32_t         _depth,
    std::string_view      _error_reason
) {
    const std::array<FieldValueView, 18> values{
        _frame_id,
        _scope_id,
        _parent_scope_id,
        _source_order,
        _local_order,
        static_cast<std::uint32_t>(_logical_queue),
        _native_queue_id,
        _family_id,
        _name,
        static_cast<std::uint32_t>(_status),
        _begin_tick,
        _end_tick,
        _valid_bits,
        _tick_period_ns,
        _total_duration_ns,
        _exclusive_duration_ns,
        _depth,
        _error_reason,
    };
    Expect(Emit(_schema, values) == EmitStatus::Accepted, "GPU scope fixture emission failed");
}

void WriteTimelineCapture(const std::filesystem::path& _path) {
    static_cast<void>(Shutdown());
    RuntimeConfig config;
    config.output_path      = _path;
    config.replace_existing = true;
    Expect(Start(config) == StartResult::Started, "fixture capture did not start");

    const SchemaRegistration cpu       = RegisterSchema(Templates::CpuScope());
    const SchemaRegistration gpu_frame = RegisterSchema(Templates::GpuFrame());
    const SchemaRegistration gpu_scope = RegisterSchema(Templates::GpuScope());
    Expect(
        cpu.status == SchemaStatus::Registered && gpu_frame.status == SchemaStatus::Registered &&
            gpu_scope.status == SchemaStatus::Registered,
        "fixture schemas did not register"
    );

    constexpr std::uint64_t cpu_origin = (std::uint64_t{1} << 54) + 1000;
    // CPU parents are emitted after their children; the consumer reconstructs
    // topology from interval/depth plus producer record order.
    EmitCpuScope(cpu.handle, 11, "point", cpu_origin + 500, cpu_origin + 500, 2);
    EmitCpuScope(cpu.handle, 11, "child", cpu_origin + 100, cpu_origin + 900, 1);
    EmitCpuScope(cpu.handle, 11, "parent", cpu_origin, cpu_origin + 1000, 0);

    EmitCpuScope(cpu.handle, 11, "equal-leaf", cpu_origin + 1200, cpu_origin + 1300, 2);
    EmitCpuScope(cpu.handle, 11, "equal-child", cpu_origin + 1100, cpu_origin + 1800, 1);
    EmitCpuScope(cpu.handle, 11, "equal-parent", cpu_origin + 1100, cpu_origin + 2000, 0);

    EmitCpuScope(cpu.handle, 11, "sibling-a", cpu_origin + 2100, cpu_origin + 2300, 0);
    EmitCpuScope(cpu.handle, 11, "sibling-b", cpu_origin + 2300, cpu_origin + 2400, 0);

    EmitCpuScope(cpu.handle, 11, "deep-point", cpu_origin + 4000, cpu_origin + 4000, 6);
    EmitCpuScope(cpu.handle, 11, "deep-5", cpu_origin + 3500, cpu_origin + 4500, 5);
    EmitCpuScope(cpu.handle, 11, "deep-4", cpu_origin + 3400, cpu_origin + 4600, 4);
    EmitCpuScope(cpu.handle, 11, "deep-3", cpu_origin + 3300, cpu_origin + 4700, 3);
    EmitCpuScope(cpu.handle, 11, "deep-2", cpu_origin + 3200, cpu_origin + 4800, 2);
    EmitCpuScope(cpu.handle, 11, "deep-1", cpu_origin + 3100, cpu_origin + 4900, 1);
    EmitCpuScope(cpu.handle, 11, "deep-parent", cpu_origin + 3000, cpu_origin + 5000, 0);

    EmitCpuScope(cpu.handle, 22, "other-thread", cpu_origin + 200, cpu_origin + 300, 0);

    EmitGpuFrame(gpu_frame.handle, 100, ProfileGpuFrameStatus::Complete, true, 4, 0, 0, "");
    EmitGpuScope(
        gpu_scope.handle,
        100,
        1,
        0,
        5,
        0,
        ProfileLogicalQueue::Graphics,
        3,
        7,
        "gfx-root",
        ProfileGpuScopeStatus::Ready,
        100,
        200,
        64,
        1.0,
        100.0,
        70.0,
        0,
        ""
    );
    EmitGpuScope(
        gpu_scope.handle,
        100,
        2,
        1,
        5,
        1,
        ProfileLogicalQueue::Graphics,
        3,
        7,
        "gfx-child",
        ProfileGpuScopeStatus::Ready,
        110,
        140,
        64,
        1.0,
        30.0,
        30.0,
        1,
        ""
    );
    EmitGpuScope(
        gpu_scope.handle,
        100,
        4,
        0,
        7,
        0,
        ProfileLogicalQueue::Graphics,
        4,
        7,
        "wrapped-root",
        ProfileGpuScopeStatus::Ready,
        0xfffffff0ull,
        0x10ull,
        32,
        2.0,
        64.0,
        64.0,
        0,
        ""
    );
    EmitGpuScope(
        gpu_scope.handle,
        100,
        3,
        0,
        6,
        0,
        ProfileLogicalQueue::Compute,
        3,
        7,
        "compute-root",
        ProfileGpuScopeStatus::Ready,
        300,
        350,
        64,
        1.0,
        50.0,
        50.0,
        0,
        ""
    );

    EmitGpuFrame(
        gpu_frame.handle, 101, ProfileGpuFrameStatus::Invalid, false, 1, 0, 1, "timestamp unavailable"
    );
    EmitGpuScope(
        gpu_scope.handle,
        101,
        1,
        0,
        8,
        0,
        ProfileLogicalQueue::Graphics,
        5,
        9,
        "failed-root",
        ProfileGpuScopeStatus::Error,
        0,
        0,
        0,
        0.0,
        0.0,
        0.0,
        0,
        "timestamp unavailable"
    );

    EmitGpuFrame(gpu_frame.handle, 102, ProfileGpuFrameStatus::Incomplete, false, 1, 1, 0, "scope dropped");
    EmitGpuScope(
        gpu_scope.handle,
        102,
        1,
        0,
        9,
        0,
        ProfileLogicalQueue::Copy,
        6,
        10,
        "incomplete-root",
        ProfileGpuScopeStatus::Ready,
        500,
        550,
        64,
        1.0,
        50.0,
        50.0,
        0,
        ""
    );

    EmitGpuFrame(gpu_frame.handle, 103, ProfileGpuFrameStatus::Complete, true, 1, 0, 0, "");
    EmitGpuScope(
        gpu_scope.handle,
        103,
        1,
        0,
        10,
        0,
        ProfileLogicalQueue::Graphics,
        3,
        7,
        "gfx-next-frame",
        ProfileGpuScopeStatus::Ready,
        1000,
        1025,
        64,
        1.0,
        25.0,
        25.0,
        0,
        ""
    );
    EmitGpuFrame(gpu_frame.handle, 104, ProfileGpuFrameStatus::Complete, true, 0, 0, 0, "");

    Expect(Shutdown() == ShutdownResult::Completed, "fixture capture did not close cleanly");
}

void WriteEmptyCapture(const std::filesystem::path& _path) {
    static_cast<void>(Shutdown());
    RuntimeConfig config;
    config.output_path      = _path;
    config.replace_existing = true;
    Expect(Start(config) == StartResult::Started, "empty fixture capture did not start");
    Expect(Shutdown() == ShutdownResult::Completed, "empty fixture capture did not close cleanly");
}

struct LoadedTimeline {
    SessionLoadResult result{};
    ProfileSession    session{};
};

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& _path);

LoadedTimeline LoadComplete(const std::filesystem::path& _path) {
    LoadedTimeline loaded;
    loaded.result = LoadProfileSessionFile(_path, {}, loaded.session);
    if (loaded.result.status != SessionLoadStatus::Complete || !loaded.session.Valid()) {
        ProfileSessionReader            diagnostic_reader;
        const std::vector<std::uint8_t> bytes = ReadFile(_path);
        static_cast<void>(diagnostic_reader.Feed(bytes));
        static_cast<void>(diagnostic_reader.Finish());
        throw std::runtime_error(
            "timeline fixture did not load as a complete session (status=" +
            std::to_string(static_cast<unsigned>(loaded.result.status)) +
            ", error=" + std::to_string(static_cast<unsigned>(loaded.result.error_code)) +
            ", packet=" + std::to_string(loaded.result.error_packet_index) +
            ", diagnostic=" + std::string(diagnostic_reader.DiagnosticMessage()) + ")"
        );
    }
    return loaded;
}

std::vector<std::uint8_t> ReadFile(const std::filesystem::path& _path) {
    std::ifstream file(_path, std::ios::binary | std::ios::ate);
    Expect(file.is_open(), "timeline fixture file did not open");
    const std::streamoff end = file.tellg();
    Expect(end > 0, "timeline fixture file is empty");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(end));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Expect(file.good(), "timeline fixture file read failed");
    return bytes;
}

LoadedTimeline LoadForensicPrefix(const std::filesystem::path& _path) {
    const std::vector<std::uint8_t> bytes              = ReadFile(_path);
    std::size_t                     offset             = 0;
    std::size_t                     session_end_offset = bytes.size();
    CodecLimits                     codec_limits;
    while (offset < bytes.size()) {
        PacketView  packet;
        std::size_t consumed = 0;
        Expect(
            DecodePacket(
                std::span<const std::uint8_t>(bytes).subspan(offset), codec_limits, packet, consumed
            ) == DecodeStatus::Ok,
            "timeline fixture packet decode failed"
        );
        if (packet.header.type == PacketType::SessionEnd) {
            session_end_offset = offset;
            break;
        }
        offset += consumed;
    }
    Expect(session_end_offset < bytes.size(), "timeline fixture did not contain SessionEnd");

    SessionLoadOptions options;
    options.allow_forensic_truncation = true;
    ProfileSessionReader reader(options);
    Expect(
        reader.Feed(std::span<const std::uint8_t>(bytes).first(session_end_offset)).status ==
            SessionLoadStatus::Reading,
        "forensic prefix became terminal before EOF"
    );
    LoadedTimeline loaded;
    loaded.result  = reader.Finish();
    loaded.session = reader.TakeSession();
    Expect(
        loaded.result.status == SessionLoadStatus::ForensicTruncated && loaded.session.Valid(),
        "missing SessionEnd was not exposed as a forensic session"
    );
    return loaded;
}

std::uint32_t
FindCpuTrack(const ProfileTimelineIndex& _index, const ProfileSession& _session, std::uint64_t _thread_id) {
    for (std::size_t index = 0; index < _index.CpuTracks().size(); ++index) {
        const CpuTimelineTrackIndex& track = _index.CpuTracks()[index];
        if (_session.CpuTracks()[track.source_track_index].thread_id == _thread_id) {
            return static_cast<std::uint32_t>(index);
        }
    }
    throw std::runtime_error("expected CPU timeline track was not found");
}

std::uint32_t FindGpuTrack(
    const ProfileTimelineIndex& _index,
    const ProfileSession&       _session,
    ProfileLogicalQueue         _queue,
    std::uint32_t               _native_queue
) {
    for (std::size_t index = 0; index < _index.GpuTracks().size(); ++index) {
        const GpuTimelineTrackIndex& indexed = _index.GpuTracks()[index];
        const GpuTrack&              source  = _session.GpuTracks()[indexed.source_track_index];
        if (source.logical_queue == _queue && source.native_queue_id == _native_queue) {
            return static_cast<std::uint32_t>(index);
        }
    }
    throw std::runtime_error("expected GPU timeline track was not found");
}

void VerifyCpuQueries(const ProfileTimelineIndex& _index, const ProfileSession& _session) {
    const std::uint32_t          thread = FindCpuTrack(_index, _session, 11);
    const CpuTimelineTrackIndex& track  = _index.CpuTracks()[thread];
    Expect(
        track.axis_index == 0 && track.begin_ns == 0 && track.end_ns == 5000 && track.max_depth == 6 &&
            track.scope_count == 15,
        "CPU relative axis or track range is wrong"
    );

    std::array<std::uint64_t, 64> results{};
    TimelineOverlapQueryResult    query = _index.QueryCpuOverlaps(thread, 700, 800, results);
    Expect(
        query.valid && !query.truncated && query.written == 2,
        "long CPU ancestors were omitted from a visible-range query"
    );
    Expect(
        _session.String(_session.CpuScopes()[results[0]].name) == "parent" &&
            _session.String(_session.CpuScopes()[results[1]].name) == "child",
        "CPU query order is not deterministic begin-time order"
    );
    std::array<std::uint64_t, 2> exact_results{};
    query = _index.QueryCpuOverlaps(thread, 700, 800, exact_results);
    Expect(
        query.valid && !query.truncated && query.written == exact_results.size(),
        "exact-fit CPU query was reported as truncated"
    );

    query = _index.QueryCpuOverlaps(thread, 500, 501, results);
    Expect(
        query.valid && !query.truncated && query.written == 3,
        "zero-duration CPU scope was omitted at its visible point"
    );

    query = _index.QueryCpuOverlaps(thread, 500, 501, std::span<std::uint64_t>(results).first(1));
    Expect(
        query.valid && query.truncated && query.written == 1,
        "CPU query did not stop at its caller-provided output bound"
    );
    query = _index.QueryCpuOverlaps(thread, 500, 501, {});
    Expect(
        query.valid && query.truncated && query.written == 0,
        "CPU query with an empty output span did not report truncation"
    );

    query = _index.QueryCpuOverlaps(thread, 1000, 1001, results);
    Expect(
        query.valid && !query.truncated && query.written == 0,
        "half-open CPU query included a scope ending at the view boundary"
    );
    Expect(
        !_index.QueryCpuOverlaps(thread, 500, 500, results).valid,
        "empty CPU view was reported as a valid range query"
    );
    Expect(
        !_index.QueryCpuOverlaps(static_cast<std::uint32_t>(_index.CpuTracks().size()), 0, 1, results).valid,
        "invalid CPU track query was reported as valid"
    );

    const auto expect_oracle = [&](std::uint64_t _begin, std::uint64_t _end) {
        std::array<std::uint64_t, 64> expected{};
        std::size_t                   expected_count = 0;
        for (std::uint64_t local_index = 0; local_index < track.scope_count; ++local_index) {
            const CpuTimelineScopeRef& scope    = _index.CpuScopes()[track.first_scope + local_index];
            const bool                 overlaps = scope.end_ns > scope.begin_ns ?
                                                      scope.end_ns > _begin && scope.begin_ns < _end :
                                                      scope.begin_ns >= _begin && scope.begin_ns < _end;
            if (overlaps) {
                expected[expected_count++] = scope.source_scope_index;
            }
        }

        std::array<std::uint64_t, 64>    actual{};
        const TimelineOverlapQueryResult actual_query = _index.QueryCpuOverlaps(thread, _begin, _end, actual);
        Expect(
            actual_query.valid && !actual_query.truncated && actual_query.written == expected_count &&
                std::equal(
                    expected.begin(),
                    expected.begin() + static_cast<std::ptrdiff_t>(expected_count),
                    actual.begin()
                ),
            "CPU interval index disagreed with the brute-force overlap oracle"
        );
    };

    constexpr std::array<std::uint64_t, 5> widths{1, 17, 211, 997, 2048};
    for (std::uint64_t begin = 0; begin < 5200; begin += 137) {
        for (const std::uint64_t width : widths) {
            expect_oracle(begin, begin + width);
        }
    }
    expect_oracle(1100, 1101);
    expect_oracle(2300, 2301);
    expect_oracle(4000, 4001);
}

void VerifyGpuDomainsAndSlices(const ProfileTimelineIndex& _index, const ProfileSession& _session) {
    Expect(
        _index.Axes().size() == 1 + _session.GpuDomains().size(),
        "timeline axes did not preserve every physical GPU domain"
    );
    Expect(
        _index.Axes().front().kind == TimelineAxisKind::CpuSteadyClock &&
            _index.Axes().front().source_domain_index == kInvalidTimelineAxis &&
            _index.Axes().front().unit_period_ns == 1.0 && _index.Axes().front().timing_available &&
            _index.Axes().front().timing_capability_trusted && _index.Axes().front().calibrated_to_cpu &&
            !_index.Axes().front().calibrated_to_other_gpu_domains,
        "CPU steady-clock axis metadata is wrong"
    );
    for (std::size_t index = 1; index < _index.Axes().size(); ++index) {
        Expect(
            _index.Axes()[index].kind == TimelineAxisKind::GpuPhysicalTimestampDomain &&
                !_index.Axes()[index].calibrated_to_cpu &&
                !_index.Axes()[index].calibrated_to_other_gpu_domains,
            "GPU axis incorrectly claims CPU or cross-domain calibration"
        );
    }

    const std::uint32_t graphics   = FindGpuTrack(_index, _session, ProfileLogicalQueue::Graphics, 3);
    const std::uint32_t compute    = FindGpuTrack(_index, _session, ProfileLogicalQueue::Compute, 3);
    const std::uint32_t wrapped    = FindGpuTrack(_index, _session, ProfileLogicalQueue::Graphics, 4);
    const std::uint32_t failed     = FindGpuTrack(_index, _session, ProfileLogicalQueue::Graphics, 5);
    const std::uint32_t incomplete = FindGpuTrack(_index, _session, ProfileLogicalQueue::Copy, 6);
    Expect(
        _index.GpuTracks()[graphics].axis_index == _index.GpuTracks()[compute].axis_index &&
            _index.GpuTracks()[graphics].axis_index != _index.GpuTracks()[wrapped].axis_index,
        "logical queue aliasing did not preserve physical-domain identity"
    );

    const GpuTimelineFrameRef* frame = _index.FindGpuFrame(100);
    Expect(
        frame != nullptr && _session.GpuFrames()[frame->source_frame_index].frame_id == 100,
        "GPU frame lookup did not retain its source identity"
    );
    const GpuTimelineFrameSlice* graphics_slice      = _index.FindGpuFrameSlice(graphics, 100);
    const GpuTimelineFrameSlice* next_graphics_slice = _index.FindGpuFrameSlice(graphics, 103);
    const GpuTimelineFrameSlice* failed_slice        = _index.FindGpuFrameSlice(failed, 101);
    const GpuTimelineFrameSlice* incomplete_slice    = _index.FindGpuFrameSlice(incomplete, 102);
    const GpuTimelineFrameSlice* compute_slice       = _index.FindGpuFrameSlice(compute, 100);
    const GpuTimelineFrameSlice* wrapped_slice       = _index.FindGpuFrameSlice(wrapped, 100);
    Expect(
        graphics_slice != nullptr && graphics_slice->scope_count == 2 && graphics_slice->has_frame_record &&
            graphics_slice->materialization_complete && graphics_slice->timing_topology_trusted &&
            next_graphics_slice != nullptr && next_graphics_slice->scope_count == 1 &&
            next_graphics_slice->timing_topology_trusted && compute_slice != nullptr &&
            compute_slice->scope_count == 1 && compute_slice->timing_topology_trusted &&
            wrapped_slice != nullptr && wrapped_slice->scope_count == 1 &&
            wrapped_slice->timing_topology_trusted,
        "trusted GPU frame slices are wrong"
    );
    Expect(
        failed_slice != nullptr && failed_slice->scope_count == 1 && failed_slice->error_scope_count == 1 &&
            failed_slice->frame_status == ProfileGpuFrameStatus::Invalid &&
            !failed_slice->timing_topology_trusted,
        "invalid/error GPU frame slice was hidden or marked trusted"
    );
    Expect(
        incomplete_slice != nullptr && incomplete_slice->scope_count == 1 &&
            incomplete_slice->error_scope_count == 0 &&
            incomplete_slice->frame_status == ProfileGpuFrameStatus::Incomplete &&
            !incomplete_slice->timing_topology_trusted,
        "incomplete GPU frame slice was hidden or marked trusted"
    );

    const std::uint32_t         shared_axis       = _index.GpuTracks()[graphics].axis_index;
    const GpuTimelineAxisFrame* shared_frame      = _index.FindGpuAxisFrame(shared_axis, 100);
    const GpuTimelineAxisFrame* next_shared_frame = _index.FindGpuAxisFrame(shared_axis, 103);
    const GpuTimelineAxisFrame* wrapped_frame =
        _index.FindGpuAxisFrame(_index.GpuTracks()[wrapped].axis_index, 100);
    const GpuTimelineAxisFrame* failed_frame =
        _index.FindGpuAxisFrame(_index.GpuTracks()[failed].axis_index, 101);
    const GpuTimelineAxisFrame* incomplete_frame =
        _index.FindGpuAxisFrame(_index.GpuTracks()[incomplete].axis_index, 102);
    Expect(
        shared_frame != nullptr && shared_frame->origin_tick == 100 && shared_frame->extent_ticks == 250 &&
            shared_frame->ready_scope_count == 3 && shared_frame->error_scope_count == 0 &&
            shared_frame->timing_available && shared_frame->timing_capability_trusted &&
            shared_frame->timing_topology_trusted && next_shared_frame != nullptr &&
            next_shared_frame->origin_tick == 1000 && next_shared_frame->extent_ticks == 25,
        "shared physical GPU axis did not get frame-local timing extents"
    );
    Expect(
        wrapped_frame != nullptr && wrapped_frame->origin_tick == 0xfffffff0ull &&
            wrapped_frame->extent_ticks == 32 && wrapped_frame->timing_available &&
            wrapped_frame->timing_topology_trusted,
        "wrapped GPU timestamps were not normalized to a local axis"
    );
    Expect(
        failed_frame != nullptr && failed_frame->ready_scope_count == 0 &&
            failed_frame->error_scope_count == 1 && !failed_frame->timing_available &&
            !failed_frame->timing_topology_trusted && incomplete_frame != nullptr &&
            incomplete_frame->origin_tick == 500 && incomplete_frame->extent_ticks == 50 &&
            incomplete_frame->timing_available && !incomplete_frame->timing_capability_trusted &&
            !incomplete_frame->timing_topology_trusted,
        "untrusted GPU frame timing diagnostics are wrong"
    );

    Expect(
        graphics_slice->axis_frame_index == compute_slice->axis_frame_index &&
            graphics_slice->axis_frame_index != next_graphics_slice->axis_frame_index &&
            graphics_slice->timeline_scope_count == 2 && compute_slice->timeline_scope_count == 1 &&
            wrapped_slice->timeline_scope_count == 1 && failed_slice->timeline_scope_count == 0 &&
            incomplete_slice->timeline_scope_count == 1,
        "GPU track slices do not reference the expected frame-local timing data"
    );

    std::array<std::uint64_t, 8> query_results{};
    TimelineOverlapQueryResult   query = _index.QueryGpuOverlaps(graphics, 100, 20, 30, query_results);
    Expect(
        query.valid && !query.truncated && query.written == 2 &&
            _session.String(_session.GpuScopes()[query_results[0]].name) == "gfx-root" &&
            _session.String(_session.GpuScopes()[query_results[1]].name) == "gfx-child",
        "GPU visible-range query lost nested scopes or deterministic order"
    );
    std::array<std::uint64_t, 2> exact_gpu_results{};
    query = _index.QueryGpuOverlaps(graphics, 100, 20, 30, exact_gpu_results);
    Expect(
        query.valid && !query.truncated && query.written == exact_gpu_results.size(),
        "exact-fit GPU query was reported as truncated"
    );
    query = _index.QueryGpuOverlaps(compute, 100, 210, 220, query_results);
    Expect(
        query.valid && !query.truncated && query.written == 1 &&
            _session.String(_session.GpuScopes()[query_results[0]].name) == "compute-root",
        "logical queues sharing one GPU axis did not share its local origin"
    );
    query = _index.QueryGpuOverlaps(wrapped, 100, 0, 32, query_results);
    Expect(
        query.valid && !query.truncated && query.written == 1,
        "wrapped GPU scope was not queryable on its normalized range"
    );
    query = _index.QueryGpuOverlaps(graphics, 100, 20, 30, std::span<std::uint64_t>(query_results).first(1));
    Expect(
        query.valid && query.truncated && query.written == 1,
        "GPU query did not honor its caller-provided output bound"
    );
    Expect(
        !_index.QueryGpuOverlaps(failed, 101, 0, 1, query_results).valid &&
            !_index.QueryGpuOverlaps(graphics, 999, 0, 1, query_results).valid,
        "GPU timing query accepted an error-only or missing frame"
    );

    const TimelineAxis& failed_axis = _index.Axes()[_index.GpuTracks()[failed].axis_index];
    Expect(
        failed_axis.native_queue_id == 5 && failed_axis.family_id == 9 && !failed_axis.timing_available &&
            !failed_axis.timing_capability_trusted && failed_axis.valid_bits == 0 &&
            failed_axis.unit_period_ns == 0.0,
        "error-only physical GPU domain advertised usable timing"
    );
    Expect(_index.FindGpuFrameSlice(compute, 101) == nullptr, "GPU track lookup invented a frame slice");
    Expect(
        _index.FindGpuFrame(999) == nullptr && _index.FindGpuFrame(104) != nullptr &&
            _index.FindGpuFrameSlice(graphics, 104) == nullptr &&
            _index.FindGpuAxisFrame(shared_axis, 999) == nullptr &&
            _index.FindGpuFrameSlice(static_cast<std::uint32_t>(_index.GpuTracks().size()), 100) == nullptr,
        "missing GPU frame or invalid track lookup returned a result"
    );
}

void VerifyPublicRangeIntegrity(const ProfileTimelineIndex& _index, const ProfileSession& _session) {
    Expect(
        _index.CpuTracks().size() == _session.CpuTracks().size() &&
            _index.CpuScopes().size() == _session.CpuScopes().size() &&
            _index.GpuFrames().size() == _session.GpuFrames().size() &&
            _index.GpuTracks().size() == _session.GpuTracks().size(),
        "timeline index omitted a public source track or record"
    );

    std::uint64_t cpu_scope_cursor = 0;
    for (const CpuTimelineTrackIndex& indexed : _index.CpuTracks()) {
        Expect(
            indexed.source_track_index < _session.CpuTracks().size() &&
                indexed.first_scope == cpu_scope_cursor && indexed.first_scope <= _index.CpuScopes().size() &&
                indexed.scope_count <= _index.CpuScopes().size() - indexed.first_scope,
            "CPU timeline track exposes an invalid retained range"
        );
        const CpuTrack& source_track = _session.CpuTracks()[indexed.source_track_index];
        Expect(
            indexed.scope_count == source_track.scope_count,
            "CPU timeline track count disagrees with its source track"
        );
        for (std::uint64_t local_index = 0; local_index < indexed.scope_count; ++local_index) {
            const CpuTimelineScopeRef& retained = _index.CpuScopes()[indexed.first_scope + local_index];
            const std::uint64_t        expected_source = source_track.first_scope + local_index;
            Expect(
                retained.source_scope_index == expected_source &&
                    expected_source < _session.CpuScopes().size(),
                "CPU timeline scope points outside its source track"
            );
            const CpuScopeRecord& source = _session.CpuScopes()[expected_source];
            Expect(
                source.track_index == indexed.source_track_index &&
                    retained.begin_ns == source.begin_ns - _session.Summary().cpu_begin_ns &&
                    retained.end_ns == source.end_ns - _session.Summary().cpu_begin_ns,
                "CPU timeline scope payload disagrees with its source record"
            );
        }
        cpu_scope_cursor += indexed.scope_count;
    }
    Expect(
        cpu_scope_cursor == _index.CpuScopes().size(),
        "CPU timeline tracks did not cover every retained scope exactly once"
    );

    for (const GpuTimelineFrameRef& indexed : _index.GpuFrames()) {
        Expect(
            indexed.source_frame_index < _session.GpuFrames().size() &&
                _session.GpuFrames()[indexed.source_frame_index].frame_id == indexed.frame_id,
            "GPU frame lookup index points outside its source records"
        );
    }

    std::uint64_t gpu_slice_cursor    = 0;
    std::uint64_t gpu_timeline_cursor = 0;
    for (const GpuTimelineTrackIndex& indexed : _index.GpuTracks()) {
        Expect(
            indexed.source_track_index < _session.GpuTracks().size() &&
                indexed.axis_index < _index.Axes().size() && indexed.first_frame_slice == gpu_slice_cursor &&
                indexed.first_frame_slice <= _index.GpuFrameSlices().size() &&
                indexed.frame_slice_count <= _index.GpuFrameSlices().size() - indexed.first_frame_slice,
            "GPU timeline track exposes an invalid retained range"
        );
        const GpuTrack&           source_track = _session.GpuTracks()[indexed.source_track_index];
        const GpuTimestampDomain& domain       = _session.GpuDomains()[source_track.domain_index];
        const TimelineAxis&       axis         = _index.Axes()[indexed.axis_index];
        Expect(
            axis.source_domain_index == source_track.domain_index &&
                axis.native_queue_id == domain.native_queue_id && axis.family_id == domain.family_id &&
                axis.logical_queue_mask == domain.logical_queue_mask &&
                axis.valid_bits == domain.valid_bits && axis.unit_period_ns == domain.tick_period_ns &&
                axis.timing_available == domain.has_ready_timestamps &&
                axis.timing_capability_trusted == domain.timing_capability_trusted,
            "GPU timeline axis disagrees with its physical source domain"
        );

        std::uint64_t previous_frame = 0;
        bool          has_frame      = false;
        std::uint64_t source_cursor  = source_track.first_scope;
        for (std::uint64_t local_index = 0; local_index < indexed.frame_slice_count; ++local_index) {
            const GpuTimelineFrameSlice& slice =
                _index.GpuFrameSlices()[indexed.first_frame_slice + local_index];
            Expect(
                (!has_frame || previous_frame < slice.frame_id) && slice.scope_count != 0 &&
                    slice.first_scope == source_cursor &&
                    slice.first_scope <= source_track.first_scope + source_track.scope_count &&
                    slice.scope_count <=
                        source_track.first_scope + source_track.scope_count - slice.first_scope &&
                    slice.axis_frame_index < _index.GpuAxisFrames().size() &&
                    slice.first_timeline_scope == gpu_timeline_cursor &&
                    slice.first_timeline_scope <= _index.GpuTimelineScopes().size() &&
                    slice.timeline_scope_count <=
                        _index.GpuTimelineScopes().size() - slice.first_timeline_scope,
                "GPU frame slices are unsorted or escape their source track"
            );
            previous_frame = slice.frame_id;
            has_frame      = true;
            source_cursor += slice.scope_count;

            std::uint64_t ready_scope_count = 0;
            for (std::uint64_t scope_offset = 0; scope_offset < slice.scope_count; ++scope_offset) {
                const GpuScopeRecord& scope = _session.GpuScopes()[slice.first_scope + scope_offset];
                Expect(
                    scope.track_index == indexed.source_track_index &&
                        scope.domain_index == source_track.domain_index && scope.frame_id == slice.frame_id &&
                        scope.frame_index == slice.source_frame_index,
                    "GPU frame slice disagrees with one of its source scopes"
                );
                ready_scope_count += static_cast<std::uint64_t>(scope.status == ProfileGpuScopeStatus::Ready);
            }
            const GpuTimelineAxisFrame& axis_frame = _index.GpuAxisFrames()[slice.axis_frame_index];
            Expect(
                axis_frame.axis_index == indexed.axis_index && axis_frame.frame_id == slice.frame_id &&
                    axis_frame.source_frame_index == slice.source_frame_index &&
                    slice.timeline_scope_count == (axis_frame.timing_available ? ready_scope_count : 0),
                "GPU frame slice disagrees with its physical axis-frame metadata"
            );
            for (std::uint64_t timeline_offset = 0; timeline_offset < slice.timeline_scope_count;
                 ++timeline_offset) {
                const GpuTimelineScopeRef& retained =
                    _index.GpuTimelineScopes()[slice.first_timeline_scope + timeline_offset];
                Expect(
                    retained.source_scope_index >= slice.first_scope &&
                        retained.source_scope_index < slice.first_scope + slice.scope_count &&
                        _session.GpuScopes()[retained.source_scope_index].status ==
                            ProfileGpuScopeStatus::Ready &&
                        retained.begin_tick_offset <= retained.end_tick_offset &&
                        retained.end_tick_offset <= axis_frame.extent_ticks,
                    "GPU timeline scope escaped its source slice or frame-local extent"
                );
            }
            gpu_timeline_cursor += slice.timeline_scope_count;
        }
        Expect(
            source_cursor == source_track.first_scope + source_track.scope_count,
            "GPU frame slices did not cover their source track exactly once"
        );
        gpu_slice_cursor += indexed.frame_slice_count;
    }
    Expect(
        gpu_slice_cursor == _index.GpuFrameSlices().size() &&
            gpu_timeline_cursor == _index.GpuTimelineScopes().size(),
        "GPU tracks did not cover every retained slice or timeline scope"
    );

    std::vector<std::pair<std::uint32_t, std::uint64_t>> expected_axis_frames;
    expected_axis_frames.reserve(_session.GpuScopes().size());
    for (const GpuScopeRecord& scope : _session.GpuScopes()) {
        expected_axis_frames.emplace_back(scope.domain_index, scope.frame_id);
    }
    std::sort(expected_axis_frames.begin(), expected_axis_frames.end());
    expected_axis_frames.erase(
        std::unique(expected_axis_frames.begin(), expected_axis_frames.end()), expected_axis_frames.end()
    );
    Expect(
        _index.GpuAxisFrames().size() == expected_axis_frames.size(),
        "GPU physical axis-frame index omitted a source domain/frame pair"
    );
    for (std::size_t index = 0; index < _index.GpuAxisFrames().size(); ++index) {
        const GpuTimelineAxisFrame& axis_frame = _index.GpuAxisFrames()[index];
        Expect(
            axis_frame.axis_index == 1 + expected_axis_frames[index].first &&
                axis_frame.frame_id == expected_axis_frames[index].second &&
                _index.FindGpuAxisFrame(axis_frame.axis_index, axis_frame.frame_id) == &axis_frame,
            "GPU physical axis-frame order or lookup is inconsistent"
        );
    }
}

template<typename T>
std::vector<T> CopySpan(std::span<const T> _values) {
    return std::vector<T>(_values.begin(), _values.end());
}

struct TimelineIndexSnapshot {
    TimelineIndexBuildResult           result{};
    ProfileTimelineQuality             quality{};
    std::vector<TimelineAxis>          axes{};
    std::vector<CpuTimelineTrackIndex> cpu_tracks{};
    std::vector<CpuTimelineScopeRef>   cpu_scopes{};
    std::vector<GpuTimelineFrameRef>   gpu_frames{};
    std::vector<GpuTimelineTrackIndex> gpu_tracks{};
    std::vector<GpuTimelineFrameSlice> gpu_slices{};
    std::vector<GpuTimelineAxisFrame>  gpu_axis_frames{};
    std::vector<GpuTimelineScopeRef>   gpu_scopes{};
};

TimelineIndexSnapshot CaptureIndex(const ProfileTimelineIndex& _index) {
    return {
        .result          = _index.BuildResult(),
        .quality         = _index.Quality(),
        .axes            = CopySpan(_index.Axes()),
        .cpu_tracks      = CopySpan(_index.CpuTracks()),
        .cpu_scopes      = CopySpan(_index.CpuScopes()),
        .gpu_frames      = CopySpan(_index.GpuFrames()),
        .gpu_tracks      = CopySpan(_index.GpuTracks()),
        .gpu_slices      = CopySpan(_index.GpuFrameSlices()),
        .gpu_axis_frames = CopySpan(_index.GpuAxisFrames()),
        .gpu_scopes      = CopySpan(_index.GpuTimelineScopes()),
    };
}

bool MatchesSnapshot(const ProfileTimelineIndex& _index, const TimelineIndexSnapshot& _snapshot) {
    const auto equals = []<typename T>(std::span<const T> _actual, const std::vector<T>& _expected) {
        return _actual.size() == _expected.size() &&
               std::equal(_actual.begin(), _actual.end(), _expected.begin());
    };
    return _index.BuildResult() == _snapshot.result && _index.Quality() == _snapshot.quality &&
           equals(_index.Axes(), _snapshot.axes) && equals(_index.CpuTracks(), _snapshot.cpu_tracks) &&
           equals(_index.CpuScopes(), _snapshot.cpu_scopes) &&
           equals(_index.GpuFrames(), _snapshot.gpu_frames) &&
           equals(_index.GpuTracks(), _snapshot.gpu_tracks) &&
           equals(_index.GpuFrameSlices(), _snapshot.gpu_slices) &&
           equals(_index.GpuAxisFrames(), _snapshot.gpu_axis_frames) &&
           equals(_index.GpuTimelineScopes(), _snapshot.gpu_scopes);
}

std::uint64_t TreeValueCount(std::uint64_t _scope_count) {
    std::uint64_t leaves = 1;
    while (leaves < _scope_count) {
        leaves *= 2;
    }
    return leaves * 2;
}

std::uint64_t ExpectedLogicalBytes(const ProfileTimelineIndex& _index) {
    std::uint64_t cpu_tree_values = 0;
    for (const CpuTimelineTrackIndex& track : _index.CpuTracks()) {
        cpu_tree_values += TreeValueCount(track.scope_count);
    }
    std::uint64_t gpu_tree_values = 0;
    for (const GpuTimelineFrameSlice& slice : _index.GpuFrameSlices()) {
        const GpuTimelineAxisFrame& axis_frame = _index.GpuAxisFrames()[slice.axis_frame_index];
        if (axis_frame.timing_available) {
            gpu_tree_values += TreeValueCount(slice.timeline_scope_count);
        }
    }
    constexpr std::uint64_t interval_tree_bytes = sizeof(std::uint64_t) * 2;
    return _index.Axes().size() * sizeof(TimelineAxis) +
           _index.CpuTracks().size() * sizeof(CpuTimelineTrackIndex) +
           _index.CpuScopes().size() * sizeof(CpuTimelineScopeRef) +
           _index.CpuTracks().size() * interval_tree_bytes + cpu_tree_values * sizeof(std::uint64_t) +
           _index.GpuFrames().size() * sizeof(GpuTimelineFrameRef) +
           _index.GpuTracks().size() * sizeof(GpuTimelineTrackIndex) +
           _index.GpuFrameSlices().size() * sizeof(GpuTimelineFrameSlice) +
           _index.GpuAxisFrames().size() * sizeof(GpuTimelineAxisFrame) +
           _index.GpuTimelineScopes().size() * sizeof(GpuTimelineScopeRef) +
           _index.GpuFrameSlices().size() * interval_tree_bytes + gpu_tree_values * sizeof(std::uint64_t);
}

void VerifyQualityAndAtomicLimits(LoadedTimeline& _complete, const LoadedTimeline& _forensic) {
    ProfileTimelineIndex           index;
    const TimelineIndexBuildResult built =
        BuildProfileTimelineIndex(_complete.session, _complete.result, {}, index);
    Expect(
        built.Succeeded() && index.Valid() && index.Matches(_complete.session),
        "complete timeline index did not build"
    );
    Expect(
        !index.Matches(_forensic.session),
        "timeline index matched a different immutable session with the same shape"
    );
    const ProfileTimelineQuality& quality = index.Quality();
    const std::uint32_t           expected_quality_flags =
        TimelineQualityBit(TimelineQualityFlag::IncompleteGpuFrames) |
        TimelineQualityBit(TimelineQualityFlag::InvalidGpuFrames) |
        TimelineQualityBit(TimelineQualityFlag::GpuScopeErrors) |
        TimelineQualityBit(TimelineQualityFlag::UntrustedGpuTiming);
    const ProfileTimelineQuality expected_quality{
        .flags                      = expected_quality_flags,
        .incomplete_gpu_frame_count = 1,
        .invalid_gpu_frame_count    = 1,
        .error_gpu_scope_count      = 1,
        .untrusted_gpu_frame_count  = 2,
    };
    Expect(quality == expected_quality, "complete session quality flags are wrong");

    ProfileTimelineIndex forensic_index;
    Expect(
        BuildProfileTimelineIndex(_forensic.session, _forensic.result, {}, forensic_index).Succeeded() &&
            forensic_index.Quality() ==
                ProfileTimelineQuality{
                    .flags =
                        expected_quality_flags | TimelineQualityBit(TimelineQualityFlag::ForensicTruncated),
                    .incomplete_gpu_frame_count = 1,
                    .invalid_gpu_frame_count    = 1,
                    .error_gpu_scope_count      = 1,
                    .untrusted_gpu_frame_count  = 2,
                },
        "forensic session quality was not retained"
    );

    const std::uint64_t retained_bytes = index.BuildResult().logical_bytes;
    Expect(
        retained_bytes != 0 && retained_bytes == ExpectedLogicalBytes(index),
        "timeline index logical bytes do not cover every retained vector"
    );

    TimelineIndexLimits exact;
    exact.max_logical_bytes = retained_bytes;
    ProfileTimelineIndex exact_index;
    Expect(
        BuildProfileTimelineIndex(_complete.session, _complete.result, exact, exact_index).Succeeded(),
        "exact timeline logical-byte limit was rejected"
    );
    const TimelineIndexSnapshot exact_snapshot = CaptureIndex(exact_index);

    TimelineIndexLimits one_less = exact;
    one_less.max_logical_bytes   = retained_bytes - 1;
    const TimelineIndexBuildResult rejected =
        BuildProfileTimelineIndex(_complete.session, _complete.result, one_less, exact_index);
    Expect(
        rejected.status == TimelineIndexBuildStatus::LimitExceeded &&
            rejected.limit_kind == TimelineIndexLimitKind::LogicalBytes && exact_index.Valid() &&
            exact_index.Matches(_complete.session) && MatchesSnapshot(exact_index, exact_snapshot),
        "failed timeline rebuild replaced the previous usable index"
    );

    TimelineIndexLimits zero_bytes = exact;
    zero_bytes.max_logical_bytes   = 0;
    const TimelineIndexBuildResult zero_rejected =
        BuildProfileTimelineIndex(_complete.session, _complete.result, zero_bytes, exact_index);
    Expect(
        zero_rejected.status == TimelineIndexBuildStatus::LimitExceeded &&
            zero_rejected.limit_kind == TimelineIndexLimitKind::LogicalBytes &&
            MatchesSnapshot(exact_index, exact_snapshot),
        "zero logical-byte limit was not reported as a bounded rejection"
    );

    TimelineIndexLimits exact_counts;
    exact_counts.max_cpu_scopes          = _complete.session.CpuScopes().size();
    exact_counts.max_gpu_frame_slices    = index.GpuFrameSlices().size();
    exact_counts.max_gpu_timeline_scopes = index.GpuTimelineScopes().size();
    ProfileTimelineIndex count_limited;
    Expect(
        BuildProfileTimelineIndex(_complete.session, _complete.result, exact_counts, count_limited)
            .Succeeded(),
        "exact timeline count limits were rejected"
    );
    const TimelineIndexSnapshot count_snapshot = CaptureIndex(count_limited);

    TimelineIndexLimits one_less_cpu = exact_counts;
    one_less_cpu.max_cpu_scopes -= 1;
    const TimelineIndexBuildResult cpu_rejected =
        BuildProfileTimelineIndex(_complete.session, _complete.result, one_less_cpu, count_limited);
    Expect(
        cpu_rejected.status == TimelineIndexBuildStatus::LimitExceeded &&
            cpu_rejected.limit_kind == TimelineIndexLimitKind::CpuScopes &&
            MatchesSnapshot(count_limited, count_snapshot),
        "CPU scope limit rejection replaced the prior timeline index"
    );

    TimelineIndexLimits one_less_gpu = exact_counts;
    one_less_gpu.max_gpu_frame_slices -= 1;
    const TimelineIndexBuildResult gpu_rejected =
        BuildProfileTimelineIndex(_complete.session, _complete.result, one_less_gpu, count_limited);
    Expect(
        gpu_rejected.status == TimelineIndexBuildStatus::LimitExceeded &&
            gpu_rejected.limit_kind == TimelineIndexLimitKind::GpuFrameSlices &&
            MatchesSnapshot(count_limited, count_snapshot),
        "GPU frame-slice limit rejection replaced the prior timeline index"
    );

    TimelineIndexLimits one_less_gpu_scope = exact_counts;
    one_less_gpu_scope.max_gpu_timeline_scopes -= 1;
    const TimelineIndexBuildResult gpu_scope_rejected =
        BuildProfileTimelineIndex(_complete.session, _complete.result, one_less_gpu_scope, count_limited);
    Expect(
        gpu_scope_rejected.status == TimelineIndexBuildStatus::LimitExceeded &&
            gpu_scope_rejected.limit_kind == TimelineIndexLimitKind::GpuTimelineScopes &&
            MatchesSnapshot(count_limited, count_snapshot),
        "GPU timeline-scope limit rejection replaced the prior index"
    );

    TimelineIndexLimits zero_transient       = exact_counts;
    zero_transient.max_transient_build_bytes = 0;
    const TimelineIndexBuildResult transient_rejected =
        BuildProfileTimelineIndex(_complete.session, _complete.result, zero_transient, count_limited);
    Expect(
        transient_rejected.status == TimelineIndexBuildStatus::LimitExceeded &&
            transient_rejected.limit_kind == TimelineIndexLimitKind::TransientBuildBytes &&
            MatchesSnapshot(count_limited, count_snapshot),
        "transient build-byte rejection replaced the prior timeline index"
    );

    SessionLoadResult unusable = _complete.result;
    unusable.status            = SessionLoadStatus::Reading;
    const TimelineIndexBuildResult unusable_rejected =
        BuildProfileTimelineIndex(_complete.session, unusable, exact_counts, count_limited);
    Expect(
        unusable_rejected.status == TimelineIndexBuildStatus::InvalidArgument &&
            MatchesSnapshot(count_limited, count_snapshot),
        "unusable session result replaced the prior timeline index"
    );

    const TimelineIndexBuildResult mismatched_rejected =
        BuildProfileTimelineIndex(_complete.session, _forensic.result, exact_counts, count_limited);
    Expect(
        mismatched_rejected.status == TimelineIndexBuildStatus::InvalidArgument &&
            MatchesSnapshot(count_limited, count_snapshot),
        "load result from a different session was accepted by the index builder"
    );

    std::stop_source stop_source;
    stop_source.request_stop();
    const TimelineIndexBuildControl cancelled_control{
        .stop_token                  = stop_source.get_token(),
        .cancellation_check_interval = 1,
    };
    const TimelineIndexBuildResult cancelled = BuildProfileTimelineIndex(
        _complete.session, _complete.result, exact_counts, count_limited, cancelled_control
    );
    Expect(
        cancelled.status == TimelineIndexBuildStatus::Cancelled &&
            MatchesSnapshot(count_limited, count_snapshot),
        "cancelled timeline build replaced the prior usable index"
    );

    std::stop_source                passive_stop_source;
    const TimelineIndexBuildControl zero_interval_control{
        .stop_token                  = passive_stop_source.get_token(),
        .cancellation_check_interval = 0,
    };
    ProfileTimelineIndex zero_interval_index;
    Expect(
        BuildProfileTimelineIndex(
            _complete.session, _complete.result, exact_counts, zero_interval_index, zero_interval_control
        )
            .Succeeded(),
        "zero cancellation interval or a non-requested live stop token rejected a valid build"
    );

    const auto build_with_budget = [&](std::uint64_t _budget, ProfileTimelineIndex& _output) {
        return BuildProfileTimelineIndex(
            _complete.session,
            _complete.result,
            exact_counts,
            _output,
            TimelineIndexBuildControl{
                .cancellation_check_interval  = 1,
                .max_work_items_before_cancel = _budget,
            }
        );
    };
    std::uint64_t minimum_success_budget = 1;
    for (;;) {
        ProfileTimelineIndex           probe;
        const TimelineIndexBuildResult probe_result = build_with_budget(minimum_success_budget, probe);
        if (probe_result.Succeeded()) {
            break;
        }
        Expect(
            probe_result.status == TimelineIndexBuildStatus::Cancelled,
            "cooperative work budget produced a non-cancellation failure"
        );
        Expect(
            minimum_success_budget <= (std::uint64_t{1} << 20),
            "cooperative cancellation checkpoint count exceeded the test bound"
        );
        minimum_success_budget *= 2;
    }
    std::uint64_t cancelled_budget = minimum_success_budget / 2;
    while (cancelled_budget + 1 < minimum_success_budget) {
        const std::uint64_t  middle = cancelled_budget + (minimum_success_budget - cancelled_budget) / 2;
        ProfileTimelineIndex probe;
        if (build_with_budget(middle, probe).Succeeded()) {
            minimum_success_budget = middle;
        } else {
            cancelled_budget = middle;
        }
    }
    const TimelineIndexBuildResult deep_cancelled = build_with_budget(cancelled_budget, count_limited);
    Expect(
        deep_cancelled.status == TimelineIndexBuildStatus::Cancelled &&
            MatchesSnapshot(count_limited, count_snapshot),
        "late cooperative cancellation replaced the prior usable index"
    );
    VerifyCpuQueries(count_limited, _complete.session);
    VerifyGpuDomainsAndSlices(count_limited, _complete.session);

    ProfileTimelineIndex moved = std::move(exact_index);
    Expect(
        moved.Valid() && moved.Matches(_complete.session) && !exact_index.Valid(),
        "timeline index move invalidated its retained source indices"
    );
    ProfileSession moved_session = std::move(_complete.session);
    Expect(
        moved.Matches(moved_session) && !moved.Matches(_complete.session),
        "timeline index identity did not survive a ProfileSession move"
    );
}

void VerifyEmptySession(const LoadedTimeline& _empty) {
    ProfileTimelineIndex           index;
    const TimelineIndexBuildResult built =
        BuildProfileTimelineIndex(_empty.session, _empty.result, {}, index);
    Expect(
        built.Succeeded() && index.Valid() && index.Matches(_empty.session) && index.Axes().size() == 1 &&
            index.Axes().front().kind == TimelineAxisKind::CpuSteadyClock &&
            !index.Axes().front().timing_available && !index.Axes().front().timing_capability_trusted &&
            index.CpuTracks().empty() && index.CpuScopes().empty() && index.GpuFrames().empty() &&
            index.GpuTracks().empty() && index.GpuFrameSlices().empty() && index.GpuAxisFrames().empty() &&
            index.GpuTimelineScopes().empty() &&
            index.BuildResult().logical_bytes == ExpectedLogicalBytes(index) && index.Quality().Clean(),
        "empty profile session did not produce a valid empty timeline index"
    );

    std::array<std::uint64_t, 1> output{};
    Expect(
        !index.QueryCpuOverlaps(0, 0, 1, output).valid && index.FindGpuFrame(0) == nullptr &&
            index.FindGpuFrameSlice(0, 0) == nullptr && index.FindGpuAxisFrame(0, 0) == nullptr &&
            !index.QueryGpuOverlaps(0, 0, 0, 1, output).valid,
        "empty timeline index exposed invented CPU or GPU entries"
    );

    ProfileTimelineIndex invalid;
    Expect(
        !invalid.Valid() && invalid.BuildResult().status == TimelineIndexBuildStatus::InvalidArgument &&
            invalid.Axes().empty() && invalid.CpuTracks().empty() && invalid.CpuScopes().empty() &&
            invalid.GpuFrames().empty() && invalid.GpuTracks().empty() && invalid.GpuFrameSlices().empty() &&
            invalid.GpuAxisFrames().empty() && invalid.GpuTimelineScopes().empty() &&
            !invalid.QueryCpuOverlaps(0, 0, 1, output).valid && invalid.FindGpuFrame(0) == nullptr &&
            invalid.FindGpuFrameSlice(0, 0) == nullptr && invalid.FindGpuAxisFrame(0, 0) == nullptr &&
            !invalid.QueryGpuOverlaps(0, 0, 0, 1, output).valid,
        "default timeline index did not remain safely invalid"
    );
}

} // namespace

int main(int _argc, char** _argv) {
    try {
        if (_argc == 2) {
            const LoadedTimeline external = LoadComplete(std::filesystem::path(_argv[1]));
            ProfileTimelineIndex external_index;
            Expect(
                BuildProfileTimelineIndex(external.session, external.result, {}, external_index).Succeeded(),
                "external capture timeline index did not build"
            );
            VerifyPublicRangeIntegrity(external_index, external.session);
            std::cout << "ProfileTimelineIndex external capture passed (cpu_scopes="
                      << external_index.CpuScopes().size()
                      << ", gpu_frames=" << external_index.GpuFrames().size()
                      << ", gpu_slices=" << external_index.GpuFrameSlices().size()
                      << ", logical_bytes=" << external_index.BuildResult().logical_bytes << ")\n";
            return 0;
        }
        Expect(_argc == 1, "usage: TestProfileTimelineIndex [capture.mpd]");

        ScopedTimelineCapture capture;
        WriteTimelineCapture(capture.Path());
        LoadedTimeline       complete = LoadComplete(capture.Path());
        const LoadedTimeline forensic = LoadForensicPrefix(capture.Path());

        ProfileTimelineIndex index;
        Expect(
            BuildProfileTimelineIndex(complete.session, complete.result, {}, index).Succeeded(),
            "timeline index fixture did not build"
        );
        VerifyCpuQueries(index, complete.session);
        VerifyGpuDomainsAndSlices(index, complete.session);
        VerifyPublicRangeIntegrity(index, complete.session);
        VerifyQualityAndAtomicLimits(complete, forensic);

        ScopedTimelineCapture empty_capture;
        WriteEmptyCapture(empty_capture.Path());
        const LoadedTimeline empty = LoadComplete(empty_capture.Path());
        VerifyEmptySession(empty);
    } catch (const std::exception& error) {
        std::cerr << "ProfileTimelineIndex contract failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "ProfileTimelineIndex contract passed\n";
    return 0;
}
