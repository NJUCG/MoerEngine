#include "rhi/ExternalCpuJoinPool.h"
#include "rhi/RHIParallelRecord.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using namespace Moer::Render;
using namespace std::chrono_literals;

namespace {

using Type = Command::EType;

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

ParallelRecordLayerDescription Describe(const std::vector<Type>& _types) {
    return {.command_types = std::span<const Type>(_types)};
}

ParallelRecordLayerDescription DescribeWeighted(
    const std::vector<Type>&     _types,
    const std::vector<uint32_t>& _work_units
) {
    return {
        .command_types = std::span<const Type>(_types),
        .command_work_units = std::span<const uint32_t>(_work_units),
    };
}

ParallelRecordLayerDescription DescribeForcedSerial(const std::vector<Type>& _types) {
    return {.command_types = std::span<const Type>(_types), .force_serial = true};
}

void EligibleLayerUsesStableBalancedPartitions() {
    const std::vector<Type> types(5, Type::BufferToBuffer);
    const std::vector<ParallelRecordLayerDescription> layers{Describe(types)};

    const ParallelRecordPlan plan = BuildParallelRecordPlan(layers, 3, 1);
    Expect(plan.CanRecordInParallel(), "eligible layer did not produce a parallel plan");
    Expect(plan.parallel_layer_count == 1, "parallel layer count was incorrect");
    Expect(plan.layers.size() == 1 && plan.layers[0].parallel, "layer was not marked parallel");
    Expect(plan.jobs.size() == 3, "worker count did not cap partition count");
    Expect(plan.jobs[0].command_begin == 0 && plan.jobs[0].command_count == 2, "first range was not balanced");
    Expect(plan.jobs[1].command_begin == 2 && plan.jobs[1].command_count == 2, "second range was not contiguous");
    Expect(plan.jobs[2].command_begin == 4 && plan.jobs[2].command_count == 1, "third range lost the remainder");
}

void MixedSerialLayerRemainsOneOrderedJob() {
    const std::vector<Type> eligible{Type::ClearResource, Type::ClearResource};
    const std::vector<Type> mixed{Type::BufferToBuffer, Type::Scope, Type::BufferToBuffer};
    const std::vector<ParallelRecordLayerDescription> layers{Describe(eligible), Describe(mixed)};

    const ParallelRecordPlan plan = BuildParallelRecordPlan(layers, 4, 1);
    Expect(plan.CanRecordInParallel(), "serial neighbor disabled an eligible layer");
    Expect(plan.layers.size() == 2, "layer descriptions were not preserved");
    Expect(plan.layers[0].parallel && plan.layers[0].job_count == 2, "eligible layer was not split");
    Expect(!plan.layers[1].parallel && plan.layers[1].job_count == 1, "mixed layer was partially split");
    Expect(plan.jobs.size() == 3, "mixed plan emitted the wrong job count");
    Expect(plan.jobs[2].layer_index == 1 && plan.jobs[2].command_begin == 0 &&
               plan.jobs[2].command_count == mixed.size(),
           "serial layer did not remain a single contiguous job");
}

void RuntimeConstraintForcesAnOtherwiseEligibleLayerSerial() {
    const std::vector<Type> forced{Type::ShaderDispatch, Type::ShaderDispatch};
    const std::vector<Type> eligible{Type::ClearResource, Type::ClearResource};
    const std::vector<ParallelRecordLayerDescription> layers{
        DescribeForcedSerial(forced), Describe(eligible)
    };

    const ParallelRecordPlan plan = BuildParallelRecordPlan(layers, 4, 1);
    Expect(plan.CanRecordInParallel(), "forced serial neighbor disabled the eligible layer");
    Expect(!plan.layers[0].parallel && plan.layers[0].job_count == 1,
           "runtime-constrained layer was split");
    Expect(plan.layers[1].parallel && plan.layers[1].job_count == 2,
           "unconstrained eligible layer was not split");
}

void OrderedSingletonLayersFormAParallelRecordingWave() {
    const std::vector<Type> first{Type::SetDrawState};
    const std::vector<Type> second{Type::ClearResource};
    const std::vector<ParallelRecordLayerDescription> layers{
        Describe(first), Describe(second)
    };

    const ParallelRecordPlan plan = BuildParallelRecordPlan(layers, 4, 1);
    Expect(plan.CanRecordInParallel(), "ordered singleton layers did not form a recording wave");
    Expect(plan.parallel_layer_count == 2, "singleton wave lost an eligible layer");
    Expect(plan.jobs.size() == 2, "singleton wave emitted the wrong job count");
    Expect(plan.jobs[0].layer_index == 0 && plan.jobs[0].command_count == 1,
           "first singleton job was not stable");
    Expect(plan.jobs[1].layer_index == 1 && plan.jobs[1].command_count == 1,
           "second singleton job was not stable");

    const std::vector<ParallelRecordLayerDescription> one_layer{Describe(first)};
    const ParallelRecordPlan one_job = BuildParallelRecordPlan(one_layer, 4, 1);
    Expect(one_job.fallback_reason == ParallelRecordFallbackReason::InsufficientConcurrency,
           "a lone singleton paid parallel dispatch overhead");
}

void SerialIslandBreaksSingletonRecordingWaves() {
    const std::vector<Type> first{Type::SetDrawState};
    const std::vector<Type> serial{Type::Scope};
    const std::vector<Type> third{Type::ClearResource};
    const std::vector<ParallelRecordLayerDescription> isolated{
        Describe(first), Describe(serial), Describe(third)
    };

    const ParallelRecordPlan no_overlap = BuildParallelRecordPlan(isolated, 4, 1);
    Expect(no_overlap.fallback_reason == ParallelRecordFallbackReason::InsufficientConcurrency,
           "serial island let isolated singleton jobs advertise false concurrency");

    const std::vector<Type> wide{Type::BufferToBuffer, Type::BufferToBuffer};
    const std::vector<ParallelRecordLayerDescription> mixed{
        Describe(first), Describe(serial), Describe(wide)
    };
    const ParallelRecordPlan retained = BuildParallelRecordPlan(mixed, 4, 1);
    Expect(retained.CanRecordInParallel(), "profitable wave was lost with an isolated neighbor");
    Expect(!retained.layers[0].parallel && retained.layers[0].job_count == 1,
           "isolated singleton was not demoted to coordinator recording");
    Expect(retained.layers[2].parallel && retained.layers[2].job_count == 2,
           "profitable wave was not retained");
}

void InvalidInputsFailClosedWithoutPublishingJobs() {
    const std::vector<Type> eligible{Type::TextureToTexture, Type::TextureToTexture};
    const std::vector<ParallelRecordLayerDescription> layers{Describe(eligible)};

    const ParallelRecordPlan one_worker = BuildParallelRecordPlan(layers, 1, 1);
    Expect(one_worker.fallback_reason == ParallelRecordFallbackReason::WorkerCountTooSmall,
           "one worker reported the wrong fallback");
    Expect(one_worker.jobs.empty() && one_worker.layers.empty(), "one-worker fallback published work");

    const std::vector<Type> serial{Type::Scope, Type::Custom};
    const std::vector<ParallelRecordLayerDescription> serial_layers{Describe(serial)};
    const ParallelRecordPlan no_eligible = BuildParallelRecordPlan(serial_layers, 2, 1);
    Expect(no_eligible.fallback_reason == ParallelRecordFallbackReason::NoEligibleLayer,
           "serial-only input reported the wrong fallback");
    Expect(no_eligible.jobs.empty() && no_eligible.layers.empty(), "serial-only fallback published work");

    const std::vector<Type> invalid{Type::BufferToBuffer, Type::Count};
    const std::vector<ParallelRecordLayerDescription> invalid_layers{Describe(invalid)};
    const ParallelRecordPlan invalid_plan = BuildParallelRecordPlan(invalid_layers, 2, 1);
    Expect(invalid_plan.fallback_reason == ParallelRecordFallbackReason::InvalidCommandType,
           "Count sentinel reported the wrong fallback");
    Expect(invalid_plan.jobs.empty() && invalid_plan.layers.empty(), "invalid command published work");

    const std::vector<uint32_t> wrong_work_units{1};
    const std::vector<ParallelRecordLayerDescription> invalid_work{
        DescribeWeighted(eligible, wrong_work_units)
    };
    const ParallelRecordPlan invalid_work_plan = BuildParallelRecordPlan(invalid_work, 2, 1);
    Expect(
        invalid_work_plan.fallback_reason == ParallelRecordFallbackReason::InvalidWorkDescription,
        "mismatched work-unit descriptions did not fail closed"
    );
    Expect(invalid_work_plan.jobs.empty(), "invalid work description published jobs");
}

void StableMergeOrderReconstructsOriginalLayerOrder() {
    const std::vector<Type> first(5, Type::ShaderDispatch);
    const std::vector<Type> serial{Type::Scope};
    const std::vector<Type> third(4, Type::MultiDraw);
    const std::vector<ParallelRecordLayerDescription> layers{
        Describe(first), Describe(serial), Describe(third)
    };

    const ParallelRecordPlan first_plan  = BuildParallelRecordPlan(layers, 3, 1);
    const ParallelRecordPlan second_plan = BuildParallelRecordPlan(layers, 3, 1);
    Expect(first_plan.CanRecordInParallel() && second_plan.CanRecordInParallel(), "stable plan did not build");
    Expect(first_plan.jobs.size() == second_plan.jobs.size(), "same input produced a different job count");

    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (size_t index = 0; index < first_plan.jobs.size(); ++index) {
        const ParallelRecordJobRange& lhs = first_plan.jobs[index];
        const ParallelRecordJobRange& rhs = second_plan.jobs[index];
        Expect(lhs.layer_index == rhs.layer_index && lhs.command_begin == rhs.command_begin &&
                   lhs.command_count == rhs.command_count,
               "same input produced unstable ranges");
        for (uint32_t command_index = lhs.command_begin; command_index < lhs.CommandEnd(); ++command_index) {
            merged.emplace_back(lhs.layer_index, command_index);
        }
    }

    std::vector<std::pair<uint32_t, uint32_t>> expected;
    for (uint32_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
        for (uint32_t command_index = 0; command_index < layers[layer_index].command_types.size(); ++command_index) {
            expected.emplace_back(layer_index, command_index);
        }
    }
    Expect(merged == expected, "ordered job merge changed command order");
}

void WorkGranularityGateAmortizesWorkerAndPrimaryOverhead() {
    const std::vector<Type> below_floor(7, Type::BufferToBuffer);
    const std::vector<ParallelRecordLayerDescription> below_floor_layers{Describe(below_floor)};
    const ParallelRecordPlan below_floor_plan = BuildParallelRecordPlan(below_floor_layers, 4, 8);
    Expect(
        below_floor_plan.fallback_reason == ParallelRecordFallbackReason::InsufficientWork,
        "sub-threshold layer did not report insufficient work"
    );

    const std::vector<Type> too_small(15, Type::BufferToBuffer);
    const std::vector<ParallelRecordLayerDescription> small_layers{Describe(too_small)};
    const ParallelRecordPlan rejected = BuildParallelRecordPlan(small_layers, 4, 8);
    Expect(
        rejected.fallback_reason == ParallelRecordFallbackReason::InsufficientConcurrency,
        "one amortized job did not report insufficient concurrency"
    );
    Expect(rejected.jobs.empty(), "sub-threshold layer published worker jobs");

    const std::vector<Type> enough(16, Type::BufferToBuffer);
    const std::vector<ParallelRecordLayerDescription> enough_layers{Describe(enough)};
    const ParallelRecordPlan accepted = BuildParallelRecordPlan(enough_layers, 4, 8);
    Expect(accepted.CanRecordInParallel(), "two amortized worker jobs were rejected");
    Expect(accepted.jobs.size() == 2, "minimum work gate emitted the wrong job count");
    Expect(
        accepted.jobs[0].command_count == 8 && accepted.jobs[1].command_count == 8,
        "minimum work gate did not preserve eight commands per worker job"
    );
}

void WeightedWorkPreservesHeavyCommandsWithoutInventingConcurrency() {
    const std::vector<Type> heavy{Type::MultiDraw};
    const std::vector<uint32_t> heavy_work{96};
    const std::vector<ParallelRecordLayerDescription> one_heavy{
        DescribeWeighted(heavy, heavy_work)
    };
    const ParallelRecordPlan isolated = BuildParallelRecordPlan(one_heavy, 4, 64);
    Expect(
        isolated.fallback_reason == ParallelRecordFallbackReason::InsufficientConcurrency,
        "an isolated heavy command advertised false parallelism"
    );

    const std::vector<ParallelRecordLayerDescription> heavy_wave{
        DescribeWeighted(heavy, heavy_work), DescribeWeighted(heavy, heavy_work)
    };
    const ParallelRecordPlan wave = BuildParallelRecordPlan(heavy_wave, 4, 64);
    Expect(wave.CanRecordInParallel(), "two heavy singleton commands did not form a wave");
    Expect(wave.jobs.size() == 2, "heavy singleton wave emitted the wrong job count");
    Expect(
        wave.jobs[0].command_count == 1 && wave.jobs[0].work_units == 96 &&
            wave.jobs[1].command_count == 1 && wave.jobs[1].work_units == 96,
        "heavy singleton work was split or lost"
    );

    const std::vector<uint32_t> light_work{3};
    const std::vector<ParallelRecordLayerDescription> light_wave{
        DescribeWeighted(heavy, light_work), DescribeWeighted(heavy, light_work)
    };
    const ParallelRecordPlan light = BuildParallelRecordPlan(light_wave, 4, 64);
    Expect(
        light.fallback_reason == ParallelRecordFallbackReason::InsufficientWork,
        "light singleton commands crossed the production work floor"
    );
}

void WeightedPartitionsRemainStableAndThresholdSized() {
    const std::vector<Type> pair(2, Type::MultiDraw);
    const std::vector<uint32_t> balanced_work{64, 64};
    const std::vector<ParallelRecordLayerDescription> balanced_layers{
        DescribeWeighted(pair, balanced_work)
    };
    const ParallelRecordPlan balanced = BuildParallelRecordPlan(balanced_layers, 4, 64);
    Expect(balanced.CanRecordInParallel(), "two threshold-sized commands were rejected");
    Expect(balanced.jobs.size() == 2, "balanced weighted input emitted the wrong job count");
    Expect(
        balanced.jobs[0].command_begin == 0 && balanced.jobs[0].command_count == 1 &&
            balanced.jobs[0].work_units == 64 && balanced.jobs[1].command_begin == 1 &&
            balanced.jobs[1].command_count == 1 && balanced.jobs[1].work_units == 64,
        "balanced weighted partitions were not stable"
    );

    const std::vector<Type> uneven_types(3, Type::MultiDraw);
    const std::vector<uint32_t> uneven_work{100, 1, 27};
    const std::vector<Type> neighbor_type{Type::MultiDraw};
    const std::vector<uint32_t> neighbor_work{64};
    const std::vector<ParallelRecordLayerDescription> uneven_layers{
        DescribeWeighted(uneven_types, uneven_work),
        DescribeWeighted(neighbor_type, neighbor_work),
    };
    const ParallelRecordPlan uneven = BuildParallelRecordPlan(uneven_layers, 4, 64);
    Expect(uneven.CanRecordInParallel(), "valid uneven work was rejected");
    Expect(uneven.layers[0].job_count == 1, "sub-threshold tail formed an invalid job");
    Expect(
        uneven.jobs[0].command_begin == 0 && uneven.jobs[0].command_count == 3 &&
            uneven.jobs[0].work_units == 128,
        "uneven work was not deterministically coalesced"
    );
}

void LargeWeightedLayerKeepsStableLinearSuffixPlanning() {
    constexpr uint32_t command_count = 8192;
    const std::vector<Type> types(command_count, Type::BufferToBuffer);
    const std::vector<uint32_t> work(command_count, 1u);
    const std::vector<ParallelRecordLayerDescription> layers{
        DescribeWeighted(types, work)
    };

    const ParallelRecordPlan plan = BuildParallelRecordPlan(layers, 32, 64);
    Expect(plan.CanRecordInParallel(), "large weighted layer did not produce a plan");
    Expect(plan.jobs.size() == 32, "large weighted layer did not use the worker cap");
    uint32_t next_command = 0;
    uint64_t total_work = 0;
    for (const ParallelRecordJobRange& job : plan.jobs) {
        Expect(job.command_begin == next_command, "large weighted ranges were not contiguous");
        Expect(job.command_count != 0, "large weighted plan emitted an empty range");
        Expect(job.work_units >= 64, "large weighted plan emitted a sub-threshold range");
        next_command = job.CommandEnd();
        total_work += job.work_units;
    }
    Expect(next_command == command_count, "large weighted plan lost commands");
    Expect(total_work == command_count, "large weighted plan lost work units");
}

void ExternalJoinUsesRealConcurrentWorkers() {
    ExternalCpuJoinPool pool(2);
    std::mutex              mutex;
    std::condition_variable cv;
    uint32_t                arrived{0};

    std::vector<ExternalCpuJoinPool::Job> jobs;
    for (uint32_t index = 0; index < 2; ++index) {
        jobs.emplace_back([&] {
            std::unique_lock lock(mutex);
            ++arrived;
            cv.notify_all();
            if (!cv.wait_for(lock, 2s, [&] { return arrived == 2; })) {
                throw std::runtime_error("jobs did not overlap on distinct workers");
            }
        });
    }

    Expect(pool.RunAndWait(jobs) == ExternalJoinResult::Completed, "concurrent worker join failed");
    Expect(arrived == 2, "concurrent worker join lost a job");
}

void WorkerFailureKeepsSubmissionGateClosed() {
    ExternalCpuJoinPool pool(2);
    std::atomic<uint32_t> completed_recorders{0};
    std::atomic<uint32_t> submission_gate{0};

    std::vector<ExternalCpuJoinPool::Job> jobs;
    jobs.emplace_back([] { throw std::runtime_error("injected recorder failure"); });
    jobs.emplace_back([&] { completed_recorders.fetch_add(1, std::memory_order_relaxed); });

    const ExternalJoinResult result = pool.RunAndWait(jobs);
    if (result == ExternalJoinResult::Completed) {
        submission_gate.store(1, std::memory_order_release);
    }

    Expect(result == ExternalJoinResult::Failed, "recorder failure did not fail the joined batch");
    Expect(completed_recorders.load(std::memory_order_acquire) == 1, "failed batch did not drain accepted work");
    Expect(submission_gate.load(std::memory_order_acquire) == 0, "failed recording opened the submit gate");
}

} // namespace

int main() {
    try {
        EligibleLayerUsesStableBalancedPartitions();
        MixedSerialLayerRemainsOneOrderedJob();
        RuntimeConstraintForcesAnOtherwiseEligibleLayerSerial();
        OrderedSingletonLayersFormAParallelRecordingWave();
        SerialIslandBreaksSingletonRecordingWaves();
        InvalidInputsFailClosedWithoutPublishingJobs();
        StableMergeOrderReconstructsOriginalLayerOrder();
        WorkGranularityGateAmortizesWorkerAndPrimaryOverhead();
        WeightedWorkPreservesHeavyCommandsWithoutInventingConcurrency();
        WeightedPartitionsRemainStableAndThresholdSized();
        LargeWeightedLayerKeepsStableLinearSuffixPlanning();
        ExternalJoinUsesRealConcurrentWorkers();
        WorkerFailureKeepsSubmissionGateClosed();
        std::cout << "RHI parallel record plan tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI parallel record plan test failed: " << error.what() << '\n';
        return 1;
    }
}
