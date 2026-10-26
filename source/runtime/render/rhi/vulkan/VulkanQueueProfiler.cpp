#include "VulkanQueueTiming.h"
#include "VulkanQueue.h"
#include "VulkanDevice.h"
#include "log/LogSystem.h"
#include "rhi/RHIThreadHeartbeat.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace Moer::Render {

ProfilerStorage::ProfilerStorage(VkNativeQueryPool& _pool) : timestamp_pool(_pool) {

    std::memset(queries_used, 0, sizeof(queries_used));
    std::memset(query_pool_results, 0, sizeof(query_pool_results));
    timestamp_period = timestamp_pool.GetDevice().GetCoreProperties().core_1_0.limits.timestampPeriod;
    active           = true;
    name2sample.reserve(100);
}
void ProfilerStorage::CollectProfiling(VkCommandBuffer _cb) {
    if (!active) {
        return;
    }
    bool b_any_query_used = false;
    //maybe should use last frame
    for (uint idx = 0; idx < s_max_num_profiler_queries_per_frame / 8; ++idx) {
        if (queries_used[idx + cur_frame * s_max_num_profiler_queries_per_frame / 8]) {
            b_any_query_used = true;
            break;
        }
    }
    if (b_any_query_used) {
        VkResult result = vkGetQueryPoolResults(
            timestamp_pool.GetDevice().GetDevice(),
            timestamp_pool.GetHandle(),
            s_max_num_profiler_queries_per_frame * cur_frame,
            s_max_num_profiler_queries_per_frame,
            sizeof(query_pool_results),
            query_pool_results,
            sizeof(query_pool_results[0]) * 2, // each result contain two int
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
        );

        if (result != VK_SUCCESS && result != VK_NOT_READY) {
            LOG_ERROR("Failed call to vkGetQueryPoolResults, error code = {}\n", uint64(result));
            b_any_query_used = false;
        }
    }

    if (b_any_query_used) {
        for (auto& [name, sample] : name2sample) {
            // clang-format off
                if (IsQueryUsed (sample.index * 2 + 0 + cur_frame * s_max_num_profiler_queries_per_frame)
                    && IsQueryUsed(sample.index * 2 + 1 + cur_frame * s_max_num_profiler_queries_per_frame)
                    && query_pool_results[sample.index * 2 * 2 + 1] != 0 // first *2 for begin/end, second *2 for query result
                    && query_pool_results[(sample.index * 2 + 1) * 2 + 1] != 0) {
                // clang-format on

                uint64_t begin = query_pool_results[sample.index * 2 * 2 + 0];
                uint64_t end   = query_pool_results[(sample.index * 2 + 1) * 2 + 0];
                sample.Record(end - begin);
            } else {
                // not valid
                sample.Reset();
            }
        }
    } else {
        memset(query_pool_results, 0, sizeof(query_pool_results));
    }

    vkCmdResetQueryPool(
        _cb,
        timestamp_pool.GetHandle(),
        s_max_num_profiler_queries_per_frame * cur_frame,
        s_max_num_profiler_queries_per_frame
    );

    memset(
        queries_used + s_max_num_profiler_queries_per_frame * cur_frame / 8,
        0,
        s_max_num_profiler_queries_per_frame / 8
    );
}

int ProfilerStorage::GetQueryStorageIndex(std::string_view _name) {
    const std::string name{_name};
    if (const auto found = name2sample.find(name); found != name2sample.end()) {
        return found->second.index;
    }
    if (name2sample.size() >= s_query_max_storage) {
        LOG_ERROR(
            "GPU profiler query-name capacity ({}) exhausted; skipping timestamp scope '{}'.",
            s_query_max_storage,
            name
        );
        return -1;
    }
    const int index = static_cast<int>(name2sample.size());
    name2sample.emplace(name, Sample(index));
    return index;
}

void ProfilerStorage::BeginProfilerSession(
    VulkanCommandList&      _cmd_list,
    std::string_view        _name,
    VkPipelineStageFlagBits _stage
) {
    if (!active) {
        return;
    }
    const int storage_index = GetQueryStorageIndex(_name);
    if (storage_index < 0) {
        return;
    }
    uint idx = storage_index * 2 + 0 + cur_frame * s_max_num_profiler_queries_per_frame;
    vkCmdWriteTimestamp(_cmd_list.GetHandle(), _stage, timestamp_pool.GetHandle(), idx);
    SetQueryUsed(idx);
    assert(IsQueryUsed(idx + 1) == false && "Query already used");
}

