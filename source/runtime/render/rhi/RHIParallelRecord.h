#ifndef MOER_RENDER_RHI_PARALLEL_RECORD_H
#define MOER_RENDER_RHI_PARALLEL_RECORD_H

#include "RHIRecordDiagnostics.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace Moer::Render {

enum class ParallelRecordFallbackReason : uint8_t {
    None,
    WorkerCountTooSmall,
    NoEligibleLayer,
    InsufficientWork,
    InsufficientConcurrency,
    InvalidWorkDescription,
    InvalidCommandType,
    LayerTooLarge,
};

// Mirrors UE's 64-draw floor, but uses backend-estimated native recording work
// rather than Moer's high-level command count. A single MultiDraw may therefore
// qualify while a long list of cheap copies still remains serial.
inline constexpr uint32_t kDefaultParallelRecordMinWorkUnitsPerJob = 64;

// A borrowed, immutable view of one reorderer layer. Command indices in the
// generated plan are local to this span, which lets the backend bind the plan
// to its own immutable command-pointer storage without copying payloads.
struct ParallelRecordLayerDescription {
    std::span<const Command::EType> command_types;
    // Optional for pure contract tests. Runtime descriptions provide one
    // positive native-recording work estimate per command.
    std::span<const uint32_t> command_work_units;
    // Runtime-only constraints (for example, commands that own GPU timestamp
    // queries) can keep an otherwise whitelisted layer on the coordinator.
    bool force_serial{false};
};

// One recorder owns exactly one contiguous command range. Jobs are emitted in
// deterministic merge/submission order: layer first, then command_begin.
struct ParallelRecordJobRange {
    uint32_t layer_index{0};
    uint32_t command_begin{0};
    uint32_t command_count{0};
    uint64_t work_units{0};

    constexpr uint32_t CommandEnd() const {
        return command_begin + command_count;
    }
};

struct ParallelRecordLayerPlan {
    uint32_t layer_index{0};
    uint32_t command_count{0};
    uint32_t first_job{0};
    uint32_t job_count{0};
    bool     parallel{false};
};

struct ParallelRecordPlan {
    ParallelRecordFallbackReason          fallback_reason{ParallelRecordFallbackReason::NoEligibleLayer};
    std::vector<ParallelRecordLayerPlan> layers;
    std::vector<ParallelRecordJobRange>  jobs;
    uint32_t                             parallel_layer_count{0};

    constexpr bool CanRecordInParallel() const {
        return fallback_reason == ParallelRecordFallbackReason::None;
    }
};

