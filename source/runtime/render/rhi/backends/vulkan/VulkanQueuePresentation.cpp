#include "VulkanBarrier.h"
#include "VulkanCommandRecorder.h"
#include "VulkanCustomCommand.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "VulkanQueue.h"
#include "VulkanQueueInternal.h"
#include "VulkanSwapChain.h"
#include "log/LogSystem.h"
#include "misc/Types.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif

#include <algorithm>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <variant>

namespace Moer::Render {

namespace {
constexpr uint64 kPresentAcquireTimeoutNanoseconds = 50'000'000;

struct VulkanPresentSourceContract {
    bool        valid{false};
    const char* reason{"unknown present-source contract failure"};
};

[[nodiscard]] VulkanOperationResult
ClassifyPresentSourceRejection(VulkanDevice& _device, bool& _recoverable_rejection) noexcept {
    // This acquire is the rejection linearization point. A device fault may
    // latch after the optimistic check at Present entry while the source
    // contract/readiness is being inspected; never report that already
    // latched hard failure as a recoverable source error.
    const bool faulted     = _device.IsFaulted();
    _recoverable_rejection = !faulted;
    return {
        EVulkanOperationStatus::Rejected,
        faulted ? _device.GetFirstFaultResult() : VK_ERROR_UNKNOWN,
    };
}

[[nodiscard]] bool AreVulkanCopyFormatsSizeCompatible(EPixelFormat _source, EPixelFormat _destination) noexcept {
    const uint32     source_index                = static_cast<uint32>(_source);
    const uint32     destination_index           = static_cast<uint32>(_destination);
    constexpr uint32 first_single_plane_color    = static_cast<uint32>(PF_R4G4_UNORM_PACK8);
    constexpr uint32 last_single_plane_color     = static_cast<uint32>(PF_E5B9G9R9_UFLOAT_PACK32);
    const auto       is_known_single_plane_color = [](uint32 _format_index) noexcept {
        return _format_index >= first_single_plane_color && _format_index <= last_single_plane_color;
    };
    if (!is_known_single_plane_color(source_index) || !is_known_single_plane_color(destination_index)) {
        return false;
    }

    const VulkanPixelFormatInfo& source      = VulkanEnumTranslator::GetPixelFormatInfo(_source);
    const VulkanPixelFormatInfo& destination = VulkanEnumTranslator::GetPixelFormatInfo(_destination);
    return source.format != VK_FORMAT_UNDEFINED && destination.format != VK_FORMAT_UNDEFINED && source.stride != 0 &&
           source.stride == destination.stride;
}

[[nodiscard]] VulkanPresentSourceContract
ValidatePresentSourceContract(const TextureView& _view, const Swapchain* _swapchain) noexcept {
    if (_swapchain == nullptr) {
        return {false, "Vulkan Present requires a swapchain"};
    }
    const Texture* texture = _view.texture;
    if (texture == nullptr) {
        return {false, "Vulkan Present requires a source texture"};
    }
    if ((texture->GetUsage() & ETextureUsageFlags::TRANSFER_SRC) != ETextureUsageFlags::TRANSFER_SRC) {
        return {false, "Vulkan present source must declare TRANSFER_SRC usage"};
    }
    if (texture->GetDimension() != ETextureDimension::TEX_2D) {
        return {false, "Vulkan present source must be a 2D texture"};
    }
    if (texture->GetAspectFlags() != ETextureAspectFlags::COLOR) {
        return {false, "Vulkan present source must be color-only"};
    }
    if (texture->GetNumSamples() != 1) {
        return {false, "Vulkan present source must be single-sampled for vkCmdCopyImage"};
    }
    if (!AreVulkanCopyFormatsSizeCompatible(texture->GetFormat(), _swapchain->format)) {
        return {false, "Vulkan present source and swapchain formats must be size-compatible"};
    }
    if (_view.offset.x != 0 || _view.offset.y != 0 || _view.offset.z != 0) {
        return {false, "Vulkan present source offset must be zero"};
    }
    if (_view.mip_level != 0 || _view.num_mips != 1) {
        return {false, "Vulkan present source must select exactly mip level zero"};
    }
    if (_view.array_layer != 0 || _view.num_array != 1) {
        return {false, "Vulkan present source must select exactly array layer zero"};
    }

    const uint3 texture_extent = texture->GetExtent();
    if (_view.extent.x != texture_extent.x || _view.extent.y != texture_extent.y ||
        _view.extent.z != texture_extent.z) {
        return {false, "Vulkan present source view must cover the complete texture extent"};
    }
    if (_view.extent.z != 1 || _view.extent.x != _swapchain->size.x || _view.extent.y != _swapchain->size.y) {
        return {false, "Vulkan present source extent must match the 2D swapchain"};
    }
    return {true, ""};
}

static EPresentStatus PresentStatusFromVulkanOutcome(const VulkanOperationResult& _outcome) noexcept {
    switch (_outcome.result) {
        case VK_SUCCESS:
            return EPresentStatus::Success;
        case VK_SUBOPTIMAL_KHR:
            return EPresentStatus::Suboptimal;
        case VK_NOT_READY:
        case VK_TIMEOUT:
            return EPresentStatus::Retry;
        case VK_ERROR_OUT_OF_DATE_KHR:
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT:
            return EPresentStatus::OutOfDate;
        case VK_ERROR_SURFACE_LOST_KHR:
            return EPresentStatus::SurfaceLost;
        case VK_ERROR_DEVICE_LOST:
        case VK_ERROR_OUT_OF_HOST_MEMORY:
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return EPresentStatus::Faulted;
        default:
            break;
    }

    switch (_outcome.status) {
        case EVulkanOperationStatus::Success:
            return EPresentStatus::Success;
        case EVulkanOperationStatus::Retry:
            return EPresentStatus::Retry;
        case EVulkanOperationStatus::Recreate:
            return EPresentStatus::OutOfDate;
        case EVulkanOperationStatus::Faulted:
            return EPresentStatus::Faulted;
        case EVulkanOperationStatus::Rejected:
        default:
            return EPresentStatus::Rejected;
    }
}

static uint8 PresentStatusPriority(EPresentStatus _status) noexcept {
    switch (_status) {
        case EPresentStatus::Success:
            return 1;
        case EPresentStatus::Retry:
            return 2;
        case EPresentStatus::Rejected:
            return 3;
        case EPresentStatus::Suboptimal:
            return 4;
        case EPresentStatus::OutOfDate:
            return 5;
        case EPresentStatus::SurfaceLost:
            return 6;
        case EPresentStatus::Faulted:
            return 7;
        case EPresentStatus::Unresolved:
        default:
            return 0;
    }
}

static void AccumulatePresentReceiptOutcome(
    EPresentStatus&              _status,
    EPresentStage&               _stage,
    const VulkanOperationResult& _outcome,
    EPresentStage                _outcome_stage
) noexcept {
    const EPresentStatus outcome_status = PresentStatusFromVulkanOutcome(_outcome);
    if (PresentStatusPriority(outcome_status) >= PresentStatusPriority(_status)) {
        _status = outcome_status;
        _stage  = _outcome_stage;
    }
}

static void ResolvePresentReceiptNoexcept(
    const PresentReceiptRef& _receipt,
    bool                     _submitted,
    EPresentStatus           _status,
    EPresentStage            _stage
) noexcept {
    if (!_receipt) {
        return;
    }
    try {
        _receipt->Resolve(_submitted, _status, _stage);
    } catch (...) {
        LogQueueErrorNoexcept("[RHIExecutor][Vulkan] failed to resolve Present receipt");
    }
}

} // namespace

VulkanSubmissionResult VulkanCommandQueue::PresentForRuntime(
    SwapchainRef      _swapchain,
    TextureView       _source,
    PresentReceiptRef _receipt
) noexcept {
    assert(executor_claimed.load(std::memory_order_acquire) && "runtime Present requires an exclusively claimed queue");
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission && "runtime Present must run on the Submission owner"
    );
    try {
        if (vk_device.IsFaulted()) {
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(_receipt, false, EPresentStatus::Faulted, EPresentStage::Validation);
            return {.outcome = {EVulkanOperationStatus::Rejected, vk_device.GetFirstFaultResult()}};
        }
        TextureRef source_texture{_source.texture};
#if MOER_VULKAN_TEST_SUPPORT
        const VulkanScriptedPresentOverrideForTesting* scripted_override = GetVulkanScriptedPresentOverrideForTesting();
#endif
        const VulkanPresentSourceContract source_contract = ValidatePresentSourceContract(_source, _swapchain.Get());
        if (!source_contract.valid) {
#if MOER_VULKAN_TEST_SUPPORT
            InvokePresentSourceRejectionCallbackForTesting(scripted_override);
#endif
            bool                        recoverable_rejection = false;
            const VulkanOperationResult rejection             = ClassifyPresentSourceRejection(vk_device, recoverable_rejection);
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(
                _receipt, false, PresentStatusFromVulkanOutcome(rejection), EPresentStage::Validation
            );
            LogQueueErrorNoexcept("[RHIExecutor][Vulkan] rejected Present source contract: {}", source_contract.reason);
            return {
                .outcome               = rejection,
                .recoverable_rejection = recoverable_rejection,
            };
        }
        if (
            !VulkanResourceStateValidator::ValidateExternalAccess(vk_device, queue.GetType(), {_source, {ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_READ, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC}, ETextureAspectFlags::COLOR})
#if MOER_VULKAN_TEST_SUPPORT
            && (scripted_override == nullptr || scripted_override->validate_source_access)
#endif
        ) {
#if MOER_VULKAN_TEST_SUPPORT
            InvokePresentSourceRejectionCallbackForTesting(scripted_override);
#endif
            bool                        recoverable_rejection = false;
            const VulkanOperationResult rejection             = ClassifyPresentSourceRejection(vk_device, recoverable_rejection);
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(
                _receipt, false, PresentStatusFromVulkanOutcome(rejection), EPresentStage::Validation
            );
            LogQueueErrorNoexcept(
                "[RHIExecutor][Vulkan] rejected Present source before an "
                "accepted producer export published TRANSFER_SRC / TRANSFER_READ"
            );
            return {
                .outcome               = rejection,
                .recoverable_rejection = recoverable_rejection,
            };
        }

        auto                         execution_lease = runtime_execution_gate.Acquire();
        std::unique_lock<std::mutex> execution_lock(exec_mtx);
        const uint64                 current_timeline = last_reserved_timeline.fetch_add(1, std::memory_order_relaxed) + 1;
#if MOER_VULKAN_TEST_SUPPORT
        if (scripted_override != nullptr) {
            VulkanScriptedPresentResult scripted =
                scripted_override->callback(scripted_override->context, current_timeline);
            switch (scripted.outcome.status) {
                case EVulkanOperationStatus::Retry:
                case EVulkanOperationStatus::Recreate:
                    break;
                case EVulkanOperationStatus::Rejected:
                    vk_device.RecordRejectedPresent();
                    break;
                case EVulkanOperationStatus::Faulted:
                case EVulkanOperationStatus::Success:
                default:
                    // A headless test hook must never claim native success or
                    // mutate VulkanDevice fault state.
                    scripted.outcome.status = EVulkanOperationStatus::Rejected;
                    if (scripted.outcome.result == VK_SUCCESS) {
                        scripted.outcome.result = VK_ERROR_UNKNOWN;
                    }
                    vk_device.RecordRejectedPresent();
                    break;
            }

            const VulkanOperationContext context{
                .operation   = EVulkanFaultOperation::PresentSubmit,
                .queue_type  = queue.GetType(),
                .queue       = queue.GetHandle(),
                .timeline    = current_timeline,
                .work_serial = current_timeline,
            };
            VulkanPendingSubmission recorded{CmdSubmit{{}, {}, {}, {}}, context, current_timeline};
            recorded.submit.callbacks.emplace_back([swapchain = std::move(_swapchain),
                                                    source    = std::move(source_texture)] {});
            recorded.deferred_releases  = vk_device.GetCommandRecorder(queue.GetType()).TakeDeferredReleases();
            recorded.submission_outcome = scripted.outcome;
            CommitRuntimeSubmitCompletion(recorded, scripted.outcome);
            ResolvePresentReceiptNoexcept(
                _receipt, false, PresentStatusFromVulkanOutcome(scripted.outcome), EPresentStage::Present
            );
            return {
                .outcome               = scripted.outcome,
                .completion            = std::nullopt,
                .recoverable_rejection = false,
            };
        }
#endif
        return PresentSwapchain(
            std::move(_swapchain),
            std::move(source_texture),
            _source,
            std::move(_receipt),
            current_timeline,
            current_timeline
        );
    } catch (...) {
        vk_device.RecordRejectedPresent();
        ResolvePresentReceiptNoexcept(
            _receipt,
            false,
            vk_device.IsFaulted() ? EPresentStatus::Faulted : EPresentStatus::Rejected,
            EPresentStage::Validation
        );
        LogQueueErrorNoexcept("[RHIExecutor][Vulkan] Present owner setup threw before timeline reservation");
        return {
            .outcome = {
                EVulkanOperationStatus::Rejected,
                vk_device.IsFaulted() ? vk_device.GetFirstFaultResult() : VK_ERROR_UNKNOWN
            }
        };
    }
}

