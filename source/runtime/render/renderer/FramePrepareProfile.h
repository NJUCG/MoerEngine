#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace Moer::Render {

using FramePrepareProfileClock = std::chrono::steady_clock;

struct FramePrepareProfile {
    double window_ms          = 0.0;
    double scripting_ms       = 0.0;
    double test_ms            = 0.0;
    double ui_tick_ms         = 0.0;
    double camera_and_test_ms = 0.0;
    double config_snapshot_ms = 0.0;
    double scene_update_ms    = 0.0;
    double scene_snapshot_ms  = 0.0;
    double ui_composition_ms  = 0.0;
    double ui_draw_packet_ms  = 0.0;

    [[nodiscard]] double MeasuredTotalMilliseconds() const {
        return window_ms + scripting_ms + test_ms + ui_tick_ms + camera_and_test_ms + config_snapshot_ms +
               scene_update_ms + scene_snapshot_ms + ui_composition_ms + ui_draw_packet_ms;
    }

    void Accumulate(const FramePrepareProfile& other) {
        window_ms += other.window_ms;
        scripting_ms += other.scripting_ms;
        test_ms += other.test_ms;
        ui_tick_ms += other.ui_tick_ms;
        camera_and_test_ms += other.camera_and_test_ms;
        config_snapshot_ms += other.config_snapshot_ms;
        scene_update_ms += other.scene_update_ms;
        scene_snapshot_ms += other.scene_snapshot_ms;
        ui_composition_ms += other.ui_composition_ms;
        ui_draw_packet_ms += other.ui_draw_packet_ms;
    }
};

struct FramePrepareWorkload {
    uint64_t scene_ready_frames          = 0;
    uint64_t scene_dirty_frames          = 0;
    uint64_t initial_gpu_update_frames   = 0;
    uint64_t update_gpu_update_frames    = 0;
    uint64_t geometry_snapshot_frames    = 0;
    uint64_t scene_snapshot_build_frames = 0;
    uint64_t ui_vertices                 = 0;
    uint64_t ui_indices                  = 0;
    uint64_t ui_commands                 = 0;
    uint64_t ui_vertices_max             = 0;

    void Accumulate(const FramePrepareWorkload& other) {
        scene_ready_frames += other.scene_ready_frames;
        scene_dirty_frames += other.scene_dirty_frames;
        initial_gpu_update_frames += other.initial_gpu_update_frames;
        update_gpu_update_frames += other.update_gpu_update_frames;
        geometry_snapshot_frames += other.geometry_snapshot_frames;
        scene_snapshot_build_frames += other.scene_snapshot_build_frames;
        ui_vertices += other.ui_vertices;
        ui_indices += other.ui_indices;
        ui_commands += other.ui_commands;
        ui_vertices_max = std::max(ui_vertices_max, other.ui_vertices_max);
    }
};

// This state is allocated only while profile logging is enabled. Keeping it outside the
// Renderer object prevents GT-only counters from sharing a cache line with RT frame state.
struct FramePrepareProfileState {
    FramePrepareProfileClock::time_point window_start{};
    uint64_t                             samples               = 0;
    double                               total_ms              = 0.0;
    double                               total_max_ms          = 0.0;
    double                               other_ms              = 0.0;
    double                               scene_update_max_ms   = 0.0;
    double                               scene_snapshot_max_ms = 0.0;
    double                               ui_draw_packet_max_ms = 0.0;
    FramePrepareProfile                  accumulated{};
    FramePrepareWorkload                 workload{};
};

class ScopedFramePrepareProfileTimer {
public:
    ScopedFramePrepareProfileTimer(bool enabled, double& accumulator) :
        accumulator(enabled ? &accumulator : nullptr) {
        if (this->accumulator != nullptr) {
            started_at = FramePrepareProfileClock::now();
        }
    }

    ~ScopedFramePrepareProfileTimer() {
        if (accumulator == nullptr) {
            return;
        }
        *accumulator +=
            std::chrono::duration<double, std::milli>(FramePrepareProfileClock::now() - started_at).count();
    }

    ScopedFramePrepareProfileTimer(const ScopedFramePrepareProfileTimer&)            = delete;
    ScopedFramePrepareProfileTimer& operator=(const ScopedFramePrepareProfileTimer&) = delete;

private:
    double*                              accumulator = nullptr;
    FramePrepareProfileClock::time_point started_at{};
};

} // namespace Moer::Render