void ProfilerStorage::EndProfilerSession(
    VulkanCommandList&      _cmd_list,
    std::string_view        _name,
    VkPipelineStageFlagBits _stage
) {
    if (!active) {
        return;
    }
    const int storage_index = GetQueryStorageIndex(_name);
    if (storage_index < 0) {
        return;
    }
    uint idx = storage_index * 2 + 1 + cur_frame * s_max_num_profiler_queries_per_frame;
    vkCmdWriteTimestamp(_cmd_list.GetHandle(), _stage, timestamp_pool.GetHandle(), idx);
    SetQueryUsed(idx);
    assert(IsQueryUsed(idx - 1) == true && "Query not used");
}

QueryFrameDiagnostics ProfilerStorage::GetCurrentFrameQueryDiagnostics() const {
    QueryFrameDiagnostics diagnostics;
    StableRecordHash      hash;
    const uint32 frame_base = cur_frame * s_max_num_profiler_queries_per_frame;
    for (uint32 query = 0; query < s_max_num_profiler_queries_per_frame; ++query) {
        const uint32 absolute_query = frame_base + query;
        if ((queries_used[absolute_query / 8] & (1u << (absolute_query % 8))) == 0) {
            continue;
        }
        ++diagnostics.used_query_count;
        hash.Add(query);
    }
    hash.Add(diagnostics.used_query_count);
    diagnostics.digest = hash.Value();
    return diagnostics;
}

void VkCommandQueue::BeginSplitProfilingCpuFrame() {
    ResetSplitProfilingCpuFrame();
    split_profiling_cpu_frame.active  = true;
    split_profiling_cpu_frame.started = std::chrono::steady_clock::now();
}

void VkCommandQueue::AccumulateSplitProfilingCpuFrame(
    double _reorder_ms,
    double _preprocess_ms
) {
    // Malformed phase sequences must still publish finite CPU diagnostics.
    // Starting here does not repair the missing GPU Begin query, but avoids an
    // uninitialized clock if an End packet is used defensively on its own.
    if (!split_profiling_cpu_frame.active) {
        split_profiling_cpu_frame.active  = true;
        split_profiling_cpu_frame.started = std::chrono::steady_clock::now();
    }
    split_profiling_cpu_frame.reorder_ms += _reorder_ms;
    split_profiling_cpu_frame.preprocess_ms += _preprocess_ms;
}

void VkCommandQueue::EndSplitProfilingCpuFrame() {
    if (!split_profiling_cpu_frame.active) {
        split_profiling_cpu_frame.active  = true;
        split_profiling_cpu_frame.started = std::chrono::steady_clock::now();
    }

    const double execute_ms = RhiThreadProfileMilliseconds(
        split_profiling_cpu_frame.started, std::chrono::steady_clock::now()
    );
    profiler_storage.RegisterCpuTimestamp("Queue Execution", execute_ms);
    profiler_storage.RegisterCpuTimestamp(
        "Command Reorder", split_profiling_cpu_frame.reorder_ms
    );
    profiler_storage.RegisterCpuTimestamp(
        "Command Preprocess", split_profiling_cpu_frame.preprocess_ms
    );
    if (execute_ms > 0.0) {
        profiler_storage.RegisterCpuTimestamp(
            "Reorder Percentage", split_profiling_cpu_frame.reorder_ms / execute_ms
        );
        profiler_storage.RegisterCpuTimestamp(
            "Preprocess Percentage", split_profiling_cpu_frame.preprocess_ms / execute_ms
        );
    }
    profiler_storage.AdvanceFrame();
    ResetSplitProfilingCpuFrame();
}

void VkCommandQueue::ResetSplitProfilingCpuFrame() {
    split_profiling_cpu_frame = {};
}

