#include "rhi/RHIPresentationCompletion.h"
#if !defined(__APPLE__)
#include "rhi/vulkan/vulkanextension/VulkanExtensionFactories.h"
#include "rhi/vulkan/vulkanextension/VulkanExtensionRegistry.h"
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace {

using namespace Moer::Render;
using namespace std::chrono_literals;

void Expect(bool _condition, std::string_view _message) {
    if (!_condition) {
        throw std::runtime_error(std::string(_message));
    }
}

void CompletePresentFence(
    const PresentationCompletionStateRef& _state,
    const PresentationCompletionRef&   _record
) {
    Expect(
        _state->SetWsiMode(
            _record,
            EPresentationWsiCompletionMode::PresentFence
        ),
        "could not select PresentFence mode"
    );
    Expect(
        _state->MarkGpuComplete(_record),
        "could not mark GPU completion"
    );
    Expect(
        _state->MarkWsiComplete(_record),
        "could not mark WSI completion"
    );
}

void SeparateStatesRejectCrossMutation() {
    const auto state_a = PresentationCompletionState::Create();
    const auto state_b = PresentationCompletionState::Create();

    const auto record_a = state_a->Reserve(1, 1);
    Expect(bool(record_a), "state A returned an invalid record");
    Expect(
        !state_b->SetWsiMode(
            record_a,
            EPresentationWsiCompletionMode::Immediate
        ) &&
            !state_b->MarkGpuComplete(record_a) &&
            !state_b->MarkWsiComplete(record_a) &&
            !state_b->MarkWsiFailed(record_a) &&
            !state_b->PublishRetired(record_a),
        "state B accepted state A's completion record"
    );
    Expect(
        state_a->OutstandingCount() == 1 &&
            state_b->OutstandingCount() == 0,
        "cross-state rejection changed outstanding records"
    );

    Expect(
        state_a->SetWsiMode(
            record_a,
            EPresentationWsiCompletionMode::Immediate
        ) &&
            state_a->MarkGpuComplete(record_a) &&
            state_a->PublishRetired(record_a),
        "state A could not retire its own record"
    );
}

void EpochDrainDoesNotCaptureNewGeneration() {
    const auto state = PresentationCompletionState::Create();
    const auto old_record = state->Reserve(10, 20);
    const auto new_record = state->Reserve(11, 21);
    const auto old_point = state->Freeze(10, 20);
    const auto all_point = state->Freeze();

    CompletePresentFence(state, old_record);
    Expect(
        state->PublishRetired(old_record),
        "old epoch did not retire"
    );
    Expect(
        state->WaitRetired(old_point, 20ms),
        "old epoch drain waited on a newer generation"
    );
    Expect(
        !state->WaitRetired(all_point, 2ms),
        "all-identity drain ignored the newer generation"
    );

    CompletePresentFence(state, new_record);
    Expect(
        state->PublishRetired(new_record) &&
            state->WaitRetired(all_point, 20ms),
        "all-identity drain did not finish after both generations retired"
    );
}

void OutOfOrderRetirementDoesNotCompleteOlderRecords() {
    const auto state = PresentationCompletionState::Create();
    const auto first  = state->Reserve(30, 40);
    const auto second = state->Reserve(30, 40);
    const auto point = state->Freeze();

    CompletePresentFence(state, second);
    Expect(
        state->PublishRetired(second),
        "later issue could not retire out of order"
    );
    Expect(
        state->OutstandingCount() == 1 &&
            !state->WaitRetired(point, 2ms),
        "out-of-order retirement falsely advanced past an older issue"
    );

    CompletePresentFence(state, first);
    Expect(
        state->PublishRetired(first) &&
            state->WaitRetired(point, 20ms),
        "drain did not finish after the older issue retired"
    );
}

void FrozenDrainExcludesLaterPresents() {
    const auto state = PresentationCompletionState::Create();
    const auto fallback = state->Reserve(41, 42);
    const auto fence = state->Reserve(41, 42);
    Expect(state->SetWsiMode(fallback, EPresentationWsiCompletionMode::QueueIdleFallback) &&
               state->SetWsiMode(fence, EPresentationWsiCompletionMode::PresentFence),
           "could not configure mixed presentation completion modes");
    const auto point = state->Freeze(41, 42);
    const auto later = state->Reserve(41, 42);
    Expect(state->SetWsiMode(later, EPresentationWsiCompletionMode::QueueIdleFallback),
           "could not configure the later Present");

    Expect(state->ResolveQueueIdle(point) &&
               fallback->GetWsiOutcome() == EPresentationWsiCompletionOutcome::Succeeded &&
               fence->GetWsiOutcome() == EPresentationWsiCompletionOutcome::Pending &&
               later->GetWsiOutcome() == EPresentationWsiCompletionOutcome::Pending,
           "queue-idle resolution completed a fence or a later Present");
    Expect(!state->PublishRetired(fallback), "WSI completion retired resources before GPU completion");
    Expect(state->MarkGpuComplete(fallback) && state->PublishRetired(fallback), "fallback did not retire");
    Expect(state->MarkWsiComplete(fence) && state->MarkGpuComplete(fence) && state->PublishRetired(fence) &&
               state->WaitRetired(point, 20ms) && state->OutstandingCount() == 1,
           "frozen drain waited for a later Present in the same generation");

    const auto tail = state->Freeze();
    Expect(state->ResolveQueueIdle(tail) && state->MarkGpuComplete(later) && state->PublishRetired(later) &&
               state->WaitRetired(tail, 20ms),
           "later Present did not drain independently");
}

void FastFailureIsNotHeldBehindAcceptedPresent() {
    const auto state = PresentationCompletionState::Create();
    const auto accepted = state->Reserve(50, 60);
    const auto failed   = state->Reserve(50, 60);
    Expect(
        state->SetWsiMode(
            accepted,
            EPresentationWsiCompletionMode::PresentFence
        ),
        "accepted Present could not select its WSI mode"
    );
    Expect(
        state->SetWsiMode(
            failed,
            EPresentationWsiCompletionMode::Failed
        ) &&
            state->MarkGpuComplete(failed) &&
            state->PublishRetired(failed),
        "fast failure could not retire behind an accepted Present"
    );
    Expect(
        state->WaitForWsi(failed, 2ms) &&
            !state->WaitForWsi(accepted, 2ms) &&
            state->OutstandingCount() == 1,
        "fast failure publication changed the accepted Present"
    );

    Expect(
        state->MarkGpuComplete(accepted) &&
            state->MarkWsiComplete(accepted) &&
            state->PublishRetired(accepted),
        "accepted Present cleanup failed"
    );
}

void FailedPresentFenceOutcomeSurvivesRetirement() {
    const auto state = PresentationCompletionState::Create();
    const auto record = state->Reserve(61, 62);
    Expect(
        state->SetWsiMode(
            record,
            EPresentationWsiCompletionMode::PresentFence
        ) &&
            state->MarkGpuComplete(record) &&
            state->MarkWsiFailed(record),
        "failed Present-fence setup did not become terminal"
    );
    Expect(
        record->GetWsiOutcome() ==
                EPresentationWsiCompletionOutcome::Failed &&
            state->WaitForWsi(record, 20ms),
        "failed Present fence reported successful or pending"
    );
    Expect(
        state->PublishRetired(record) &&
            record->GetWsiOutcome() ==
                EPresentationWsiCompletionOutcome::Failed,
        "retirement erased the record's failure outcome"
    );
}

void RetiredIssueRejectsLateCallbacks() {
    const auto state = PresentationCompletionState::Create();
    const auto old_record = state->Reserve(70, 80);
    CompletePresentFence(state, old_record);
    Expect(state->PublishRetired(old_record), "old issue did not retire");

    const auto new_record = state->Reserve(70, 80);
    Expect(new_record != old_record, "new Present reused an old completion record");
    Expect(
        !state->MarkGpuComplete(old_record) && !state->MarkWsiComplete(old_record) &&
            !state->SetWsiMode(old_record, EPresentationWsiCompletionMode::Immediate) &&
            !state->PublishRetired(old_record),
        "late old-issue callback remained mutable after retirement"
    );
    Expect(!state->WaitForWsi(new_record, 2ms), "old-issue callback completed the reused identity");

    CompletePresentFence(state, new_record);
    Expect(state->PublishRetired(new_record), "new issue did not retire independently");
}

void QueueIdleFallbackIsTargetedAndOwnerResolved() {
    const auto state = PresentationCompletionState::Create();
    const auto fallback = state->Reserve(90, 100);
    Expect(
        state->SetWsiMode(
            fallback,
            EPresentationWsiCompletionMode::QueueIdleFallback
        ) &&
            state->MarkGpuComplete(fallback),
        "fallback Present setup failed"
    );
    Expect(
        !state->MarkWsiComplete(fallback),
        "Completion owner bypassed the queue-idle fallback owner"
    );
    const auto point = state->Freeze(90, 100);
    Expect(
        point.IsValid() &&
            state->RequiresQueueIdle(point) &&
            !state->WaitForWsi(fallback, 2ms),
        "fallback drain did not expose its queue-idle requirement"
    );
    Expect(
        state->ResolveQueueIdle(point) &&
            !state->RequiresQueueIdle(point) &&
            state->WaitForWsi(fallback, 20ms),
        "queue-idle resolution did not publish WSI completion"
    );
    Expect(
        state->PublishRetired() == 1 &&
            state->WaitRetired(point, 20ms),
        "resolved fallback did not retire"
    );
}

void ReacquisitionCompletesOnlyThePreviousPresent() {
    const auto state    = PresentationCompletionState::Create();
    const auto previous = state->Reserve(101, 102);
    const auto current  = state->Reserve(101, 102);
    for (const auto& record : {previous, current}) {
        Expect(
            state->SetWsiMode(record, EPresentationWsiCompletionMode::QueueIdleFallback) &&
                state->MarkGpuComplete(record),
            "reacquisition setup failed"
        );
    }
    Expect(!state->WaitForWsi(previous, 1ms), "GPU completion alone retired the previous Present");
    Expect(
        state->MarkImageAvailable(previous) && state->PublishRetired(previous),
        "completed acquisition did not retire the previous Present"
    );
    Expect(
        !state->MarkImageAvailable(previous) && !state->WaitForWsi(current, 1ms) &&
            state->OutstandingCount() == 1,
        "previous acquisition completed the current Present"
    );
    const auto tail = state->Freeze();
    Expect(
        state->RequiresQueueIdle(tail) && state->ResolveQueueIdle(tail) && state->PublishRetired(current) &&
            state->WaitRetired(tail, 20ms),
        "the final unacquired Present did not drain through the compatibility fallback"
    );
}

void WaitersWakeAndStrongOwnersReleaseBoundedly() {
    auto state = PresentationCompletionState::Create();
    std::weak_ptr<PresentationCompletionState> weak_state = state;
    auto                                       record     = state->Reserve(110, 120);
    auto point = state->Freeze();
    Expect(
        state->SetWsiMode(
            record,
            EPresentationWsiCompletionMode::PresentFence
        ),
        "bounded-wait setup failed"
    );
    Expect(
        !state->WaitForWsi(record, 2ms) &&
            !state->WaitRetired(point, 2ms),
        "pending waits did not honor their timeout"
    );

    std::atomic_bool resolver_succeeded{false};
    std::jthread resolver([state, record, &resolver_succeeded] {
        std::this_thread::sleep_for(5ms);
        resolver_succeeded.store(
            state->MarkWsiComplete(record) &&
                state->MarkGpuComplete(record) &&
                state->PublishRetired(record),
            std::memory_order_release
        );
    });
    const bool wsi_released =
        state->WaitForWsi(record, 2s);
    const bool retirement_released =
        state->WaitRetired(point, 2s);
    resolver.join();
    Expect(
        wsi_released &&
            retirement_released &&
            resolver_succeeded.load(std::memory_order_acquire) &&
            !state->HasOutstanding() &&
            state->OutstandingCount() == 0,
        "waiter/resolver failed or terminal records were retained"
    );

    state.reset();
    Expect(
        !weak_state.expired(),
        "drain point did not keep its state alive"
    );
    point = {};
    Expect(weak_state.expired(), "completed record retained its state");
    Expect(record->GetWsiOutcome() == EPresentationWsiCompletionOutcome::Succeeded,
           "releasing the state erased the record outcome");
    record = {};
    Expect(
        weak_state.expired(),
        "strong presentation completion owners did not release"
    );
}

#if !defined(__APPLE__)
void Maintenance1CapabilityGateIsExplicitAndOptional() {
    bool found_surface_dependency = false;
    bool found_surface_maintenance = false;
    bool found_swapchain_maintenance = false;
    for (const VulkanExtensionDesc& desc :
         GetVulkanExtensionDescs()) {
        if (desc.name ==
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) {
            found_surface_dependency =
                desc.kind == EVulkanExtensionKind::Instance &&
                desc.optional;
        } else if (
            desc.name ==
            VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME
        ) {
            found_surface_maintenance =
                desc.kind == EVulkanExtensionKind::Instance &&
                desc.optional;
        } else if (
            desc.name ==
            VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME
        ) {
            found_swapchain_maintenance =
                desc.kind == EVulkanExtensionKind::Device &&
                desc.optional && desc.factory != nullptr;
        }
    }
    Expect(
        found_surface_dependency &&
            found_surface_maintenance &&
            found_swapchain_maintenance,
        "maintenance1 registry dependencies are not optional and explicit"
    );

    Expect(
        !CanEnableVulkanSurfaceMaintenance1(false, false) &&
            !CanEnableVulkanSurfaceMaintenance1(true, false) &&
            !CanEnableVulkanSurfaceMaintenance1(false, true) &&
            CanEnableVulkanSurfaceMaintenance1(true, true),
        "surface maintenance1 dependency truth table is wrong"
    );
    Expect(
        !CanEnableVulkanSwapchainMaintenance1(false, false) &&
            !CanEnableVulkanSwapchainMaintenance1(true, false) &&
            !CanEnableVulkanSwapchainMaintenance1(false, true) &&
            CanEnableVulkanSwapchainMaintenance1(true, true),
        "swapchain maintenance1 dependency truth table is wrong"
    );

    const auto verify_feature =
        [](VkBool32 _feature_value, bool _expected_enabled) {
            auto extension =
                CreateVulkanEXTSwapchainMaintenance1Extension(true);
            extension->Enable();
            VkPhysicalDeviceFeatures2 features{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2
            };
            extension->PreGpuFeatures(features);
            auto* maintenance_features =
                static_cast<
                    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT*>(
                    features.pNext
                );
            Expect(
                maintenance_features != nullptr &&
                    maintenance_features->sType ==
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT,
                "maintenance1 feature query was not chained"
            );
            maintenance_features->swapchainMaintenance1 =
                _feature_value;
            VulkanOptionalDeviceExtensions optional_extensions{};
            extension->PostGpuFeatures(optional_extensions);
            Expect(
                extension->ShouldEnableDeviceCreate() ==
                        _expected_enabled &&
                    optional_extensions.
                            m_has_ext_swapchain_maintenance1 ==
                        _expected_enabled,
                "maintenance1 feature bit did not gate device enablement"
            );
        };
    verify_feature(VK_FALSE, false);
    verify_feature(VK_TRUE, true);
}
#endif

} // namespace

int main() {
    static_assert(
        std::is_copy_constructible_v<PresentationCompletionRef>
    );
    static_assert(
        std::is_copy_constructible_v<PresentationDrainPoint>
    );

    try {
        SeparateStatesRejectCrossMutation();
        EpochDrainDoesNotCaptureNewGeneration();
        OutOfOrderRetirementDoesNotCompleteOlderRecords();
        FrozenDrainExcludesLaterPresents();
        FastFailureIsNotHeldBehindAcceptedPresent();
        FailedPresentFenceOutcomeSurvivesRetirement();
        RetiredIssueRejectsLateCallbacks();
        QueueIdleFallbackIsTargetedAndOwnerResolved();
        ReacquisitionCompletesOnlyThePreviousPresent();
        WaitersWakeAndStrongOwnersReleaseBoundedly();
#if !defined(__APPLE__)
        Maintenance1CapabilityGateIsExplicitAndOptional();
#endif
    } catch (const std::exception& exception) {
        std::cerr
            << "RHIPresentationCompletionContract: "
            << exception.what() << '\n';
        return 1;
    }

    std::cout
        << "RHIPresentationCompletionContract: all checks passed\n";
    return 0;
}
