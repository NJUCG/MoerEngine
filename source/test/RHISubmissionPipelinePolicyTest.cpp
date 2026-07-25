#include "rhi/RHISubmissionPipelinePolicy.h"

#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace Moer::Render;

namespace {

void Expect(bool _condition, const char* _message) {
    if (!_condition) {
        throw std::runtime_error(_message);
    }
}

RHIQueueTopology MakeTopology(
    bool     _graphics_available,
    uint32_t _graphics_native,
    bool     _compute_available,
    uint32_t _compute_native
) {
    RHIQueueTopology topology{};
    topology.graphics.available       = _graphics_available;
    topology.graphics.native_queue_id = _graphics_native;
    topology.compute.available        = _compute_available;
    topology.compute.native_queue_id  = _compute_native;
    return topology;
}

void ClampContract() {
    using namespace RHISubmissionPipelinePolicy;

    static_assert(DefaultBatchWindow == 2);
    static_assert(MinBatchWindow == 1);
    static_assert(MaxBatchWindow == 8);
    static_assert(ClampBatchWindow(0) == 1);
    static_assert(ClampBatchWindow(1) == 1);
    static_assert(ClampBatchWindow(2) == 2);
    static_assert(ClampBatchWindow(8) == 8);
    static_assert(ClampBatchWindow(9) == 8);
    static_assert(
        ClampBatchWindow(std::numeric_limits<uint32_t>::max()) == 8
    );
}

void DistinctNativeQueuesPreserveTheClampedWindow() {
    using namespace RHISubmissionPipelinePolicy;

    constexpr RHIQueueTopology topology = [] {
        RHIQueueTopology result{};
        result.graphics.native_queue_id = 3;
        result.compute.native_queue_id  = 7;
        return result;
    }();
    static_assert(!GraphicsComputeShareNativeLane(topology));
    static_assert(ResolveEffectiveBatchWindow(0, topology) == 1);
    static_assert(ResolveEffectiveBatchWindow(2, topology) == 2);
    static_assert(ResolveEffectiveBatchWindow(9, topology) == 8);
}

void GraphicsComputeAliasForcesOneBatchInFlight() {
    using namespace RHISubmissionPipelinePolicy;

    const RHIQueueTopology topology = MakeTopology(true, 5, true, 5);
    Expect(
        GraphicsComputeShareNativeLane(topology),
        "available Graphics/Compute alias was not detected"
    );
    Expect(
        ResolveEffectiveBatchWindow(8, topology) == 1,
        "Graphics/Compute alias did not force a single in-flight batch"
    );
}

void UnavailableLogicalQueuesDoNotTriggerAliasFallback() {
    using namespace RHISubmissionPipelinePolicy;

    const RHIQueueTopology compute_unavailable =
        MakeTopology(true, 0, false, 0);
    Expect(
        !GraphicsComputeShareNativeLane(compute_unavailable),
        "unavailable Compute queue triggered alias fallback"
    );
    Expect(
        ResolveEffectiveBatchWindow(4, compute_unavailable) == 4,
        "unavailable Compute queue changed the configured batch window"
    );

    const RHIQueueTopology graphics_unavailable =
        MakeTopology(false, 11, true, 11);
    Expect(
        !GraphicsComputeShareNativeLane(graphics_unavailable),
        "unavailable Graphics queue triggered alias fallback"
    );
    Expect(
        ResolveEffectiveBatchWindow(9, graphics_unavailable) == 8,
        "unavailable Graphics queue bypassed the ordinary clamp"
    );
}

void BatchWorkStateSequentialContract() {
    using RHISubmissionPipelinePolicy::PipelineBatchWorkState;

    PipelineBatchWorkState empty{};
    Expect(
        empty.Seal(),
        "sealing an empty batch did not own its terminal transition"
    );
    Expect(
        !empty.Seal(),
        "an empty batch published more than one terminal transition"
    );

    PipelineBatchWorkState seal_first{};
    seal_first.AddWork();
    Expect(
        !seal_first.Seal(),
        "a sealed batch with outstanding work terminated early"
    );
    Expect(
        seal_first.FinishWork(),
        "final work retirement did not terminate a sealed batch"
    );

    PipelineBatchWorkState finish_first{};
    finish_first.AddWork();
    Expect(
        !finish_first.FinishWork(),
        "an unsealed batch terminated on its final work retirement"
    );
    Expect(
        finish_first.Seal(),
        "sealing an already-empty batch did not terminate it"
    );

    PipelineBatchWorkState multiple{};
    multiple.AddWork();
    multiple.AddWork();
    Expect(
        !multiple.Seal(),
        "a multi-work batch terminated while work was outstanding"
    );
    Expect(
        !multiple.FinishWork(),
        "the first multi-work retirement terminated the batch"
    );
    Expect(
        multiple.FinishWork(),
        "the final multi-work retirement did not terminate the batch"
    );
}

void BatchWorkStateSealFinishRaceHasOneTerminalOwner() {
    using RHISubmissionPipelinePolicy::PipelineBatchWorkState;

    constexpr uint32_t iterations = 20000;
    std::barrier        phase(3);
    std::atomic<PipelineBatchWorkState*> active_state{nullptr};
    std::atomic<uint32_t>                terminal_owners{0};

    std::jthread seal_thread([&] {
        for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
            phase.arrive_and_wait();
            PipelineBatchWorkState* state =
                active_state.load(std::memory_order_acquire);
            if ((iteration & 1u) == 0) {
                std::this_thread::yield();
            }
            if (state->Seal()) {
                terminal_owners.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            phase.arrive_and_wait();
        }
    });
    std::jthread finish_thread([&] {
        for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
            phase.arrive_and_wait();
            PipelineBatchWorkState* state =
                active_state.load(std::memory_order_acquire);
            if ((iteration & 1u) != 0) {
                std::this_thread::yield();
            }
            if (state->FinishWork()) {
                terminal_owners.fetch_add(
                    1, std::memory_order_relaxed
                );
            }
            phase.arrive_and_wait();
        }
    });

    uint32_t mismatch_count = 0;
    for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
        PipelineBatchWorkState state{};
        state.AddWork();
        terminal_owners.store(0, std::memory_order_relaxed);
        active_state.store(&state, std::memory_order_release);
        phase.arrive_and_wait();
        phase.arrive_and_wait();
        if (terminal_owners.load(std::memory_order_relaxed) != 1) {
            ++mismatch_count;
        }
    }
    seal_thread.join();
    finish_thread.join();
    Expect(
        mismatch_count == 0,
        "Seal/FinishWork race lost or duplicated the terminal transition"
    );
}

} // namespace

int main() {
    try {
        ClampContract();
        DistinctNativeQueuesPreserveTheClampedWindow();
        GraphicsComputeAliasForcesOneBatchInFlight();
        UnavailableLogicalQueuesDoNotTriggerAliasFallback();
        BatchWorkStateSequentialContract();
        BatchWorkStateSealFinishRaceHasOneTerminalOwner();
        std::cout << "RHI submission pipeline policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI submission pipeline policy test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