static uint64 CanonicalDigest(const UnorderedSet<uint64>& _digests) {
    Array<uint64> sorted_digests(_digests.begin(), _digests.end());
    std::sort(sorted_digests.begin(), sorted_digests.end());

    StableRecordHash hash;
    hash.Add(sorted_digests.size());
    for (const uint64 digest : sorted_digests) {
        hash.Add(digest);
    }
    return hash.Value();
}

static std::string JoinRecordCounts(std::span<const uint32> _counts) {
    std::string result;
    for (size_t index = 0; index < _counts.size(); ++index) {
        if (index != 0) {
            result += ',';
        }
        result += std::to_string(_counts[index]);
    }
    return result;
}

ProfileData VkCommandQueue::GetProfilerEntry() {
    std::unique_lock<std::mutex> lock(profiler_mutex);
    return cached_profiler_entry;
}

void VkCommandQueue::EnsureRecordCalibration() {
    if (!thread_profile || thread_profile->calibration_ready ||
        queue.GetType() != EQueueType::Graphics) {
        return;
    }

    RhiThreadProfileState& profile = *thread_profile;
    profile.model_workers = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));

    auto fail_calibration = [&](std::string_view _reason) {
        profile.dispatch_join_median_ms = 1000000.0;
        profile.dispatch_join_tail_ms   = 1000000.0;
        profile.calibration_ready       = true;
        LOG_ERROR(
            "[ThreadingProfile][RHIRecordCalibration] queue=Graphics model_workers={} "
            "status=failed estimate_basis=conservative reason={}",
            profile.model_workers,
            _reason
        );
    };

    constexpr uint32 warmup_count = 16;
    constexpr uint32 sample_count = 64;
    try {
        ExternalCpuJoinPool join_pool(profile.model_workers);
        Array<ExternalCpuJoinPool::Job> jobs;
        jobs.reserve(profile.model_workers);
        for (uint32 worker = 0; worker < profile.model_workers; ++worker) {
            jobs.emplace_back([] {});
        }
        const std::span<const ExternalCpuJoinPool::Job> job_span(jobs.data(), jobs.size());

        for (uint32 warmup = 0; warmup < warmup_count; ++warmup) {
            if (join_pool.RunAndWait(job_span) != ExternalJoinResult::Completed) {
                fail_calibration("warmup_join");
                return;
            }
        }

        Array<double> samples;
        samples.reserve(sample_count);
        for (uint32 sample = 0; sample < sample_count; ++sample) {
            const auto started = std::chrono::steady_clock::now();
            if (join_pool.RunAndWait(job_span) != ExternalJoinResult::Completed) {
                fail_calibration("sample_join");
                return;
            }
            samples.push_back(
                RhiThreadProfileMilliseconds(started, std::chrono::steady_clock::now())
            );
        }
        std::sort(samples.begin(), samples.end());
        const size_t median_index = samples.size() / 2;
        const size_t tail_index   = (samples.size() * 95 + 99) / 100 - 1;
        profile.dispatch_join_median_ms = samples[median_index];
        profile.dispatch_join_tail_ms   = samples[tail_index];
        profile.calibration_ready       = true;

        LOG_INFO(
            "[ThreadingProfile][RHIRecordCalibration] queue=Graphics model_workers={} warmup={} "
            "samples={} dispatch_join_median_ms={:.6f} dispatch_join_tail_ms={:.6f} "
            "estimate_basis=p95 status=completed",
            profile.model_workers,
            warmup_count,
            sample_count,
            profile.dispatch_join_median_ms,
            profile.dispatch_join_tail_ms
        );
    } catch (const std::exception& error) {
        fail_calibration(error.what());
    } catch (...) {
        fail_calibration("unknown_exception");
    }
}

