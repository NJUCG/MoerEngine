#include "rhi/RHISubmissionPipelinePolicy.h"

#include <iostream>
#include <limits>
#include <stdexcept>

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

} // namespace

int main() {
    try {
        ClampContract();
        DistinctNativeQueuesPreserveTheClampedWindow();
        GraphicsComputeAliasForcesOneBatchInFlight();
        UnavailableLogicalQueuesDoNotTriggerAliasFallback();
        std::cout << "RHI submission pipeline policy tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RHI submission pipeline policy test failed: "
                  << error.what() << '\n';
        return 1;
    }
}
