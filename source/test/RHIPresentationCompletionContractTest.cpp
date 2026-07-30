#include "rhi/RHIPresentationCompletion.h"
#include "rhi/vulkan/vulkanextension/VulkanExtensionFactories.h"
#include "rhi/vulkan/vulkanextension/VulkanExtensionRegistry.h"

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

PresentationCompletionIdentity Identity(
    const PresentationCompletionStateRef& _state,
    std::uint64_t _epoch,
    std::uint64_t _generation,
    std::uint64_t _request_serial
) {
    return PresentationCompletionIdentity{
        .state_instance_id = _state->GetStateInstanceId(),
        .presentation_epoch = _epoch,
        .drawable_generation = _generation,
        .request_serial = _request_serial,
    };
}

void CompletePresentFence(
    const PresentationCompletionStateRef& _state,
    const PresentationCompletionTicket&   _ticket
) {
    Expect(
        _state->SetWsiMode(
            _ticket,
            EPresentationWsiCompletionMode::PresentFence
        ),
        "could not select PresentFence mode"
    );
    Expect(
        _state->MarkGpuComplete(_ticket),
        "could not mark GPU completion"
    );
    Expect(
        _state->MarkWsiComplete(_ticket),
        "could not mark WSI completion"
    );
}

void SeparateStatesRejectCrossMutation() {
    const auto state_a = PresentationCompletionState::Create();
    const auto state_b = PresentationCompletionState::Create();
    Expect(
        state_a->GetStateInstanceId() !=
            state_b->GetStateInstanceId(),
        "separate states reused an instance identity"
    );

    const auto ticket_a =
        state_a->Reserve(Identity(state_a, 1, 1, 7), 0, 1);
    Expect(ticket_a.IsValid(), "state A returned an invalid ticket");
    Expect(
        !state_b->SetWsiMode(
            ticket_a,
            EPresentationWsiCompletionMode::Immediate
        ) &&
            !state_b->MarkGpuComplete(ticket_a) &&
            !state_b->MarkWsiComplete(ticket_a) &&
            !state_b->MarkWsiFailed(ticket_a) &&
            !state_b->PublishRetired(ticket_a),
        "state B accepted state A's strong ticket"
    );
    Expect(
        state_a->OutstandingCount() == 1 &&
            state_b->OutstandingCount() == 0,
        "cross-state rejection changed outstanding records"
    );

    Expect(
        state_a->SetWsiMode(
            ticket_a,
            EPresentationWsiCompletionMode::Immediate
        ) &&
            state_a->MarkGpuComplete(ticket_a) &&
            state_a->PublishRetired(ticket_a),
        "state A could not retire its own ticket"
    );
}