void VkCommandQueue::RecordRhiRecordProfile(const RhiRecordExecuteSample& _sample) {
    if (!thread_profile) {
        return;
    }

    RhiThreadProfileState& profile = *thread_profile;
    const RecordPrediction prediction = PredictParallelRecordCriticalPath(
        _sample.layer_timings, profile.model_workers, profile.dispatch_join_tail_ms
    );
    RhiRecordWindowTotals& totals = profile.record;
    ++totals.samples;
    totals.serial_record_wall_total_ms += prediction.serial_record_wall_ms;
    totals.serial_record_wall_max_ms =
        std::max(totals.serial_record_wall_max_ms, prediction.serial_record_wall_ms);
    totals.serial_command_sum_total_ms += _sample.serial_command_sum_ms;
    totals.eligible_record_total_ms += prediction.eligible_record_ms;
    totals.predicted_critical_total_ms += prediction.predicted_critical_ms;
    totals.dispatch_join_estimate_total_ms += prediction.dispatch_join_estimate_ms;
    totals.predicted_net_saving_total_ms += prediction.predicted_net_saving_ms;
    totals.layer_total += _sample.topology.layer_count;
    totals.command_total += _sample.topology.command_count;
    totals.candidate_command_total += _sample.topology.candidate_command_count;
    totals.safe_command_total += _sample.topology.safe_command_count;
    totals.parallel_layer_total += prediction.parallel_layer_count;
    totals.descriptor_bytes_total += _sample.descriptor_bytes;
    totals.buffer_barrier_total += _sample.buffer_barriers;
    totals.texture_barrier_total += _sample.texture_barriers;
    totals.memory_barrier_total += _sample.memory_barriers;
    totals.used_query_total += _sample.used_queries;
    totals.layer_max = std::max(totals.layer_max, _sample.topology.layer_count);
    totals.command_max = std::max(totals.command_max, _sample.topology.command_count);
    totals.parallel_layer_max =
        std::max(totals.parallel_layer_max, prediction.parallel_layer_count);
    totals.command_digests.insert(_sample.topology.command_digest);
    totals.layer_digests.insert(_sample.topology.layer_digest);
    totals.barrier_digests.insert(_sample.barrier_hash.Value());
    totals.descriptor_digests.insert(_sample.descriptor_digest);
    totals.query_digests.insert(_sample.query_digest);
    if (_sample.serial_golden.summary.complete) {
        ++totals.golden_complete;
    } else {
        ++totals.golden_incomplete;
    }
    totals.golden_unresolved_total += _sample.serial_golden.unresolved_count;
    totals.golden_opaque_total += _sample.serial_golden.opaque_count;
    totals.golden_command_digests.insert(_sample.serial_golden.summary.command_digest);
    totals.golden_layer_digests.insert(_sample.serial_golden.summary.layer_digest);
    totals.golden_barrier_digests.insert(_sample.serial_golden.summary.barrier_digest);
    totals.golden_descriptor_digests.insert(_sample.serial_golden.summary.descriptor_digest);
    totals.golden_query_digests.insert(_sample.serial_golden.summary.query_digest);
    totals.golden_combined_digests.insert(_sample.serial_golden.summary.combined_digest);
    StableRecordHash manifest_entry_hash;
    manifest_entry_hash.Add(_sample.topology.topology_digest);
    manifest_entry_hash.Add(_sample.serial_golden.summary.combined_digest);
    manifest_entry_hash.Add(_sample.serial_golden.summary.complete ? 1u : 0u);
    totals.golden_manifest_entries.insert(manifest_entry_hash.Value());
    const bool is_new_manifest_entry =
        profile.observed_golden_manifests.insert(manifest_entry_hash.Value()).second;

    if (is_new_manifest_entry) {
        const SerialBarrierItem first_unresolved =
            _sample.serial_golden.first_unresolved_buffer_barrier.value_or(SerialBarrierItem{});
        LOG_INFO(
            "[ThreadingProfile][RHIRecordGolden] queue=Graphics schema=1 "
            "identity=submission-alpha complete={} topology_digest={:016x} "
            "combined_digest={:016x} command_digest={:016x} layer_digest={:016x} "
            "barrier_digest={:016x} descriptor_digest={:016x} query_digest={:016x} "
            "commands={} layers={} barriers={} descriptors={} queries={} unresolved={} opaque={} "
            "unresolved_command_mask={:016x} opaque_command_mask={:016x} "
            "unresolved_native_buffers={} unresolved_native_images={} "
            "first_unresolved_buffer_group={} first_unresolved_buffer_src_stage={:x} "
            "first_unresolved_buffer_dst_stage={:x} first_unresolved_buffer_src_access={:x} "
            "first_unresolved_buffer_dst_access={:x} first_unresolved_buffer_offset={} "
            "first_unresolved_buffer_size={}",
            _sample.serial_golden.summary.complete ? 1 : 0,
            _sample.topology.topology_digest,
            _sample.serial_golden.summary.combined_digest,
            _sample.serial_golden.summary.command_digest,
            _sample.serial_golden.summary.layer_digest,
            _sample.serial_golden.summary.barrier_digest,
            _sample.serial_golden.summary.descriptor_digest,
            _sample.serial_golden.summary.query_digest,
            _sample.serial_golden.summary.command_count,
            _sample.serial_golden.summary.layer_count,
            _sample.serial_golden.summary.barrier_count,
            _sample.serial_golden.summary.descriptor_count,
            _sample.serial_golden.summary.query_count,
            _sample.serial_golden.unresolved_count,
            _sample.serial_golden.opaque_count,
            _sample.serial_golden.unresolved_command_mask,
            _sample.serial_golden.opaque_command_mask,
            _sample.serial_golden.unresolved_native_buffers,
            _sample.serial_golden.unresolved_native_images,
            first_unresolved.group_ordinal,
            first_unresolved.src_stage_mask,
            first_unresolved.dst_stage_mask,
            first_unresolved.src_access_mask,
            first_unresolved.dst_access_mask,
            first_unresolved.range_offset,
            first_unresolved.range_size
        );
    }

    const bool topology_changed =
        profile.has_topology && profile.last_topology_digest != _sample.topology.topology_digest;
    if (!profile.has_topology || topology_changed) {
        LOG_INFO(
            "[ThreadingProfile][RHIRecordTopology] queue=Graphics topology_digest={:016x} "
            "command_digest={:016x} layer_digest={:016x} layers={} commands={} "
            "candidate_commands={} safe_commands={} layer_commands={} layer_candidates={} "
            "change={}",
            _sample.topology.topology_digest,
            _sample.topology.command_digest,
            _sample.topology.layer_digest,
            _sample.topology.layer_count,
            _sample.topology.command_count,
            _sample.topology.candidate_command_count,
            _sample.topology.safe_command_count,
            JoinRecordCounts(_sample.topology.layer_command_counts),
            JoinRecordCounts(_sample.topology.layer_candidate_counts),
            topology_changed ? 1 : 0
        );
    }
    if (topology_changed) {
        ++totals.topology_changes;
    }
    profile.has_topology         = true;
    profile.last_topology_digest = _sample.topology.topology_digest;
}