void VulkanCommandQueue::Present(SwapchainRef, TextureView, PresentReceiptRef _receipt) {
    LogQueueErrorNoexcept(
        "[RHIExecutor][Vulkan] direct Present is unsupported; "
        "publish work through RHIExecutor"
    );
    vk_device.RecordRejectedPresent();
    ResolvePresentReceiptNoexcept(_receipt, false, EPresentStatus::Rejected, EPresentStage::Validation);
}

VulkanSubmissionResult VulkanCommandQueue::PresentSwapchain(
    SwapchainRef&&    _swapchain,
    TextureRef&&      _source_texture,
    TextureView       _source_view,
    PresentReceiptRef _receipt,
    uint64            _timeline,
    uint64            _serial
) noexcept {
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Submission && "Present must run on the Submission owner");

    const VulkanOperationContext context{
        .operation   = EVulkanFaultOperation::PresentSubmit,
        .queue_type  = queue.GetType(),
        .queue       = queue.GetHandle(),
        .timeline    = _timeline,
        .work_serial = _serial,
    };
    VulkanPendingSubmission recorded{CmdSubmit{{}, {}, {}, {}}, context, _timeline};
    auto*                   swapchain = ResourceCast(_swapchain.Get());
    VulkanOperationResult   final_outcome{EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
    bool                    present_accepted      = false;
    bool                    recoverable_rejection = false;
    EPresentStatus          receipt_status        = EPresentStatus::Unresolved;
    EPresentStage           receipt_stage         = EPresentStage::None;

    try {
        // Retain Copy inputs through the ordinary GPU completion callbacks.
        recorded.submit.callbacks.emplace_back([swapchain = std::move(_swapchain),
                                                source    = std::move(_source_texture)] {});
#if MOER_VULKAN_TEST_SUPPORT
        const auto* scripted_override = GetVulkanScriptedPresentOverrideForTesting();
#endif
        const auto source_contract = ValidatePresentSourceContract(_source_view, swapchain);
        if (vk_device.IsFaulted()) {
            vk_device.RecordRejectedPresent();
            final_outcome = {EVulkanOperationStatus::Rejected, vk_device.GetFirstFaultResult()};
            AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Validation);
        } else if (!source_contract.valid ||
                   !VulkanResourceStateValidator::ValidateExternalAccess(vk_device, queue.GetType(), {_source_view, {ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_READ, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC}, ETextureAspectFlags::COLOR})) {
#if MOER_VULKAN_TEST_SUPPORT
            InvokePresentSourceRejectionCallbackForTesting(scripted_override);
#endif
            final_outcome = ClassifyPresentSourceRejection(vk_device, recoverable_rejection);
            vk_device.RecordRejectedPresent();
            AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Validation);
            LOG_ERROR(
                "[RHIExecutor][Vulkan] rejected Present source contract: {}",
                source_contract.valid ? "source range has no accepted external transfer-read state" : source_contract.reason
            );
        } else {
            recorded.command_context   = vk_device.GetCommandRecorder(queue.GetType()).AcquireContext();
            auto&      command_context = *recorded.command_context;
            const auto acquire         = swapchain->AcquireNextImage(
                command_context.GetImageReadySemaphore(), kPresentAcquireTimeoutNanoseconds
            );
            final_outcome = acquire.outcome;
            AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, acquire.outcome, EPresentStage::Acquire);
            if (acquire.HasImage()) {
                // An acquired binary semaphore cannot be reused if this
                // recording never reaches the GPU wait operation.
                command_context.MarkImageAcquired();
                auto&                 command_list = command_context.GetCommandList();
                VulkanBarrierRecorder barriers;
                auto*                 source_texture    = static_cast<VulkanTexture*>(_source_view.texture);
                const uint32          image_index       = acquire.image_index;
                auto*                 swapchain_texture = ResourceCast(swapchain->GetSwapchainImage(image_index).texture);
                VK_CHECK_RESULT(command_list.Begin());
                const std::string present_label = std::format("Present: {}", source_texture->GetName());
                command_list.BeginLabel(present_label, {0.0f, 0.85f, 0.95f, 1.0f});
                // Source visibility/layout is established by its ordinary exported transfer-read contract.
                barriers.Append(
                    swapchain_texture,
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    0,
                    1,
                    0,
                    1,
                    EBarrierQueueTransferPhase::None,
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_PIPELINE_STAGE_2_NONE,
                    VK_ACCESS_2_NONE,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                );
                barriers.Flush(command_list);
                command_list.InsertLabel("Copy Present Image", GpuMarkerPalette::Transfer());
                command_list.CopyTexture(
                    source_texture,
                    swapchain_texture,
                    _source_view.extent,
                    {0, 0, 0},
                    {0, 0, 0},
                    0,
                    0,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                );
                barriers.Append(
                    swapchain_texture,
                    VK_IMAGE_ASPECT_COLOR_BIT,
                    0,
                    1,
                    0,
                    1,
                    EBarrierQueueTransferPhase::None,
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE,
                    VK_ACCESS_2_NONE,
                    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                );
                barriers.Flush(command_list);
                command_list.EndLabel();
                VK_CHECK_RESULT(command_list.End());

                recorded.has_native_commands = true;
                recorded.native_waits.push_back(
                    VkSemaphoreSubmitInfo{
                        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                        .semaphore = acquire.ready_semaphore,
                        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    }
                );
                recorded.native_signals.push_back(
                    VkSemaphoreSubmitInfo{
                        .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                        .semaphore = swapchain->GetRenderFinishedSemaphore(image_index),
                        .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    }
                );
                recorded.deferred_releases = vk_device.GetCommandRecorder(queue.GetType()).TakeDeferredReleases();
                recorded.resource_state_program.emplace_back(RHIResourceAccess{
                    _source_view, {ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_READ, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC}, ETextureAspectFlags::COLOR
                });
                const VulkanOperationResult copy_result = SubmitRecorded(recorded);
                final_outcome                           = copy_result;
                recoverable_rejection                   = recorded.recoverable_rejection;
                AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Submit);
                if (copy_result.Succeeded()) {
                    const auto result = swapchain->Present(queue.GetHandle(), image_index, _timeline, _serial);
                    present_accepted  = result.accepted;
                    AccumulatePresentReceiptOutcome(
                        receipt_status, receipt_stage, result.outcome, EPresentStage::Present
                    );
                    if (!result.outcome.Succeeded()) {
                        final_outcome = result.outcome;
                    }
                }
            } else {
                recorded.deferred_releases = vk_device.GetCommandRecorder(queue.GetType()).TakeDeferredReleases();
            }
        }
    } catch (const VulkanCallError& error) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedPresent();
        vk_device.TryLatchFirstFault(context, error.GetResult(), false);
        final_outcome = {EVulkanOperationStatus::Faulted, error.GetResult()};
        AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Submit);
    } catch (const std::exception& error) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedPresent();
        final_outcome = {EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
        AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Submit);
        LogQueueErrorNoexcept("[RHIExecutor][Vulkan] Present threw before retirement: {}", error.what());
    } catch (...) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedPresent();
        final_outcome = {EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
        AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Submit);
        LogQueueErrorNoexcept("[RHIExecutor][Vulkan] Present threw before retirement");
    }

    if (!recorded.submission_outcome.has_value()) {
        recorded.submission_outcome = final_outcome;
    }
    const bool gpu_submitted = recorded.submission_outcome->Succeeded();
    // A Present failure must never overwrite an accepted Copy's GPU outcome.
    CommitRuntimeSubmitCompletion(recorded, *recorded.submission_outcome);
    if (receipt_status == EPresentStatus::Unresolved) {
        AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Submit);
    }
    ResolvePresentReceiptNoexcept(_receipt, present_accepted, receipt_status, receipt_stage);
    VulkanSubmissionResult result{
        .outcome               = final_outcome,
        .recoverable_rejection = recoverable_rejection,
    };
    if (gpu_submitted) {
        result.completion = WaitEvent{uint64(timeline.Get()), _timeline};
    }
    return result;
}

} // namespace Moer::Render