void EpochDrainDoesNotCaptureNewGeneration() {
    const auto state = PresentationCompletionState::Create();
    const auto old_ticket =
        state->Reserve(Identity(state, 10, 20, 1), 0, 1);
    const auto new_ticket =
        state->Reserve(Identity(state, 11, 21, 2), 1, 1);
    const auto old_point = state->Freeze(10, 20);
    const auto all_point = state->Freeze();

    CompletePresentFence(state, old_ticket);
    Expect(
        state->PublishRetired(old_ticket),
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

    CompletePresentFence(state, new_ticket);
    Expect(
        state->PublishRetired(new_ticket) &&
            state->WaitRetired(all_point, 20ms),
        "all-identity drain did not finish after both generations retired"
    );
}

void OutOfOrderRetirementUsesInternalIssueSequence() {
    const auto state = PresentationCompletionState::Create();
    const auto first =
        state->Reserve(Identity(state, 30, 40, 900), 0, 1);
    const auto second =
        state->Reserve(Identity(state, 30, 40, 1), 1, 1);
    Expect(
        first.GetIssueSequence() != first.GetIdentity().request_serial &&
            second.GetIssueSequence() ==
                first.GetIssueSequence() + 1,
        "internal issue ordering inherited receipt serials"
    );
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

void FastFailureIsNotHeldBehindAcceptedPresent() {
    const auto state = PresentationCompletionState::Create();
    const auto accepted =
        state->Reserve(Identity(state, 50, 60, 1), 0, 1);
    const auto failed =
        state->Reserve(Identity(state, 50, 60, 2), 1, 1);
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
    const auto ticket =
        state->Reserve(Identity(state, 61, 62, 1), 2, 7);
    Expect(
        state->SetWsiMode(
            ticket,
            EPresentationWsiCompletionMode::PresentFence
        ) &&
            state->MarkGpuComplete(ticket) &&
            state->MarkWsiFailed(ticket),
        "failed Present-fence setup did not become terminal"
    );
    Expect(
        ticket.GetWsiOutcome() ==
                EPresentationWsiCompletionOutcome::Failed &&
            state->WaitForWsi(ticket, 20ms),
        "failed Present fence reported successful or pending"
    );
    Expect(
        state->PublishRetired(ticket) &&
            ticket.GetWsiOutcome() ==
                EPresentationWsiCompletionOutcome::Failed,
        "retirement erased the slot-visible failure outcome"
    );
}

void RetiredSlotGenerationRejectsLateCallbacks() {
    const auto state = PresentationCompletionState::Create();
    const auto old_ticket =
        state->Reserve(Identity(state, 70, 80, 1), 4, 12);
    CompletePresentFence(state, old_ticket);
    Expect(
        state->PublishRetired(old_ticket),
        "old slot generation did not retire"
    );

    const auto new_ticket =
        state->Reserve(Identity(state, 71, 81, 2), 4, 13);
    Expect(
        new_ticket.GetFenceSlot() == old_ticket.GetFenceSlot() &&
            new_ticket.GetSlotGeneration() !=
                old_ticket.GetSlotGeneration(),
        "slot generation setup was invalid"
    );
    Expect(
        !state->MarkGpuComplete(old_ticket) &&
            !state->MarkWsiComplete(old_ticket) &&
            !state->SetWsiMode(
                old_ticket,
                EPresentationWsiCompletionMode::Immediate
            ) &&
            !state->PublishRetired(old_ticket),
        "late old-slot callback remained mutable after retirement"
    );
    Expect(
        !state->WaitForWsi(new_ticket, 2ms),
        "old-slot callback completed the reused slot"
    );

    CompletePresentFence(state, new_ticket);
    Expect(
        state->PublishRetired(new_ticket),
        "new slot generation did not retire independently"
    );
}

void QueueIdleFallbackIsTargetedAndOwnerResolved() {
    const auto state = PresentationCompletionState::Create();
    const auto fallback =
        state->Reserve(Identity(state, 90, 100, 1), 0, 1);
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

void WaitersWakeAndStrongOwnersReleaseBoundedly() {
    auto state = PresentationCompletionState::Create();
    std::weak_ptr<PresentationCompletionState> weak_state = state;
    auto ticket =
        state->Reserve(Identity(state, 110, 120, 1), 0, 1);
    auto point = state->Freeze();
    Expect(
        state->SetWsiMode(
            ticket,
            EPresentationWsiCompletionMode::PresentFence
        ),
        "bounded-wait setup failed"
    );
    Expect(
        !state->WaitForWsi(ticket, 2ms) &&
            !state->WaitRetired(point, 2ms),
        "pending waits did not honor their timeout"
    );

    std::atomic_bool resolver_succeeded{false};
    std::jthread resolver([state, ticket, &resolver_succeeded] {
        std::this_thread::sleep_for(5ms);
        resolver_succeeded.store(
            state->MarkWsiComplete(ticket) &&
                state->MarkGpuComplete(ticket) &&
                state->PublishRetired(ticket),
            std::memory_order_release
        );
    });
    const bool wsi_released =
        state->WaitForWsi(ticket, 2s);
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
        "ticket/drain point did not strongly own their state"
    );
    ticket = {};
    point  = {};
    Expect(
        weak_state.expired(),
        "strong presentation completion owners did not release"
    );
}

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

} // namespace

int main() {
    static_assert(
        std::is_copy_constructible_v<PresentationCompletionTicket>
    );
    static_assert(
        std::is_copy_constructible_v<PresentationDrainPoint>
    );

    try {
        SeparateStatesRejectCrossMutation();
        EpochDrainDoesNotCaptureNewGeneration();
        OutOfOrderRetirementUsesInternalIssueSequence();
        FastFailureIsNotHeldBehindAcceptedPresent();
        FailedPresentFenceOutcomeSurvivesRetirement();
        RetiredSlotGenerationRejectsLateCallbacks();
        QueueIdleFallbackIsTargetedAndOwnerResolved();
        WaitersWakeAndStrongOwnersReleaseBoundedly();
        Maintenance1CapabilityGateIsExplicitAndOptional();
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