void VkCommandQueue::RecordParallelRecordProfile(
    const ParallelRecordProfileSample& _sample
) {
    if (!parallel_record_profile) {
        return;
    }
    assert(!_sample.planned || _sample.requested);
    assert(!_sample.effective || _sample.planned);
    assert(
        !_sample.worker_fallback ||
        (_sample.planned && !_sample.effective)
    );

    ParallelRecordProfileState& profile = *parallel_record_profile;
    ++profile.samples;
    profile.requested += _sample.requested ? 1u : 0u;
    profile.planned += _sample.planned ? 1u : 0u;
    profile.effective += _sample.effective ? 1u : 0u;
    profile.worker_fallbacks += _sample.worker_fallback ? 1u : 0u;
    profile.record_samples_ms.push_back(_sample.record_wall_ms);
    profile.execute_cpu_samples_ms.push_back(_sample.execute_cpu_wall_ms);
    profile.reorder_total_ms += _sample.reorder_ms;
    profile.preprocess_total_ms += _sample.preprocess_ms;
    profile.worker_join_total_ms += _sample.worker_join_ms;
    profile.submit_cpu_total_ms += _sample.submit_cpu_ms;
    profile.layer_total += _sample.layers;
    profile.job_total += _sample.jobs;
    profile.work_unit_total += _sample.work_units;
    profile.ordered_cb_total += _sample.ordered_cb;
    profile.secondary_cb_total += _sample.secondary_cb;
    profile.max_active = std::max(profile.max_active, _sample.max_active);

    const auto now = std::chrono::steady_clock::now();
    if (RhiThreadProfileMilliseconds(profile.window_start, now) >= 1000.0) {
        FlushParallelRecordProfile();
    }
}