inline ParallelRecordPlan BuildParallelRecordPlan(
    std::span<const ParallelRecordLayerDescription> _layers,
    uint32_t                                        _worker_count,
    uint32_t _min_work_units_per_job = kDefaultParallelRecordMinWorkUnitsPerJob
) {
    ParallelRecordPlan plan;
    if (_worker_count < 2) {
        plan.fallback_reason = ParallelRecordFallbackReason::WorkerCountTooSmall;
        return plan;
    }

    _min_work_units_per_job = std::max(1u, _min_work_units_per_job);

    std::vector<bool>                                layer_is_parallel;
    std::vector<std::vector<ParallelRecordJobRange>> layer_jobs;
    std::vector<uint64_t>                            layer_total_work;
    layer_is_parallel.reserve(_layers.size());
    layer_jobs.reserve(_layers.size());
    layer_total_work.reserve(_layers.size());
    bool saw_insufficient_work        = false;
    bool saw_insufficient_concurrency = false;
    for (size_t layer_index = 0; layer_index < _layers.size(); ++layer_index) {
        const ParallelRecordLayerDescription& layer = _layers[layer_index];
        if (layer.command_types.size() > std::numeric_limits<uint32_t>::max()) {
            plan.fallback_reason = ParallelRecordFallbackReason::LayerTooLarge;
            return plan;
        }
        if (!layer.command_work_units.empty() &&
            layer.command_work_units.size() != layer.command_types.size()) {
            plan.fallback_reason = ParallelRecordFallbackReason::InvalidWorkDescription;
            return plan;
        }

        // Recording independence is weaker than GPU execution independence.
        // Once the coordinator has resolved every layer's state transitions,
        // safe bodies from different ordered layers may be translated on
        // different CPU workers and still be submitted in original layer
        // order. A singleton layer therefore contributes one recording job.
        bool eligible = !layer.force_serial && !layer.command_types.empty();
        for (const Command::EType type : layer.command_types) {
            if (static_cast<size_t>(type) >= static_cast<size_t>(Command::EType::Count)) {
                plan.fallback_reason = ParallelRecordFallbackReason::InvalidCommandType;
                return plan;
            }
            eligible &= GetCommandRecordTraits(type).capability ==
                        RecordCapability::ParallelPrimarySafe;
        }
        uint64_t total_work = 0;
        for (size_t command_index = 0; command_index < layer.command_types.size(); ++command_index) {
            const uint32_t work = layer.command_work_units.empty() ?
                                      1u :
                                      std::max(1u, layer.command_work_units[command_index]);
            total_work += work;
        }
        std::vector<ParallelRecordJobRange> ranges;
        if (eligible) {
            if (total_work < _min_work_units_per_job) {
                eligible = false;
                saw_insufficient_work = true;
            } else {
                std::vector<uint32_t> weights(layer.command_types.size(), 1u);
                if (!layer.command_work_units.empty()) {
                    for (size_t command_index = 0; command_index < weights.size(); ++command_index) {
                        weights[command_index] =
                            std::max(1u, layer.command_work_units[command_index]);
                    }
                }

                // Positive weights make the maximum feasible group count a
                // greedy property: cutting at the first threshold leaves the
                // most work for every suffix. We still choose boundaries near
                // equal-work targets, but only when that suffix can form all
                // remaining threshold-sized jobs.
                // Precompute the maximum greedy threshold-group capacity for
                // every suffix. This keeps candidate validation O(1) instead
                // of rescanning the suffix for every possible boundary.
                std::vector<uint64_t> prefix_work(weights.size() + 1, 0);
                for (size_t index = 0; index < weights.size(); ++index) {
                    prefix_work[index + 1] = prefix_work[index] + weights[index];
                }
                const size_t invalid_end = weights.size() + 1;
                std::vector<size_t> next_group_end(weights.size(), invalid_end);
                size_t end = 0;
                for (size_t begin = 0; begin < weights.size(); ++begin) {
                    end = std::max(end, begin + 1);
                    while (end <= weights.size() &&
                           prefix_work[end] - prefix_work[begin] <
                               _min_work_units_per_job) {
                        ++end;
                    }
                    if (end <= weights.size()) {
                        next_group_end[begin] = end;
                    }
                }
                std::vector<uint32_t> suffix_group_capacities(weights.size() + 1, 0);
                for (size_t begin = weights.size(); begin-- > 0;) {
                    const size_t next = next_group_end[begin];
                    if (next <= weights.size()) {
                        suffix_group_capacities[begin] =
                            1u + suffix_group_capacities[next];
                    }
                }
                auto suffix_group_capacity = [&](uint32_t _begin) {
                    return suffix_group_capacities[_begin];
                };
                auto try_partition = [&](uint32_t _job_count) {
                    std::vector<ParallelRecordJobRange> candidate;
                    candidate.reserve(_job_count);
                    uint32_t begin = 0;
                    uint64_t remaining_work = total_work;
                    for (uint32_t job_index = 0; job_index + 1 < _job_count; ++job_index) {
                        const uint32_t remaining_jobs = _job_count - job_index;
                        const uint64_t target_work =
                            (remaining_work + remaining_jobs - 1) / remaining_jobs;
                        uint64_t range_work = 0;
                        uint64_t selected_work = 0;
                        uint64_t best_distance = std::numeric_limits<uint64_t>::max();
                        uint32_t selected_end = 0;
                        const uint32_t last_end = static_cast<uint32_t>(weights.size()) -
                                                  (remaining_jobs - 1);
                        for (uint32_t end = begin + 1; end <= last_end; ++end) {
                            range_work += weights[end - 1];
                            if (range_work < _min_work_units_per_job) {
                                continue;
                            }
                            const uint64_t suffix_work = remaining_work - range_work;
                            if (suffix_work <
                                uint64_t(remaining_jobs - 1) * _min_work_units_per_job) {
                                continue;
                            }
                            if (suffix_group_capacity(end) < remaining_jobs - 1) {
                                continue;
                            }
                            const uint64_t distance = range_work > target_work ?
                                                          range_work - target_work :
                                                          target_work - range_work;
                            if (distance < best_distance) {
                                best_distance = distance;
                                selected_end = end;
                                selected_work = range_work;
                            }
                        }
                        if (selected_end == 0) {
                            return std::vector<ParallelRecordJobRange>{};
                        }
                        candidate.push_back({
                            .layer_index = static_cast<uint32_t>(layer_index),
                            .command_begin = begin,
                            .command_count = selected_end - begin,
                            .work_units = selected_work,
                        });
                        begin = selected_end;
                        remaining_work -= selected_work;
                    }
                    if (remaining_work < _min_work_units_per_job || begin >= weights.size()) {
                        return std::vector<ParallelRecordJobRange>{};
                    }
                    candidate.push_back({
                        .layer_index = static_cast<uint32_t>(layer_index),
                        .command_begin = begin,
                        .command_count = static_cast<uint32_t>(weights.size()) - begin,
                        .work_units = remaining_work,
                    });
                    return candidate;
                };

                uint32_t desired_jobs = std::min({
                    _worker_count,
                    static_cast<uint32_t>(weights.size()),
                    static_cast<uint32_t>(std::min<uint64_t>(
                        total_work / _min_work_units_per_job,
                        std::numeric_limits<uint32_t>::max()
                    )),
                });
                desired_jobs = std::max(1u, desired_jobs);
                while (desired_jobs != 0 && ranges.empty()) {
                    ranges = try_partition(desired_jobs--);
                }
                assert(!ranges.empty());
            }
        }
        layer_is_parallel.push_back(eligible);
        layer_jobs.push_back(std::move(ranges));
        layer_total_work.push_back(total_work);
    }

    // The executor drains a recording wave before visiting the next non-empty
    // serial island so translate-time CPU side effects stay ordered. Evaluate
    // profitability on those same wave boundaries: two eligible singletons
    // separated by a serial layer cannot overlap and must not be advertised as
    // effective parallel work. Empty layers do not make the executor flush.
    size_t   wave_begin     = 0;
    uint32_t wave_job_count = 0;
    auto finish_wave = [&](size_t _wave_end) {
        if (wave_job_count != 0 && wave_job_count < 2) {
            for (size_t index = wave_begin; index < _wave_end; ++index) {
                layer_is_parallel[index] = false;
            }
            saw_insufficient_concurrency = true;
        }
        wave_begin     = _wave_end;
        wave_job_count = 0;
    };
    for (size_t layer_index = 0; layer_index < _layers.size(); ++layer_index) {
        if (layer_is_parallel[layer_index]) {
            if (wave_job_count == 0) {
                wave_begin = layer_index;
            }
            wave_job_count += static_cast<uint32_t>(layer_jobs[layer_index].size());
        } else if (!_layers[layer_index].command_types.empty()) {
            finish_wave(layer_index);
            wave_begin = layer_index + 1;
        }
    }
    finish_wave(_layers.size());

    for (const bool parallel : layer_is_parallel) {
        plan.parallel_layer_count += parallel ? 1u : 0u;
    }
    if (plan.parallel_layer_count == 0) {
        plan.fallback_reason = saw_insufficient_concurrency ?
                                   ParallelRecordFallbackReason::InsufficientConcurrency :
                               saw_insufficient_work ?
                                   ParallelRecordFallbackReason::InsufficientWork :
                                   ParallelRecordFallbackReason::NoEligibleLayer;
        return plan;
    }

    plan.layers.reserve(_layers.size());
    for (size_t layer_index = 0; layer_index < _layers.size(); ++layer_index) {
        const uint32_t command_count = static_cast<uint32_t>(_layers[layer_index].command_types.size());
        const bool     parallel      = layer_is_parallel[layer_index];
        const uint32_t job_count = parallel ? static_cast<uint32_t>(layer_jobs[layer_index].size())
                                            : (command_count == 0 ? 0u : 1u);

        ParallelRecordLayerPlan layer_plan{
            .layer_index   = static_cast<uint32_t>(layer_index),
            .command_count = command_count,
            .first_job     = static_cast<uint32_t>(plan.jobs.size()),
            .job_count     = job_count,
            .parallel      = parallel,
        };

        if (parallel) {
            plan.jobs.insert(
                plan.jobs.end(),
                layer_jobs[layer_index].begin(),
                layer_jobs[layer_index].end()
            );
        } else if (command_count != 0) {
            plan.jobs.push_back({
                .layer_index   = static_cast<uint32_t>(layer_index),
                .command_begin = 0,
                .command_count = command_count,
                .work_units    = layer_total_work[layer_index],
            });
        }

        plan.layers.push_back(layer_plan);
    }

    plan.fallback_reason = ParallelRecordFallbackReason::None;
    return plan;
}

} // namespace Moer::Render

#endif