void VkCommandQueue::FlushParallelRecordProfile() {
    if (!parallel_record_profile || parallel_record_profile->samples == 0) {
        return;
    }

    ParallelRecordProfileState& profile = *parallel_record_profile;
    const auto now = std::chrono::steady_clock::now();
    const double window_ms = RhiThreadProfileMilliseconds(profile.window_start, now);
    Array<double> sorted_record = profile.record_samples_ms;
    Array<double> sorted_execute_cpu = profile.execute_cpu_samples_ms;
    std::sort(sorted_record.begin(), sorted_record.end());
    std::sort(sorted_execute_cpu.begin(), sorted_execute_cpu.end());
    assert(sorted_record.size() == profile.samples);
    assert(sorted_execute_cpu.size() == profile.samples);
    auto percentile = [](const Array<double>& _sorted, double _fraction) {
        const size_t index = static_cast<size_t>(
            std::ceil(_fraction * static_cast<double>(_sorted.size() - 1))
        );
        return _sorted[index];
    };
    const double sample_count = static_cast<double>(profile.samples);
    const bool all_samples_parallel = profile.planned == profile.samples &&
                                      profile.effective == profile.planned;
    const std::string_view mode = profile.requested == 0 ? "serial" :
                                  all_samples_parallel    ? "parallel" :
                                                            "mixed";

    LOG_INFO(
        "[ParallelRecordProfile] queue=Graphics mode={} window_ms={:.3f} samples={} "
        "requested={} planned={} effective={} worker_fallbacks={} record_p50_ms={:.6f} "
        "record_p95_ms={:.6f} record_p99_ms={:.6f} record_max_ms={:.6f} "
        "execute_cpu_p50_ms={:.6f} execute_cpu_p95_ms={:.6f} "
        "execute_cpu_p99_ms={:.6f} execute_cpu_max_ms={:.6f} "
        "reorder_avg_ms={:.6f} preprocess_avg_ms={:.6f} worker_join_avg_ms={:.6f} "
        "submit_cpu_avg_ms={:.6f} layers_avg={:.3f} jobs_avg={:.3f} "
        "work_units_avg={:.3f} ordered_cb_avg={:.3f} secondary_cb_avg={:.3f} max_active={}",
        mode,
        window_ms,
        profile.samples,
        profile.requested,
        profile.planned,
        profile.effective,
        profile.worker_fallbacks,
        percentile(sorted_record, 0.50),
        percentile(sorted_record, 0.95),
        percentile(sorted_record, 0.99),
        sorted_record.back(),
        percentile(sorted_execute_cpu, 0.50),
        percentile(sorted_execute_cpu, 0.95),
        percentile(sorted_execute_cpu, 0.99),
        sorted_execute_cpu.back(),
        profile.reorder_total_ms / sample_count,
        profile.preprocess_total_ms / sample_count,
        profile.worker_join_total_ms / sample_count,
        profile.submit_cpu_total_ms / sample_count,
        static_cast<double>(profile.layer_total) / sample_count,
        static_cast<double>(profile.job_total) / sample_count,
        static_cast<double>(profile.work_unit_total) / sample_count,
        static_cast<double>(profile.ordered_cb_total) / sample_count,
        static_cast<double>(profile.secondary_cb_total) / sample_count,
        profile.max_active
    );

    profile.window_start = now;
    profile.record_samples_ms.clear();
    profile.execute_cpu_samples_ms.clear();
    profile.samples = 0;
    profile.requested = 0;
    profile.planned = 0;
    profile.effective = 0;
    profile.worker_fallbacks = 0;
    profile.reorder_total_ms = 0.0;
    profile.preprocess_total_ms = 0.0;
    profile.worker_join_total_ms = 0.0;
    profile.submit_cpu_total_ms = 0.0;
    profile.layer_total = 0;
    profile.job_total = 0;
    profile.work_unit_total = 0;
    profile.ordered_cb_total = 0;
    profile.secondary_cb_total = 0;
    profile.max_active = 0;
}

void VkCommandQueue::RecordThreadingProfile(
    ERhiWorkKind _kind,
    double       _caller_ms,
    double       _queue_wait_ms,
    double       _work_ms,
    uint32       _enqueue_depth
) {
    if (!thread_profile) {
        return;
    }

    RhiThreadProfileState& profile = *thread_profile;
    ++profile.samples;
    RhiWorkProfileTotals& kind_profile = _kind == ERhiWorkKind::Execute
                                            ? profile.execute
                                            : profile.present;
    ++kind_profile.samples;
    kind_profile.caller_total_ms += _caller_ms;
    kind_profile.queue_wait_total_ms += _queue_wait_ms;
    kind_profile.work_total_ms += _work_ms;
    profile.caller_total_ms += _caller_ms;
    profile.caller_max_ms = std::max(profile.caller_max_ms, _caller_ms);
    profile.queue_wait_total_ms += _queue_wait_ms;
    profile.queue_wait_max_ms = std::max(profile.queue_wait_max_ms, _queue_wait_ms);
    profile.work_total_ms += _work_ms;
    profile.work_max_ms = std::max(profile.work_max_ms, _work_ms);
    profile.max_enqueue_depth = std::max(profile.max_enqueue_depth, _enqueue_depth);

    const auto   now       = std::chrono::steady_clock::now();
    const double window_ms = RhiThreadProfileMilliseconds(profile.window_start, now);
    if (window_ms < 1000.0) {
        return;
    }

    const uint64 submitted_timeline = last_frame.load(std::memory_order_acquire);
    const uint64 completed_timeline = cpu_settled_frame.load(std::memory_order_acquire);
    const uint64 gpu_pending = submitted_timeline > completed_timeline
                                   ? submitted_timeline - completed_timeline
                                   : 0;
    auto average = [](double _total, uint64 _samples) {
        return _samples == 0 ? 0.0 : _total / double(_samples);
    };
    LOG_INFO(
        "[ThreadingProfile][RHI] queue={} mode={} window_ms={:.3f} samples={} execute={} "
        "present={} caller_avg_ms={:.3f} caller_max_ms={:.3f} queue_wait_avg_ms={:.3f} "
        "queue_wait_max_ms={:.3f} work_avg_ms={:.3f} work_max_ms={:.3f} "
        "execute_caller_avg_ms={:.3f} execute_wait_avg_ms={:.3f} execute_work_avg_ms={:.3f} "
        "present_caller_avg_ms={:.3f} present_wait_avg_ms={:.3f} present_work_avg_ms={:.3f} "
        "max_enqueue_depth={} gpu_pending={}",
        queue.GetType() == EQueueType::Graphics ? "Graphics" : "Compute",
        rhi_thread_enabled ? "threaded" : "synchronous",
        window_ms,
        profile.samples,
        profile.execute.samples,
        profile.present.samples,
        profile.caller_total_ms / double(profile.samples),
        profile.caller_max_ms,
        profile.queue_wait_total_ms / double(profile.samples),
        profile.queue_wait_max_ms,
        profile.work_total_ms / double(profile.samples),
        profile.work_max_ms,
        average(profile.execute.caller_total_ms, profile.execute.samples),
        average(profile.execute.queue_wait_total_ms, profile.execute.samples),
        average(profile.execute.work_total_ms, profile.execute.samples),
        average(profile.present.caller_total_ms, profile.present.samples),
        average(profile.present.queue_wait_total_ms, profile.present.samples),
        average(profile.present.work_total_ms, profile.present.samples),
        profile.max_enqueue_depth,
        gpu_pending
    );

    const RhiRecordWindowTotals& record = profile.record;
    if (record.samples > 0) {
        const double record_samples = double(record.samples);
        const double predicted_net_pct = record.serial_record_wall_total_ms > 0.0
                                                 ? record.predicted_net_saving_total_ms /
                                                       record.serial_record_wall_total_ms * 100.0
                                                 : 0.0;
        LOG_INFO(
            "[ThreadingProfile][RHIRecord] queue=Graphics mode={} window_ms={:.3f} samples={} "
            "model_workers={} layers_avg={:.3f} layers_max={} commands_avg={:.3f} commands_max={} "
            "candidate_commands_avg={:.3f} safe_commands_avg={:.3f} parallel_layers_avg={:.3f} "
            "parallel_layers_max={} serial_record_wall_avg_ms={:.3f} "
            "serial_record_wall_max_ms={:.3f} serial_command_sum_avg_ms={:.3f} "
            "eligible_record_avg_ms={:.3f} predicted_critical_avg_ms={:.3f} "
            "dispatch_join_est_avg_ms={:.3f} predicted_net_avg_ms={:.3f} predicted_net_pct={:.3f} "
            "descriptor_bytes_avg={:.3f} buffer_barriers_avg={:.3f} texture_barriers_avg={:.3f} "
            "memory_barriers_avg={:.3f} used_queries_avg={:.3f} "
            "command_digest={:016x} command_variants={} layer_digest={:016x} layer_variants={} "
            "barrier_digest={:016x} barrier_variants={} descriptor_digest={:016x} "
            "descriptor_variants={} query_digest={:016x} query_variants={} topology_changes={} "
            "golden_complete={} golden_incomplete={} golden_unresolved={} golden_opaque={} "
            "golden_command_digest={:016x} golden_command_variants={} "
            "golden_layer_digest={:016x} golden_layer_variants={} "
            "golden_barrier_digest={:016x} golden_barrier_variants={} "
            "golden_descriptor_digest={:016x} golden_descriptor_variants={} "
            "golden_query_digest={:016x} golden_query_variants={} "
            "golden_combined_digest={:016x} golden_combined_variants={} "
            "golden_manifest_digest={:016x} golden_manifest_variants={} "
            "calibration_tail_ms={:.6f}",
            rhi_thread_enabled ? "threaded" : "synchronous",
            window_ms,
            record.samples,
            profile.model_workers,
            double(record.layer_total) / record_samples,
            record.layer_max,
            double(record.command_total) / record_samples,
            record.command_max,
            double(record.candidate_command_total) / record_samples,
            double(record.safe_command_total) / record_samples,
            double(record.parallel_layer_total) / record_samples,
            record.parallel_layer_max,
            record.serial_record_wall_total_ms / record_samples,
            record.serial_record_wall_max_ms,
            record.serial_command_sum_total_ms / record_samples,
            record.eligible_record_total_ms / record_samples,
            record.predicted_critical_total_ms / record_samples,
            record.dispatch_join_estimate_total_ms / record_samples,
            record.predicted_net_saving_total_ms / record_samples,
            predicted_net_pct,
            double(record.descriptor_bytes_total) / record_samples,
            double(record.buffer_barrier_total) / record_samples,
            double(record.texture_barrier_total) / record_samples,
            double(record.memory_barrier_total) / record_samples,
            double(record.used_query_total) / record_samples,
            CanonicalDigest(record.command_digests),
            record.command_digests.size(),
            CanonicalDigest(record.layer_digests),
            record.layer_digests.size(),
            CanonicalDigest(record.barrier_digests),
            record.barrier_digests.size(),
            CanonicalDigest(record.descriptor_digests),
            record.descriptor_digests.size(),
            CanonicalDigest(record.query_digests),
            record.query_digests.size(),
            record.topology_changes,
            record.golden_complete,
            record.golden_incomplete,
            record.golden_unresolved_total,
            record.golden_opaque_total,
            CanonicalDigest(record.golden_command_digests),
            record.golden_command_digests.size(),
            CanonicalDigest(record.golden_layer_digests),
            record.golden_layer_digests.size(),
            CanonicalDigest(record.golden_barrier_digests),
            record.golden_barrier_digests.size(),
            CanonicalDigest(record.golden_descriptor_digests),
            record.golden_descriptor_digests.size(),
            CanonicalDigest(record.golden_query_digests),
            record.golden_query_digests.size(),
            CanonicalDigest(record.golden_combined_digests),
            record.golden_combined_digests.size(),
            CanonicalDigest(record.golden_manifest_entries),
            record.golden_manifest_entries.size(),
            profile.dispatch_join_tail_ms
        );
    }

    profile.window_start        = now;
    profile.samples             = 0;
    profile.execute             = {};
    profile.present             = {};
    profile.caller_total_ms     = 0.0;
    profile.caller_max_ms       = 0.0;
    profile.queue_wait_total_ms = 0.0;
    profile.queue_wait_max_ms   = 0.0;
    profile.work_total_ms       = 0.0;
    profile.work_max_ms         = 0.0;
    profile.max_enqueue_depth   = 0;
    profile.record              = {};
}

} // namespace Moer::Render
