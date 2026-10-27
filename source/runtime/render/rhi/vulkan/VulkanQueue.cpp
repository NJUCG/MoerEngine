#include "VulkanEnumTranslator.h"
#include "VulkanPipeline.h"
#include "VulkanBindlessArray.h"
#include "VulkanRaytracingResource.h"
#include "VulkanTimeline.h"
#include "VulkanQueue.h"
#include "VulkanBarrier.h"
#include "VulkanQueueInternal.h"
#include "VulkanQueueTiming.h"
#include "PixelFormat.h"
#include "VulkanCommandContext.h"
#include "VulkanCommandList.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanMacroUtils.h"
#include "VulkanRHIResource.h"
#include "VulkanSwapChain.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#include "vulkan/VulkanFaultInjection.h"
#endif
#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/RAII.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "misc/Traits.h"
#include "platform/Platform.h"
#include "rhi/ExternalCpuJoinPool.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIExecutorBackend.h"
#include "rhi/RHIMultiview.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIThreadHeartbeat.h"

#include "VulkanCustomCommand.h"
#include "shader/ShaderPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
namespace Moer::Render {
namespace {

// Keep the Submission owner's wait for a swapchain image bounded.
constexpr uint64 kPresentAcquireTimeoutNanoseconds = 50'000'000;

void ValidateTextureTransferDataSize(const VulkanTexture& _texture, uint3 _extent, uint64 _byte_size) {
    const uint64 expected_byte_size = GetSizeFromImageFormat(_texture.GetFormat(), _extent);
    if (_byte_size != expected_byte_size) {
        throw std::invalid_argument(std::format(
            "Texture transfer data size mismatch: texture='{}', bytes={}, expected={}",
            _texture.GetName(),
            _byte_size,
            expected_byte_size
        ));
    }
}

ERHIHeartbeatDomain HeartbeatDomainForQueue(EQueueType _type) noexcept {
    switch (_type) {
        case EQueueType::Graphics:
            return ERHIHeartbeatDomain::Graphics;
        case EQueueType::Compute:
            return ERHIHeartbeatDomain::Compute;
        case EQueueType::Copy:
            return ERHIHeartbeatDomain::Copy;
        default:
            return ERHIHeartbeatDomain::General;
    }
}

struct VulkanPresentSourceContract {
    bool        valid{false};
    const char* reason{"unknown present-source contract failure"};
};

[[nodiscard]] VulkanOperationResult ClassifyPresentSourceRejection(
    VulkanDevice& _device,
    bool&         _recoverable_rejection
) noexcept {
    // This acquire is the rejection linearization point. A device fault may
    // latch after the optimistic check at Present entry while the source
    // contract/readiness is being inspected; never report that already
    // latched hard failure as a recoverable source error.
    const bool faulted = _device.IsFaulted();
    _recoverable_rejection = !faulted;
    return {
        EVulkanOperationStatus::Rejected,
        faulted ? _device.GetFirstFaultResult() : VK_ERROR_UNKNOWN,
    };
}

[[nodiscard]] bool AreVulkanCopyFormatsSizeCompatible(
    EPixelFormat _source,
    EPixelFormat _destination
) noexcept {
    const uint source_index = static_cast<uint>(_source);
    const uint destination_index = static_cast<uint>(_destination);
    constexpr uint first_single_plane_color =
        static_cast<uint>(PF_R4G4_UNORM_PACK8);
    constexpr uint last_single_plane_color =
        static_cast<uint>(PF_E5B9G9R9_UFLOAT_PACK32);
    const auto is_known_single_plane_color = [](
        uint _format_index
    ) noexcept {
        return _format_index >= first_single_plane_color &&
               _format_index <= last_single_plane_color;
    };
    if (!is_known_single_plane_color(source_index) ||
        !is_known_single_plane_color(destination_index)) {
        return false;
    }

    const VulkanPixelFormatInfo& source = VulkanEnumTranslator::GetPixelFormatInfo(_source);
    const VulkanPixelFormatInfo& destination = VulkanEnumTranslator::GetPixelFormatInfo(_destination);
    return source.format != VK_FORMAT_UNDEFINED &&
           destination.format != VK_FORMAT_UNDEFINED &&
           source.stride != 0 &&
           source.stride == destination.stride;
}

[[nodiscard]] VulkanPresentSourceContract ValidatePresentSourceContract(
    const TextureView& _view,
    const Swapchain*   _swapchain
) noexcept {
    if (_swapchain == nullptr) {
        return {false, "Vulkan Present requires a swapchain"};
    }
    const Texture* texture = _view.texture;
    if (texture == nullptr) {
        return {false, "Vulkan Present requires a source texture"};
    }
    if ((texture->GetUsage() &
         ETextureUsageFlags::PRESENTATION_SOURCE) !=
        ETextureUsageFlags::PRESENTATION_SOURCE) {
        return {
            false,
            "Vulkan present source must declare PRESENTATION_SOURCE usage"
        };
    }
    if ((texture->GetUsage() &
         ETextureUsageFlags::TRANSFER_SRC) !=
        ETextureUsageFlags::TRANSFER_SRC) {
        return {
            false,
            "Vulkan present source must declare TRANSFER_SRC usage"
        };
    }
    if (texture->GetDimension() != ETextureDimension::TEX_2D) {
        return {false, "Vulkan present source must be a 2D texture"};
    }
    if (texture->GetAspectFlags() != ETextureAspectFlags::COLOR) {
        return {false, "Vulkan present source must be color-only"};
    }
    if (texture->GetNumSamples() != 1) {
        return {
            false,
            "Vulkan present source must be single-sampled for vkCmdCopyImage"
        };
    }
    if (!AreVulkanCopyFormatsSizeCompatible(
            texture->GetFormat(), _swapchain->format
        )) {
        return {
            false,
            "Vulkan present source and swapchain formats must be size-compatible"
        };
    }
    if (_view.offset.x != 0 || _view.offset.y != 0 ||
        _view.offset.z != 0) {
        return {false, "Vulkan present source offset must be zero"};
    }
    if (_view.mip_level != 0 || _view.num_mips != 1) {
        return {
            false,
            "Vulkan present source must select exactly mip level zero"
        };
    }
    if (_view.array_layer != 0 || _view.num_array != 1) {
        return {
            false,
            "Vulkan present source must select exactly array layer zero"
        };
    }

    const uint3 texture_extent = texture->GetExtent();
    if (_view.extent.x != texture_extent.x ||
        _view.extent.y != texture_extent.y ||
        _view.extent.z != texture_extent.z) {
        return {
            false,
            "Vulkan present source view must cover the complete texture extent"
        };
    }
    if (_view.extent.z != 1 ||
        _view.extent.x != _swapchain->size.x ||
        _view.extent.y != _swapchain->size.y) {
        return {
            false,
            "Vulkan present source extent must match the 2D swapchain"
        };
    }
    return {true, ""};
}

} // namespace

#pragma region[ utils ]

VkRenderingAttachmentInfo FromColorAttachmentInfo(const ColorAttachment& _attachment) {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
    attachment_info.imageView = vk_texture->GetAttachmentView(
        static_cast<uint8>(_attachment.mip_level),
        1,
        static_cast<uint8>(_attachment.array_layer),
        static_cast<uint8>(_attachment.array_count)
    );

    attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment_info.loadOp      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_attachment.action));
    attachment_info.storeOp = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_attachment.action));

    // std::memcpy(attachment_info.clearValue.color.float32, color.float32, sizeof(color.float32));
    attachment_info.clearValue.color = {
        _attachment.clear_color.x,
        _attachment.clear_color.y,
        _attachment.clear_color.z,
        _attachment.clear_color.w
    };

    return attachment_info;
}

static bool FormatHasStencil(EPixelFormat format) {
    return format == PF_D32_SFLOAT_S8_UINT || format == PF_D24_UNORM_S8_UINT ||
           format == PF_D16_UNORM_S8_UINT || format == PF_S8_UINT;
}

static bool PipelineUsesStencilAttachment(const PipelineHandle& pipeline) {
    if (!pipeline.IsValid()) {
        return false;
    }

    auto* vk_pso = static_cast<VulkanPipelineState*>(pipeline.GetState());
    return vk_pso->UsesStencilAttachment();
}

static uint32_t PipelineViewMask(const PipelineHandle& pipeline) {
    if (!pipeline.IsValid()) {
        return 0;
    }

    auto* vk_pso = static_cast<VulkanPipelineState*>(pipeline.GetState());
    return vk_pso->GetViewMask();
}

static void ValidateRenderPassMultiview(const RenderPassInfo& pass_info) {
    const uint32_t required_view_count = Multiview::RequiredViewCount(pass_info.view_mask);
    auto validate_attachment = [required_view_count, view_mask = pass_info.view_mask](
                                   Texture* texture,
                                   uint32_t mip_level,
                                   uint32_t base_layer,
                                   uint32_t layer_count,
                                   std::string_view attachment_name
                               ) {
        if (texture == nullptr || mip_level >= texture->GetNumMips() ||
            !Multiview::IsAttachmentRangeValid(
                base_layer,
                layer_count,
                texture->GetNumArray()
            )) {
            LOG_ERROR(
                "Invalid {} attachment subresource range: mip={}, base_layer={}, "
                "layer_count={}, texture_mips={}, texture_layers={}.",
                attachment_name,
                mip_level,
                base_layer,
                layer_count,
                texture ? texture->GetNumMips() : 0,
                texture ? texture->GetNumArray() : 0
            );
            throw std::invalid_argument("render attachment subresource range is invalid");
        }
        if (!Multiview::AttachmentCoversViewMask(layer_count, view_mask)) {
            LOG_ERROR(
                "Multiview {} attachment exposes {} layers, but view_mask=0x{:x} requires at least {}.",
                attachment_name,
                layer_count,
                view_mask,
                required_view_count
            );
            throw std::invalid_argument("multiview attachment does not cover the view mask");
        }
    };

    for (const ColorAttachment& attachment : pass_info.color_attachments) {
        validate_attachment(
            attachment.target,
            attachment.mip_level,
            attachment.array_layer,
            attachment.array_count,
            "color"
        );
    }
    if (pass_info.depth_attachment.Valid()) {
        validate_attachment(
            pass_info.depth_attachment.target,
            pass_info.depth_attachment.mip_level,
            pass_info.depth_attachment.array_layer,
            pass_info.depth_attachment.array_count,
            "depth"
        );
    }
}

static void ValidatePipelineViewMask(const PipelineHandle& pipeline, const RenderPassInfo& pass_info) {
    const uint32_t pipeline_view_mask = PipelineViewMask(pipeline);
    if (!Multiview::PipelineMatchesRenderPass(pipeline_view_mask, pass_info.view_mask)) {
        LOG_ERROR(
            "Graphics pipeline view mask 0x{:x} does not match rendering view mask 0x{:x}.",
            pipeline_view_mask,
            pass_info.view_mask
        );
        throw std::logic_error("graphics pipeline and rendering view masks do not match");
    }
}

static bool DrawBatchUsesStencilAttachment(const DrawBatch& draw_batch) {
    for (const DrawBatchElement& draw_cmd : draw_batch.draw_cmds) {
        if (PipelineUsesStencilAttachment(draw_cmd.handle)) {
            return true;
        }
    }

    return false;
}

VkRenderingAttachmentInfo FromDepthAttachmentInfo(const DepthAttachment& _attachment) {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
    attachment_info.imageView = vk_texture->GetAttachmentView(
        static_cast<uint8>(_attachment.mip_level),
        1,
        static_cast<uint8>(_attachment.array_layer),
        static_cast<uint8>(_attachment.array_count)
    );

    bool has_stencil            = FormatHasStencil(_attachment.target->GetFormat());
    attachment_info.imageLayout = has_stencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                                                VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    attachment_info.loadOp =
        VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(GetDepthAction(_attachment.action)));
    attachment_info.storeOp =
        VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(GetDepthAction(_attachment.action)));
    // std::memcpy(attachment_info.clearValue.color.float32, color.float32, sizeof(color.float32));
    attachment_info.clearValue.depthStencil = {_attachment.clear_depth, _attachment.clear_stencil};

    return attachment_info;
}

static std::optional<uint32_t> UniformRenderPassSampleCount(
    const RenderPassInfo& _pass_info
) {
    std::optional<uint32_t> sample_count;
    auto merge_samples = [&](const Texture* _texture) {
        if (_texture == nullptr) {
            return false;
        }
        const uint32_t attachment_samples = _texture->GetNumSamples();
        if (sample_count.has_value() && sample_count.value() != attachment_samples) {
            return false;
        }
        sample_count = attachment_samples;
        return true;
    };

    for (const ColorAttachment& attachment : _pass_info.color_attachments) {
        if (!merge_samples(attachment.target)) {
            return std::nullopt;
        }
    }
    if (_pass_info.depth_attachment.Valid() &&
        !merge_samples(_pass_info.depth_attachment.target)) {
        return std::nullopt;
    }
    return sample_count.value_or(1u);
}

// Owns every array/address referenced by the Vulkan rendering and inheritance
// structures. Build it on the coordinator before worker dispatch, then use the
// same immutable signature for vkBeginCommandBuffer and vkCmdBeginRendering.
class VulkanDynamicRenderingEnvelope {
public:
    VulkanDynamicRenderingEnvelope(
        const RenderPassInfo& _pass_info,
        bool                  _uses_stencil_attachment
    ) :
        pass_info(&_pass_info),
        uses_stencil_attachment(_uses_stencil_attachment) {
        const std::optional<uint32_t> uniform_samples =
            UniformRenderPassSampleCount(_pass_info);
        if (!uniform_samples.has_value()) {
            throw std::logic_error(
                "dynamic rendering attachments use incompatible sample counts"
            );
        }
        rasterization_samples =
            VulkanEnumTranslator::METoVKSampleCountFlagBits(uniform_samples.value());

        color_attachments.reserve(_pass_info.color_attachments.size());
        color_formats.reserve(_pass_info.color_attachments.size());
        for (const ColorAttachment& attachment : _pass_info.color_attachments) {
            color_attachments.push_back(FromColorAttachmentInfo(attachment));
            color_formats.push_back(
                VulkanEnumTranslator::METoVKFormat(attachment.target->GetFormat())
            );
        }
        if (_pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment.emplace(
                FromDepthAttachmentInfo(_pass_info.depth_attachment)
            );
            depth_format = VulkanEnumTranslator::METoVKFormat(
                _pass_info.depth_attachment.target->GetFormat()
            );
            stencil_format = _uses_stencil_attachment ? depth_format : VK_FORMAT_UNDEFINED;
        }
    }

    VkRenderingInfo RenderingInfo(VkRenderingFlags _flags = 0) const {
        return {
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = _flags,
            .renderArea =
                {.offset = {pass_info->render_area.offset.x, pass_info->render_area.offset.y},
                 .extent = {pass_info->render_area.extent.width, pass_info->render_area.extent.height}},
            .layerCount           = 1,
            .viewMask             = pass_info->view_mask,
            .colorAttachmentCount = static_cast<uint32_t>(color_attachments.size()),
            .pColorAttachments    = color_attachments.data(),
            .pDepthAttachment = depth_stencil_attachment.has_value() ?
                                    &depth_stencil_attachment.value() :
                                    nullptr,
            .pStencilAttachment =
                depth_stencil_attachment.has_value() && uses_stencil_attachment ?
                    &depth_stencil_attachment.value() :
                    nullptr,
        };
    }

    VkCommandBufferInheritanceRenderingInfo InheritanceInfo() const {
        return {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = 0,
            .viewMask = pass_info->view_mask,
            .colorAttachmentCount = static_cast<uint32_t>(color_formats.size()),
            .pColorAttachmentFormats = color_formats.data(),
            .depthAttachmentFormat = depth_format,
            .stencilAttachmentFormat = stencil_format,
            .rasterizationSamples = rasterization_samples,
        };
    }

private:
    const RenderPassInfo*                    pass_info{};
    bool                                     uses_stencil_attachment{false};
    Array<VkRenderingAttachmentInfo>         color_attachments;
    Array<VkFormat>                          color_formats;
    std::optional<VkRenderingAttachmentInfo> depth_stencil_attachment;
    VkFormat                                 depth_format{VK_FORMAT_UNDEFINED};
    VkFormat                                 stencil_format{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits rasterization_samples{VK_SAMPLE_COUNT_1_BIT};
};

static void FinalizeBindlessUpdatesOnce(
    BindlessArrayRef                         _array,
    const Array<BindlessArray::UpdateCmd>&   _updates,
    const std::shared_ptr<std::atomic_bool>& _finalized,
    bool                                     _gpu_update_succeeded
) {
    if (!_array || !_finalized || _finalized->exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    try {
        reinterpret_cast<VulkanBindlessArray*>(_array.Get())
            ->FinalizeUpdateCommand(_updates, _gpu_update_succeeded);
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "Failed to finalize bindless update lifecycle: {}", error.what()
            );
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("Failed to finalize bindless update lifecycle: unknown error");
        } catch (...) {
        }
    }
}

void FinalizeRejectedBindlessUpdates(const CmdSubmit& _submit) {
    for (const UniquePtr<Command>& command : _submit.cmds) {
        if (!command || command->Type() != Command::EType::UpdateBindlessArray) {
            continue;
        }
        const auto& update = *static_cast<const UpdateBindlessArrayCmd*>(command.get());
        if (!update.UpdatesHandedOff()) {
            continue;
        }
        FinalizeBindlessUpdatesOnce(
            BindlessArrayRef(update.Handle()),
            update.UpdateCommands(),
            update.UpdateFinalizationToken(),
            false
        );
    }
}

void InvokeCallbacksNoexcept(
    Array<std::function<void()>>& _callbacks,
    std::string_view              _context
) noexcept {
    for (std::function<void()>& callback : _callbacks) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (const std::exception& error) {
            try {
                LOG_ERROR("{} callback threw: {}", _context, error.what());
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR("{} callback threw an unknown exception", _context);
            } catch (...) {
            }
        }
    }
}

VulkanBatchCompletionGroup::VulkanBatchCompletionGroup(
    size_t _participant_count
) :
    participants(_participant_count),
    remaining(_participant_count) {
    if (_participant_count == 0) {
        throw std::invalid_argument(
            "Vulkan batch Completion group requires a participant"
        );
    }
}

void VulkanBatchCompletionGroup::Arrive(
    size_t        _participant_index,
    Participant&& _participant
) noexcept {
    Array<std::optional<Participant>> ready{};
    {
        std::lock_guard lock(mutex);
        if (released || _participant_index >= participants.size() ||
            participants[_participant_index].has_value() ||
            remaining == 0) {
            // A duplicate or invalid arrival means one ownership packet was
            // replayed or lost. Continuing could release user callbacks while
            // a sibling is still non-terminal.
            std::terminate();
        }
        participants[_participant_index].emplace(
            std::move(_participant)
        );
        --remaining;
        if (remaining != 0) {
            return;
        }
        released = true;
        ready    = std::move(participants);
    }

    // Every participant has already published its Fence, Completion and Query
    // terminal state. Release Future callbacks across the whole batch before
    // either ordinary tier, and keep resources alive until user code returns.
    // The last arriver is itself a Completion owner.
    for (auto& slot : ready) {
        Participant& participant = *slot;
        if (!participant.gpu_completion_tokens.empty()) {
            GpuCompletionBackendAccess::NotifyTerminals(
                participant.gpu_completion_tokens,
                participant.gpu_completion_batch
            );
            participant.gpu_completion_tokens.clear();
        }
    }
    for (auto& slot : ready) {
        Participant& participant = *slot;
        if (!participant.query_tokens.empty()) {
            QueryBackendAccess::NotifyTerminals(
                participant.query_tokens, participant.query_batch
            );
            participant.query_tokens.clear();
        }
    }
    for (auto& slot : ready) {
        InvokeCallbacksNoexcept(
            slot->callbacks,
            "[VulkanBatchCompletion] runtime completion"
        );
    }
    for (auto& slot : ready) {
        if (!slot->gpu_success) {
            continue;
        }
        InvokeCallbacksNoexcept(
            slot->success_callbacks,
            "[VulkanBatchCompletion] runtime success completion"
        );
    }
    for (auto& slot : ready) {
        Participant& participant = *slot;
        if (participant.deferred_releases.empty()) {
            continue;
        }
        if (participant.device == nullptr) {
            std::terminate();
        }
        if (participant.gpu_success || participant.release_safe) {
            for (auto* resource : participant.deferred_releases) {
                MoerDelete(resource);
            }
        } else {
            for (auto* resource : participant.deferred_releases) {
                participant.device->EnqueueDeferredRelease(resource);
            }
        }
    }

    ready.clear(); // Release retained owners before publishing settlement.
    {
        std::lock_guard lock(mutex);
        if (!released || settled) {
            std::terminate();
        }
        settled = true;
    }
    settled_cv.notify_all();
}

void VulkanBatchCompletionGroup::WaitUntilSettled() {
    std::unique_lock lock(mutex);
    settled_cv.wait(lock, [this] {
        return settled;
    });
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

static uint8_t PresentStatusPriority(EPresentStatus _status) noexcept {
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
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] failed to resolve Present receipt");
        } catch (...) {
        }
    }
}

class StaleBindlessUpdateBatch final : public std::runtime_error {
public:
    StaleBindlessUpdateBatch() :
        std::runtime_error("stale bindless update batch cannot be submitted with dependent commands") {}
};

#pragma endregion

#pragma region[ command translation ]
std::shared_ptr<const VulkanDescriptorRecordingPlan> PrepareDescriptorRecordingPlan(
    VulkanDevice& _device, const CmdSubmit& _submit, const VulkanDescriptorBufferLease& _lease
) {
    auto plan = std::make_shared<VulkanDescriptorRecordingPlan>();
    std::vector<std::vector<uint64_t>> requests;
    std::vector<const Command*> commands;
    for (const auto& command : _submit.cmds) {
        std::vector<uint64_t> binds;
        auto add = [&](const PipelineHandle& pipeline, size_t count = 1) {
            const auto* state = static_cast<const VulkanPipelineState*>(pipeline.GetState());
            if (state->GetDescriptorMode() != VulkanDescriptorMode::Sets) {
                const uint64_t size = state->binding_template->resource_descriptor_allocation_size;
                if (size != 0) {
                    binds.insert(binds.end(), count, size);
                }
            }
        };
        switch (command->Type()) {
            case Command::EType::ShaderDispatch:
                add(static_cast<const DispatchCmd&>(*command).Pipeline());
                break;
            case Command::EType::SetDrawState:
                add(static_cast<const SetDrawStateCmd&>(*command).Pipeline());
                break;
            case Command::EType::MultiDraw:
                for (const auto& element : static_cast<const MultiDrawCmd&>(*command).draw_batch.draw_cmds) {
                    add(element.handle);
                }
                break;
            case Command::EType::UpdateBindlessArray:
                add(_device.GetInternalShaders().sd_component_shuffle.handle, 3);
                break;
            case Command::EType::BuildTLAS:
                add(_device.GetInternalShaders().sd_component_shuffle.handle);
                break;
            default:
                break;
        }
        if (!binds.empty()) {
            commands.push_back(command.get());
            requests.push_back(std::move(binds));
        }
    }
    // Allocate all CPU metadata before acquiring GPU storage.
    for (const auto* command : commands) {
        plan->commands.emplace(command, VulkanDescriptorBufferAllocator::Recording{});
    }
    auto recordings = _device.GetGlobalDescriptorHeap().PrepareDescriptorRecordings(_lease, requests);
    if (!recordings) {
        return nullptr;
    }
    for (size_t i = 0; i < commands.size(); ++i) {
        plan->commands.at(commands[i]) = std::move((*recordings)[i]);
    }
    return plan;
}

class VkCmdVisitor : VulkanDeviceObject {
    enum class EState {
        Barrier,
        Draw,
        Common
    } state = EState::Common;
    VulkanCommandList&     cmd_list;
    VulkanCommandContext&  command_context;
    const TCachedArgArray& cached_args;
    ProfilerStorage*       profiler = nullptr;
    bool                   query_profiling_enabled = false;

    class ScopedNativeLabel {
    public:
        ScopedNativeLabel(VulkanCommandList& _cmd_list, std::string_view _name, float4 _color) :
            cmd_list(_cmd_list) {
            cmd_list.BeginLabel(_name, _color);
        }

        ~ScopedNativeLabel() {
            cmd_list.EndLabel();
        }

        ScopedNativeLabel(const ScopedNativeLabel&)            = delete;
        ScopedNativeLabel& operator=(const ScopedNativeLabel&) = delete;

    private:
        VulkanCommandList& cmd_list;
    };

public:
    VkCmdVisitor(
        VulkanDevice&          _device,
        VulkanCommandContext&  _command_context,
        VulkanCommandList&     _cmd_list,
        const TCachedArgArray& _cached_args,
        ProfilerStorage*       _profiler                   = nullptr,
        bool                   _query_profiling_enabled    = false
    ) :
        VulkanDeviceObject(&_device),
        command_context(_command_context),
        cmd_list(_cmd_list),
        cached_args(_cached_args),
        profiler(_profiler),
        query_profiling_enabled(_query_profiling_enabled) {}

    void VisitCmd(const Command* _cmd) {
        cmd_list.BeginDescriptorCommand(_cmd);
        switch (_cmd->Type()) {
            case Command::EType::UploadBuffer:
                Visit(static_cast<const UploadBufferCmd&>(*_cmd));
                break;
            case Command::EType::CopyBackBuffer:
                Visit(static_cast<const CopyBackBufferCmd&>(*_cmd));
                break;
            case Command::EType::CopyBackTexture:
                Visit(static_cast<const CopyBackTextureCmd&>(*_cmd));
                break;
            case Command::EType::BufferToBuffer:
                Visit(static_cast<const CopyBufferCmd&>(*_cmd));
                break;
            case Command::EType::BufferToTexture:
                Visit(static_cast<const CopyBufferToTextureCmd&>(*_cmd));
                break;
            case Command::EType::TextureToBuffer:
                Visit(static_cast<const CopyTextureToBufferCmd&>(*_cmd));
                break;
            case Command::EType::UploadTexture:
                Visit(static_cast<const UploadTextureCmd&>(*_cmd));
                break;
            case Command::EType::TextureToTexture:
                Visit(static_cast<const CopyTextureCmd&>(*_cmd));
                break;
            case Command::EType::ShaderDispatch:
                Visit(static_cast<const DispatchCmd&>(*_cmd));
                break;
            case Command::EType::BuildAccel:
                Visit(static_cast<const BuildAccelerationStructuresCmd&>(*_cmd));
                break;
            case Command::EType::BuildTLAS:
                Visit(static_cast<const UpdateRaytracingSceneCmd&>(*_cmd));
                break;
            case Command::EType::Barrier:
                Visit(static_cast<const BarrierCmd&>(*_cmd));
                break;
            case Command::EType::QueueTransfer:
                // Visit(static_cast<const QueueTransferCmd&>(*_cmd));
                break;
            case Command::EType::SetDrawState:
                Visit(static_cast<const SetDrawStateCmd&>(*_cmd));
                break;
            case Command::EType::MultiDraw:
                Visit(static_cast<const MultiDrawCmd&>(*_cmd));
                break;
            // case Command::EType::SetGeometryPassDrawState:
            //     Visit(static_cast<const SetGeometryPassDrawStateCmd&>(*_cmd));
            //     break;
            case Command::EType::ClearResource:
                Visit(static_cast<const ClearResourceCmd&>(*_cmd));
                break;
            case Command::EType::TraceRay:
                assert(false && "TraceRay not implemented");
                break;
            case Command::EType::UpdateBindlessArray:
                Visit(static_cast<const UpdateBindlessArrayCmd&>(*_cmd));
                break;
            case Command::EType::Scope:
                Visit(static_cast<const ScopeCmd&>(*_cmd));
                break;
            case Command::EType::Query:
                Visit(static_cast<const QueryCmd&>(*_cmd));
                break;
            case Command::EType::Custom:
                Visit(static_cast<const CustomCmd&>(*_cmd));
                break;
            case Command::EType::SetGeometryPassDrawState:
            case Command::EType::Count:
                assert(false && "Unsupported command reached Vulkan recorder");
                break;
        }
    };
    void Visit(const UploadBufferCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        auto          tmp_buffer = _cmd.staging_buffer;
        VulkanBuffer* buffer     = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
        cmd_list.CopyBuffer(
            reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            buffer,
            _cmd.ByteSize(),
            tmp_buffer.GetByteOffset(),
            _cmd.Offset()
        );
    }

    void Visit(const UploadTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        auto           tmp_buffer = _cmd.staging_buffer;
        VulkanTexture* texture    = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
        cmd_list.CopyBufferToTexture(
            reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            texture,
            tmp_buffer.GetByteOffset(),
            _cmd.Offset(),
            _cmd.Size(),
            _cmd.MipLevel(),
            _cmd.ArrayLayer()
        );
    }

    void Visit(const CopyBufferCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanBuffer* src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
        VulkanBuffer* dst_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

        cmd_list.CopyBuffer(src_buffer, dst_buffer, _cmd.ByteSize(), _cmd.SrcOffset(), _cmd.DstOffset());
    }

    void Visit(const CopyBackBufferCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanBuffer* src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
        auto          tmp_buffer = _cmd.staging_buffer;

        cmd_list.CopyBuffer(
            src_buffer,
            reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            _cmd.ByteSize(),
            _cmd.Offset(),
            tmp_buffer.GetByteOffset()
        );

        // LOG_INFO("copyback temp buffer handle {} offset {} size {}", (uint64)ResourceCast(tmp_buffer.GetBuffer())->GetHandle(), tmp_buffer.GetByteOffset(), tmp_buffer.GetByteSize());

        if (_cmd.HasOwningReadback()) {
            const ReadbackToken readback = _cmd.OwningReadback();
            const uint64 logical_size = _cmd.ByteSize();
            command_context.AddOnComplete(
                [tmp_buffer, device = m_device, readback, logical_size] {
                    struct WriterContext {
                        VulkanDevice* device{};
                        BufferView    staging{};
                        uint64        byte_size{};
                    } context{device, tmp_buffer, logical_size};
                    (void)ReadbackBackendAccess::MaterializePayload(
                        readback,
                        &context,
                        [](void* _context, std::span<byte> _destination) {
                            auto* writer =
                                static_cast<WriterContext*>(_context);
                            if (writer == nullptr ||
                                _destination.size_bytes() !=
                                    writer->byte_size) {
                                throw std::runtime_error(
                                    "Vulkan buffer readback payload size mismatch"
                                );
                            }
                            writer->device->CopyData(
                                _destination.data(),
                                writer->staging,
                                writer->byte_size
                            );
                        }
                    );
                }
            );
        } else {
            command_context.AddOnComplete(
                [tmp_buffer, device = m_device, destination(_cmd.Data()), logical_size = _cmd.ByteSize()] {
                    device->CopyData(
                        destination, tmp_buffer, logical_size
                    );
                }
            );
        }
    }

    void Visit(const CopyBackTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
        auto           tmp_buffer  = _cmd.staging_buffer;

        cmd_list.CopyTextureToBuffer(
            src_texture,
            reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            _cmd.Offset(),
            tmp_buffer.GetByteOffset(),
            _cmd.Size(),
            _cmd.MipLevel(),
            _cmd.ArrayLayer()
        );

        if (_cmd.HasOwningReadback()) {
            const ReadbackToken readback = _cmd.OwningReadback();
            const uint64 logical_size = _cmd.ByteSize();
            command_context.AddOnComplete(
                [tmp_buffer, device = m_device, readback, logical_size] {
                    struct WriterContext {
                        VulkanDevice* device{};
                        BufferView    staging{};
                        uint64        byte_size{};
                    } context{device, tmp_buffer, logical_size};
                    (void)ReadbackBackendAccess::MaterializePayload(
                        readback,
                        &context,
                        [](void* _context, std::span<byte> _destination) {
                            auto* writer =
                                static_cast<WriterContext*>(_context);
                            if (writer == nullptr ||
                                _destination.size_bytes() !=
                                    writer->byte_size) {
                                throw std::runtime_error(
                                    "Vulkan texture readback payload size mismatch"
                                );
                            }
                            writer->device->CopyData(
                                _destination.data(),
                                writer->staging,
                                writer->byte_size
                            );
                        }
                    );
                }
            );
        } else {
            command_context.AddOnComplete(
                [tmp_buffer, device = m_device, destination(_cmd.Data()), logical_size = _cmd.ByteSize()] {
                    device->CopyData(
                        destination.data(), tmp_buffer, logical_size
                    );
                }
            );
        }
    }

    void Visit(const CopyTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
        VulkanTexture* dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

        cmd_list.CopyTexture(
            src_texture,
            dst_texture,
            _cmd.Size(),
            _cmd.SrcOffset(),
            _cmd.DstOffset(),
            _cmd.SrcMipLevel(),
            _cmd.DstMipLevel()
        );
    }

    void Visit(const CopyBufferToTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanBuffer*  src_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
        VulkanTexture* dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

        cmd_list.CopyBufferToTexture(
            src_buffer,
            dst_texture,
            _cmd.SrcOffset(),
            _cmd.DstOffset(),
            _cmd.Size(),
            _cmd.MipLevel(),
            _cmd.ArrayLayer()
        );
    }

    void Visit(const CopyTextureToBufferCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
        VulkanBuffer*  dst_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

        cmd_list.CopyTextureToBuffer(
            src_texture,
            dst_buffer,
            _cmd.SrcOffset(),
            _cmd.DstOffset(),
            _cmd.Size(),
            _cmd.MipLevel(),
            _cmd.ArrayLayer()
        );
    }

    void Visit(const DispatchCmd& _cmd) {
        static float4 dispatch_color = {0.0f, 0.0f, 1.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, dispatch_color);
        const auto& param = _cmd.Param();
        const auto  profile_section_name = _cmd.ProfileSectionName();
        const bool query_timestamp = query_profiling_enabled && profile_section_name != "Other";

        if (query_timestamp) {
            assert(profiler && "profiler is not set");
            profiler->BeginProfilerSession(
                cmd_list, profile_section_name, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            );
        }

        const PipelineHandle& pso = _cmd.Pipeline();
        cmd_list.BindPipeline(_cmd.Pipeline());
        const auto& args = _cmd.Args(cached_args);

        cmd_list.BindShaderArguments(pso, args);

        std::visit(
            Overload{
                [&](uint3 _param) {
                    cmd_list.Dispatch(_param.x, _param.y, _param.z);
                },
                [&](const DispatchIndirectParam& _param) {
                    cmd_list.DispatchIndirect(
                        reinterpret_cast<VulkanBuffer*>(_param.indirect.GetBuffer()),
                        _param.indirect.GetByteOffset()
                    );
                }
            },
            param
        );

        if (query_timestamp) {
            profiler->EndProfilerSession(
                cmd_list, profile_section_name, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
            );
        }

        cmd_list.EndLabel();
    }

    // we don't need to do anything
    void Visit(const BarrierCmd& _cmd) {
        // state                      = EState::Barrier;
        // const auto& read_buffers   = _cmd.ReadBuffers();
        // const auto& write_buffers  = _cmd.WriteBuffers();
        // const auto& read_textures  = _cmd.ReadTextures();
        // const auto& write_textures = _cmd.WriteTextures();

        // for (const auto& buffer : read_buffers) {
        //     auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(buffer.handle);
        // }
        // for (const auto& buffer : write_buffers) {
        //     auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(buffer.handle);
        // }
        // for (const auto& texture : read_textures) {
        //     auto* vk_texture = reinterpret_cast<VulkanTexture*>(texture.handle);
        // }
        // for (const auto& texture : write_textures) {
        //     auto* vk_texture = reinterpret_cast<VulkanTexture*>(texture.handle);
        // }
    }

    void Visit(const SetDrawStateCmd& _cmd) {

        ValidateRenderPassMultiview(_cmd.RenderPassInfo());
        ValidatePipelineViewMask(_cmd.Pipeline(), _cmd.RenderPassInfo());

        static float4 draw_color = {0.0f, 1.0f, 0.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, draw_color);
        state = EState::Draw;

        const auto&     args = _cmd.Args();
        const PipelineHandle& pso  = _cmd.Pipeline();

        const auto& pass_info = _cmd.RenderPassInfo();

        uint tex_min_width  = pass_info.render_area.extent.width + pass_info.render_area.offset.x;
        uint tex_min_height = pass_info.render_area.extent.height + pass_info.render_area.offset.y;

        for (size_t i = 0; i < pass_info.color_attachments.size(); ++i) {
            if (pass_info.color_attachments[i].target->GetWidth() < tex_min_width ||
                pass_info.color_attachments[i].target->GetHeight() < tex_min_height) {
                LOG_ERROR(
                    "Render target size is smaller than render area! target size: {}x{}, render area size: "
                    "{}x{}. Tex Name: {}. Command Name: {}",
                    pass_info.color_attachments[i].target->GetWidth(),
                    pass_info.color_attachments[i].target->GetHeight(),
                    tex_min_width,
                    tex_min_height,
                    pass_info.color_attachments[i].target->GetName(),
                    _cmd.name
                );
            }
        }
        const bool uses_stencil_attachment =
            pass_info.depth_attachment.Valid() &&
            FormatHasStencil(pass_info.depth_attachment.target->GetFormat()) &&
            PipelineUsesStencilAttachment(_cmd.Pipeline());
        VulkanDynamicRenderingEnvelope rendering_envelope(pass_info, uses_stencil_attachment);
        VkRenderingInfo dynamic_rendering_info = rendering_envelope.RenderingInfo();
        cmd_list.BeginRendering(dynamic_rendering_info);

        cmd_list.BindPipeline(_cmd.Pipeline());

        cmd_list.BindShaderArguments(pso, args);

        const auto& cmd_vertex_buffers = _cmd.VertexBuffers();
        const auto& draw_datas         = _cmd.DrawData();
        const auto& rect               = pass_info.render_area;
        VkViewport  viewport{
            .x        = float(rect.offset.x),
            .y        = float(rect.offset.y),
            .width    = float(rect.extent.width),
            .height   = float(rect.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };

        viewport.y += viewport.height;
        viewport.height = -viewport.height;

        cmd_list.SetViewport(viewport);
        cmd_list.SetScissor({rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height});
        for (const auto& draw_data : draw_datas) {
            auto num_of_vertex_buffers = draw_data.vtx_views.size();
            if (num_of_vertex_buffers > 0) {

                Array<VkBuffer>     vertex_buffers;
                Array<VkDeviceSize> vtx_offsets;

                vertex_buffers.reserve(num_of_vertex_buffers);
                vtx_offsets.reserve(num_of_vertex_buffers);

                for (const auto& vtx_view : draw_data.vtx_views) {
                    vertex_buffers.emplace_back(ResourceCast(vtx_view.buffer)->GetHandle());
                    vtx_offsets.emplace_back(vtx_view.offset);
                }

                cmd_list.SetVertexBuffers(0, vertex_buffers, vtx_offsets);
            }

            std::visit(
                Overload{
                    [&](const IndexBuffer& _idx_input) {
                        const auto& index_buffer = _idx_input.buffer;
                        uint64      offset       = index_buffer.GetByteOffset();

                        cmd_list.SetIndexBuffer(
                            reinterpret_cast<VulkanBuffer*>(index_buffer.GetBuffer()),
                            index_buffer.GetByteOffset(),
                            VulkanEnumTranslator::METoVKIndexType(_idx_input.stride)
                        );

                        for (const auto& draw_param : draw_data.draw_params) {
                            cmd_list.DrawIndexedInstanced(
                                draw_param.index_cnt,
                                draw_param.instance_cnt,
                                draw_param.first_index,
                                draw_param.vertex_offset,
                                draw_param.first_instance
                            );
                        }

                        if (draw_data.indirect_draw_param.has_value()) {
                            VulkanBuffer* indirect_buffer =
                                ResourceCast(draw_data.indirect_draw_param->buffer.GetBuffer());
                            if (draw_data.indirect_draw_param->count_buffer.has_value()) {
                                //draw indirect with count buffer
                                auto* count_buffer =
                                    ResourceCast(draw_data.indirect_draw_param->count_buffer->GetBuffer());
                                cmd_list.DrawIndexedIndirectCount(
                                    indirect_buffer,
                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                    count_buffer,
                                    draw_data.indirect_draw_param->count_buffer->GetByteOffset(),
                                    draw_data.indirect_draw_param->count,
                                    draw_data.indirect_draw_param->stride
                                );

                            } else {
                                //draw indirect without count buffer
                                cmd_list.DrawIndexedIndirect(
                                    indirect_buffer,
                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                    draw_data.indirect_draw_param->count,
                                    draw_data.indirect_draw_param->stride
                                );
                            }
                        }
                    },
                    [&](uint _idx_input) {
                        for (const auto& draw_param : draw_data.draw_params) {
                            cmd_list.DrawInstanced(
                                draw_param.index_cnt,
                                draw_param.instance_cnt,
                                draw_param.vertex_offset,
                                draw_param.first_instance
                            );
                        }

                        //draw indirect
                        if (draw_data.indirect_draw_param.has_value()) {
                            VulkanBuffer* indirect_buffer =
                                ResourceCast(draw_data.indirect_draw_param->buffer.GetBuffer());
                            if (draw_data.indirect_draw_param->count_buffer.has_value()) {
                                //draw indirect with count buffer
                                auto* count_buffer =
                                    ResourceCast(draw_data.indirect_draw_param->count_buffer->GetBuffer());
                                cmd_list.DrawIndirectCount(
                                    indirect_buffer,
                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                    count_buffer,
                                    draw_data.indirect_draw_param->count_buffer->GetByteOffset(),
                                    draw_data.indirect_draw_param->count,
                                    draw_data.indirect_draw_param->stride
                                );

                            } else {
                                //draw indirect without count buffer
                                cmd_list.DrawIndirect(
                                    indirect_buffer,
                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                    draw_data.indirect_draw_param->count,
                                    draw_data.indirect_draw_param->stride
                                );
                            }
                        }
                    }
                },
                draw_data.idx_view
            );
        }
        cmd_list.EndRendering();
        cmd_list.EndLabel();
    }

    void Visit(const MultiDrawCmd& _cmd) {
        ValidateRenderPassMultiview(_cmd.RenderPassInfo());
        for (const DrawBatchElement& draw_cmd : _cmd.draw_batch.draw_cmds) {
            ValidatePipelineViewMask(draw_cmd.handle, _cmd.RenderPassInfo());
        }

        static float4 draw_color = {0.0f, 1.0f, 0.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, draw_color);
        state = EState::Draw;

        const auto& pass_info = _cmd.RenderPassInfo();
        const bool uses_stencil_attachment =
            pass_info.depth_attachment.Valid() &&
            FormatHasStencil(pass_info.depth_attachment.target->GetFormat()) &&
            DrawBatchUsesStencilAttachment(_cmd.draw_batch);
        VulkanDynamicRenderingEnvelope rendering_envelope(pass_info, uses_stencil_attachment);
        VkRenderingInfo dynamic_rendering_info = rendering_envelope.RenderingInfo();
        cmd_list.BeginRendering(dynamic_rendering_info);
        const auto& rect = pass_info.render_area;
        VkViewport  viewport{
            .x        = float(rect.offset.x),
            .y        = float(rect.offset.y),
            .width    = float(rect.extent.width),
            .height   = float(rect.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };
        viewport.y += viewport.height;
        viewport.height = -viewport.height;
        cmd_list.SetViewport(viewport);
        cmd_list.SetScissor({rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height});

        for (const DrawBatchElement& draw_cmd : _cmd.draw_batch.draw_cmds) {

            cmd_list.BindPipeline(draw_cmd.handle);
            const ArrayArguments* arg = std::holds_alternative<ArrayArguments>(draw_cmd.args) ?
                                            &std::get<ArrayArguments>(draw_cmd.args) :
                                            (std::holds_alternative<ArrayArgReference>(draw_cmd.args) ?
                                                 &cached_args[std::get<ArrayArgReference>(draw_cmd.args)()] :
                                                 nullptr);

            PipelineHandle& pipeline = *const_cast<PipelineHandle*>(&draw_cmd.handle);
            cmd_list.BindShaderArguments(pipeline, *arg);

            std::visit(
                Overload{
                    [&](const Array<MeshDrawData>& _mesh_draw_cmds) {
                        for (const auto& draw_data : _mesh_draw_cmds) {
                            auto num_of_vertex_buffers = draw_data.vtx_views.size();
                            if (num_of_vertex_buffers > 0) {

                                Array<VkBuffer>     vertex_buffers;
                                Array<VkDeviceSize> vtx_offsets;

                                vertex_buffers.reserve(num_of_vertex_buffers);
                                vtx_offsets.reserve(num_of_vertex_buffers);

                                for (const auto& vtx_view : draw_data.vtx_views) {
                                    vertex_buffers.emplace_back(ResourceCast(vtx_view.buffer)->GetHandle());
                                    vtx_offsets.emplace_back(vtx_view.offset);
                                }

                                cmd_list.SetVertexBuffers(0, vertex_buffers, vtx_offsets);
                            }

                            std::visit(
                                Overload{
                                    [&](const IndexBuffer& _idx_input) {
                                        const auto& index_buffer = _idx_input.buffer;
                                        uint64      offset       = index_buffer.GetByteOffset();

                                        cmd_list.SetIndexBuffer(
                                            reinterpret_cast<VulkanBuffer*>(index_buffer.GetBuffer()),
                                            index_buffer.GetByteOffset(),
                                            VulkanEnumTranslator::METoVKIndexType(_idx_input.stride)
                                        );

                                        for (const auto& draw_param : draw_data.draw_params) {
                                            cmd_list.DrawIndexedInstanced(
                                                draw_param.index_cnt,
                                                draw_param.instance_cnt,
                                                draw_param.first_index,
                                                draw_param.vertex_offset,
                                                draw_param.first_instance
                                            );
                                        }

                                        if (draw_data.indirect_draw_param.has_value()) {
                                            VulkanBuffer* indirect_buffer = ResourceCast(
                                                draw_data.indirect_draw_param->buffer.GetBuffer()
                                            );
                                            if (draw_data.indirect_draw_param->count_buffer.has_value()) {
                                                //draw indirect with count buffer
                                                auto* count_buffer = ResourceCast(
                                                    draw_data.indirect_draw_param->count_buffer->GetBuffer()
                                                );
                                                cmd_list.DrawIndexedIndirectCount(
                                                    indirect_buffer,
                                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                                    count_buffer,
                                                    draw_data.indirect_draw_param->count_buffer
                                                        ->GetByteOffset(),
                                                    draw_data.indirect_draw_param->count,
                                                    draw_data.indirect_draw_param->stride
                                                );

                                            } else {
                                                //draw indirect without count buffer
                                                cmd_list.DrawIndexedIndirect(
                                                    indirect_buffer,
                                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                                    draw_data.indirect_draw_param->count,
                                                    draw_data.indirect_draw_param->stride
                                                );
                                            }
                                        }
                                    },
                                    [&](uint _idx_input) {
                                        for (const auto& draw_param : draw_data.draw_params) {
                                            cmd_list.DrawInstanced(
                                                draw_param.index_cnt,
                                                draw_param.instance_cnt,
                                                draw_param.vertex_offset,
                                                draw_param.first_instance
                                            );
                                        }

                                        //draw indirect
                                        if (draw_data.indirect_draw_param.has_value()) {
                                            VulkanBuffer* indirect_buffer = ResourceCast(
                                                draw_data.indirect_draw_param->buffer.GetBuffer()
                                            );
                                            if (draw_data.indirect_draw_param->count_buffer.has_value()) {
                                                //draw indirect with count buffer
                                                auto* count_buffer = ResourceCast(
                                                    draw_data.indirect_draw_param->count_buffer->GetBuffer()
                                                );
                                                cmd_list.DrawIndirectCount(
                                                    indirect_buffer,
                                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                                    count_buffer,
                                                    draw_data.indirect_draw_param->count_buffer
                                                        ->GetByteOffset(),
                                                    draw_data.indirect_draw_param->count,
                                                    draw_data.indirect_draw_param->stride
                                                );

                                            } else {
                                                //draw indirect without count buffer
                                                cmd_list.DrawIndirect(
                                                    indirect_buffer,
                                                    draw_data.indirect_draw_param->buffer.GetByteOffset(),
                                                    draw_data.indirect_draw_param->count,
                                                    draw_data.indirect_draw_param->stride
                                                );
                                            }
                                        }
                                    }
                                },
                                draw_data.idx_view
                            );
                        }
                    },

                    [&](const Array<DispatchMeshData>& _mesh) {
                        for (const auto& draw_data : _mesh) {
                            std::visit(
                                Overload{
                                    [&](const IndirectDrawParam& _indirect) {
                                        VulkanBuffer* indirect_buffer =
                                            ResourceCast(_indirect.buffer.GetBuffer());
                                        if (_indirect.count_buffer.has_value()) {
                                            //draw indirect with count buffer
                                            auto* count_buffer =
                                                ResourceCast(_indirect.count_buffer->GetBuffer());
                                            cmd_list.DispatchMeshIndirectCount(
                                                indirect_buffer,
                                                _indirect.buffer.GetByteOffset(),
                                                count_buffer,
                                                _indirect.count_buffer->GetByteOffset(),
                                                _indirect.count,
                                                _indirect.stride
                                            );

                                        } else {
                                            //draw indirect without count buffer
                                            cmd_list.DispatchMeshIndirect(
                                                indirect_buffer,
                                                _indirect.buffer.GetByteOffset(),
                                                _indirect.count,
                                                _indirect.stride
                                            );
                                        }
                                    },
                                    [&](Vector3ui _dim) {
                                        cmd_list.DispatchMesh(_dim.x, _dim.y, _dim.z);
                                    }

                                },
                                draw_data.draw_param
                            );
                        }
                    }
                },
                draw_cmd.mesh_dispatch_data
            );
        }

        cmd_list.EndRendering();
        cmd_list.EndLabel();
    }

    // void Visit(const SetGeometryPassDrawStateCmd& _cmd) {
    //     static float4 draw_color = {0.0f, 1.0f, 0.0f, 1.0f};
    //     cmd_list.BeginLabel(_cmd.name, draw_color);
    //     state = EState::Draw;

    //     const auto& args = _cmd.Args();

    //     const auto&                      pass_info = _cmd.RenderPassInfo();
    //     Array<VkRenderingAttachmentInfo> color_attachments(pass_info.color_attachments.size());
    //     for (size_t i = 0; i < pass_info.color_attachments.size(); ++i) {
    //         color_attachments[i] = FromColorAttachmentInfo(pass_info.color_attachments[i]);
    //     }
    //     std::optional<VkRenderingAttachmentInfo> depth_stencil_attachment;
    //     if (pass_info.depth_attachment.Valid()) {
    //         depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
    //     }

    //     VkRenderingInfo dynamic_rendering_info{
    //         .sType      = VK_STRUCTURE_TYPE_RENDERING_INFO,
    //         .pNext      = nullptr,
    //         .flags      = 0,
    //         .renderArea = {
    //             .offset = {pass_info.render_area.offset.x, pass_info.render_area.offset.y},
    //             .extent = {pass_info.render_area.extent.width, pass_info.render_area.extent.height}},
    //         .layerCount           = 1,
    //         .colorAttachmentCount = uint(pass_info.color_attachments.size()),
    //         .pColorAttachments    = color_attachments.data(),
    //         .pDepthAttachment     = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr,
    //         .pStencilAttachment   = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr};

    //     cmd_list.BeginRendering(dynamic_rendering_info);

    //     for (const auto& [bitmask, pso] : _cmd.PipelineMap()) {
    //         cmd_list.BindPipeline(pso);
    //         cmd_list.BindShaderArguments(pso, args);

    //         if (args.constants.size() > 0) {
    //             cmd_list.UploadPushConstants(
    //                 pso,
    //                 std::span<const uint>(args.constants.data(), args.constants.size()));
    //         }
    //         const auto& draw_datas = _cmd.DrawDataArrayMap().at(bitmask);
    //         const auto& rect       = pass_info.render_area;
    //         VkViewport  viewport{
    //              .x        = float(rect.offset.x),
    //              .y        = float(rect.offset.y),
    //              .width    = float(rect.extent.width),
    //              .height   = float(rect.extent.height),
    //              .minDepth = 0.0f,
    //              .maxDepth = 1.0f};

    //         viewport.y += viewport.height;
    //         viewport.height = -viewport.height;

    //         cmd_list.SetViewport(viewport);
    //         cmd_list.SetScissor({rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height});
    //         for (const auto& draw_data : draw_datas) {
    //             auto num_of_vertex_buffers = draw_data.vtx_views.size();
    //             if (num_of_vertex_buffers > 0) {

    //                 Array<VkBuffer>     vertex_buffers;
    //                 Array<VkDeviceSize> vtx_offsets;

    //                 vertex_buffers.reserve(num_of_vertex_buffers);
    //                 vtx_offsets.reserve(num_of_vertex_buffers);

    //                 for (const auto& vtx_view : draw_data.vtx_views) {
    //                     vertex_buffers.emplace_back(ResourceCast(vtx_view.buffer)->GetHandle());
    //                     vtx_offsets.emplace_back(vtx_view.offset);
    //                 }

    //                 cmd_list.SetVertexBuffers(0,
    //                                           num_of_vertex_buffers,
    //                                           std::span<VkBuffer>(vertex_buffers.data(),
    //                                                               num_of_vertex_buffers),
    //                                           std::span<VkDeviceSize>(vtx_offsets.data(),
    //                                                                   num_of_vertex_buffers));
    //             }

    //             std::visit(
    //                 Overload{[&](const IndexBuffer& _idx_input) {
    //                              const auto& index_buffer = _idx_input.buffer;
    //                              uint64      offset       = index_buffer.GetByteOffset();

    //                              cmd_list.SetIndexBuffer(
    //                                  reinterpret_cast<VulkanBuffer*>(index_buffer.GetBuffer()),
    //                                  index_buffer.GetByteOffset(),
    //                                  VulkanEnumTranslator::METoVKIndexType(_idx_input.stride));

    //                              for (const auto& draw_param : draw_data.draw_params) {
    //                                  cmd_list.DrawIndexedInstanced(draw_param.index_cnt,
    //                                                                draw_param.instance_cnt,
    //                                                                draw_param.first_index,
    //                                                                draw_param.vertex_offset,
    //                                                                draw_param.first_instance);
    //                              }
    //                          },
    //                          [&](uint _idx_input) {
    //                              for (const auto& draw_param : draw_data.draw_params) {
    //                                  cmd_list.DrawInstanced(draw_param.index_cnt,
    //                                                         draw_param.instance_cnt,
    //                                                         draw_param.vertex_offset,
    //                                                         draw_param.first_instance);
    //                              }
    //                          }},
    //                 draw_data.idx_view);
    //         }
    //     }
    //     cmd_list.EndRendering();
    //     cmd_list.EndLabel();
    // }

    void Visit(const ClearResourceCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        std::visit(
            Overload{
                [&](const TextureView& _arg) {
                    auto*                   vk_texture = ResourceCast(_arg.GetTexture());
                    VkImageSubresourceRange range{
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = _arg.mip_level,
                        .levelCount     = _arg.num_mips,
                        .baseArrayLayer = _arg.array_layer,
                        .layerCount     = _arg.num_array
                    };
                    VkClearColorValue value;
                    std::visit(
                        Overload{
                            [&](float4 _val) {
                                value = VkClearColorValue{.float32 = {_val.x, _val.y, _val.z, _val.w}};
                            },
                            [&](uint _val) {
                                value = VkClearColorValue{.uint32 = {_val, _val, _val, _val}};
                            }
                        },
                        _cmd.ClearValue()
                    );
                    cmd_list.ClearColorTexture(vk_texture, value, range);
                },
                [&](BufferView _arg) {
                    auto* vk_buffer = ResourceCast(_arg.GetBuffer());
                    cmd_list.ClearBufferUInt(
                        vk_buffer, _arg.GetByteOffset(), _arg.GetByteSize(), _cmd.UIntValue()
                    );
                }
            },
            _cmd.Resource()
        );
    }

    void Visit(const UpdateBindlessArrayCmd& _cmd) {
        VulkanBindlessArray* bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd.Handle());
        if (!_cmd.HandOffUpdates()) {
            return;
        }
        const bool update_is_current = bindless_array->BeginUpdateCommand(_cmd.UpdateCommands());
        BindlessArrayRef array_ref(_cmd.Handle());
        auto             updates   = _cmd.UpdateCommands();
        auto             finalized = _cmd.UpdateFinalizationToken();
        // Keep released resources conservatively visible through GPU
        // completion. This covers prior packets on every queue, prevents
        // descriptor-slot reuse while old work can still read it, and
        // gives record rejection a one-shot finalization token.
        command_context.AddOnComplete([array_ref = std::move(array_ref),
                                       updates   = std::move(updates),
                                       finalized = std::move(finalized),
                                       update_is_current]() mutable {
            FinalizeBindlessUpdatesOnce(array_ref, updates, finalized, update_is_current);
        });
        if (!update_is_current) {
            LOG_WARNING(
                "Rejecting command submission containing a stale bindless update batch"
            );
            // Silently omitting the upload would allow later draw/dispatch
            // commands in this same CmdSubmit to consume descriptors that were
            // never published. Let the queue reject the complete transaction.
            throw StaleBindlessUpdateBatch{};
        }

        auto          array_data          = _cmd.StealArrayData();
        auto          array_indices_dat   = _cmd.StealArrayIndicesData();
        auto          texture_data        = _cmd.StealTextureData();
        auto          texture_indices_dat = _cmd.StealTextureIndicesData();
        Array<byte>&& buffer_data         = _cmd.StealBufferData();
        auto          buffer_indices_dat  = _cmd.StealBufferIndicesData();

        auto& shuffle_sd = m_device->GetInternalShaders().sd_component_shuffle;

        bool b_array   = !array_data.empty();
        bool b_texture = !texture_data.empty();
        bool b_buffer  = !buffer_data.empty();

        //buffer for all data
        BufferView staging_buffer{};

        //staging data views
        BufferView texture_desc_staging{};
        BufferView buffer_desc_staging{};
        BufferView array_staging{};

        //indices data views
        BufferView array_indices_buf{};
        BufferView texture_indices_buf{};
        BufferView buffer_indices_buf{};

        //calculate all staging size and allocate buffer
        if (b_array) {
            uint storage_alignment = 256u;

            //calculate all staging size
            uint current_offset = 0;
            uint staging_size = 0, array_staging_offset = 0, texture_staging_offset = 0,
                 buffer_staging_offset = 0;
            uint array_staging_size = 0, texture_staging_size = 0, buffer_staging_size = 0;
            uint array_indices_offset = 0, texture_indices_offset = 0, buffer_indices_offset = 0;
            uint array_indices_size = 0, texture_indices_size = 0, buffer_indices_size = 0;

            if (b_array) {
                array_staging_size   = array_data.size();
                staging_size         = array_staging_size;
                array_indices_offset = Moer::AlignUp(array_staging_size, storage_alignment);

                array_indices_size = sizeof(uint) * (array_indices_dat.size()) * 2;

                current_offset = Moer::AlignUp(array_indices_size + array_indices_offset, storage_alignment);
            }

            if (b_texture) {
                texture_staging_size   = texture_data.size();
                texture_staging_offset = current_offset;

                texture_indices_offset =
                    Moer::AlignUp(texture_staging_size + texture_staging_offset, storage_alignment);
                texture_indices_size = sizeof(uint) * texture_indices_dat.size() * 2;
                current_offset =
                    Moer::AlignUp(texture_indices_size + texture_indices_offset, storage_alignment);
            }

            if (b_buffer) {
                buffer_staging_size   = buffer_data.size();
                buffer_staging_offset = current_offset;
                buffer_indices_offset =
                    Moer::AlignUp(buffer_staging_size + buffer_staging_offset, storage_alignment);
                buffer_indices_size = sizeof(uint) * buffer_indices_dat.size() * 2;

                current_offset =
                    Moer::AlignUp(buffer_indices_size + buffer_indices_offset, storage_alignment);
            }

            staging_size = current_offset;

            staging_buffer = command_context.GetTemporaryBufferAllocator().AllocateShaderBuffer(staging_size);

            //copy cpu data to staging buffer
            if (b_array) {
                array_staging     = staging_buffer.buffer->GetView(array_staging_offset, array_staging_size);
                array_indices_buf = staging_buffer.buffer->GetView(array_indices_offset, array_indices_size);
                m_device->CopyData(array_staging, array_data.data(), array_staging_size);
                m_device->CopyData(array_indices_buf, array_indices_dat.data(), array_indices_size);
            }

            if (b_texture) {
                texture_desc_staging =
                    staging_buffer.buffer->GetView(texture_staging_offset, texture_staging_size);
                texture_indices_buf =
                    staging_buffer.buffer->GetView(texture_indices_offset, texture_indices_size);
                m_device->CopyData(texture_desc_staging, texture_data.data(), texture_staging_size);
                m_device->CopyData(texture_indices_buf, texture_indices_dat.data(), texture_indices_size);
            }

            if (b_buffer) {
                buffer_desc_staging =
                    staging_buffer.buffer->GetView(buffer_staging_offset, buffer_staging_size);
                buffer_indices_buf =
                    staging_buffer.buffer->GetView(buffer_indices_offset, buffer_indices_size);
                m_device->CopyData(buffer_desc_staging, buffer_data.data(), buffer_staging_size);
                m_device->CopyData(buffer_indices_buf, buffer_indices_dat.data(), buffer_indices_size);
            }
        }

        //recording bindless array updates
        if (b_array) {
            cmd_list.BeginLabel("UpdateBindlessArray", {0.0f, 1.0f, 0.0f, 1.0f});
            cmd_list.BindPipeline(shuffle_sd.handle);
            ComponentShuffleShader::Arg arg;

            {
                //bindless array update
                arg.component_cnt = array_indices_dat.size();
                arg.stride        = sizeof(uint) >> 2;

                cmd_list.BindShaderArguments(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg,
                        array_indices_buf,
                        array_staging,
                        bindless_array->GetIndexBuffer()->GetView()
                    )
                );
                cmd_list.Dispatch((array_indices_dat.size() + 63) / 64, 1, 1);
            }

            if (b_texture) {
                arg.component_cnt = texture_indices_dat.size();
                arg.stride = m_device->GetGlobalDescriptorHeap().GetDescriptorSize(
                                 VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                             ) >>
                             2;

                cmd_list.BindShaderArguments(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg,
                        texture_indices_buf,
                        texture_desc_staging,
                        bindless_array->GetTextureSamplerDescriptorBuffer()->GetView(
                            bindless_array->texture_offset_in_buffer
                        )
                    )
                );
                cmd_list.Dispatch((texture_indices_dat.size() + 63) / 64, 1, 1);
            }

            if (b_buffer) {
                arg.component_cnt = buffer_indices_dat.size();
                arg.stride = m_device->GetGlobalDescriptorHeap().GetDescriptorSize(
                                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                             ) >>
                             2;

                cmd_list.BindShaderArguments(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg,
                        buffer_indices_buf,
                        buffer_desc_staging,
                        bindless_array->GetBufferDescriptorBuffer()->GetView(
                            bindless_array->buffer_descriptors_byte_offset
                        )
                    )
                );
                cmd_list.Dispatch((buffer_indices_dat.size() + 63) / 64, 1, 1);
            }

            cmd_list.EndLabel();
        }

    }

    void Visit(const ScopeCmd& _cmd) {
        if (_cmd.IsPush()) {
            cmd_list.BeginLabel(_cmd.ScopeName(), _cmd.Color());
            if (_cmd.QueryTimestamp() && query_profiling_enabled) {
                assert(profiler && "profiler is not set");
                profiler->BeginProfilerSession(cmd_list, _cmd.ScopeName());
            }
        } else {
            if (_cmd.QueryTimestamp() && query_profiling_enabled) {
                assert(profiler && "profiler is not set");
                profiler->EndProfilerSession(cmd_list, _cmd.ScopeName());
            }
            cmd_list.EndLabel();
        }
    }

    void Visit(const QueryCmd& _cmd) {
        command_context.RecordQuery(cmd_list, _cmd);
    }

    void Visit(const BuildAccelerationStructuresCmd& _cmd) {
        const Array<AccelerationStructureBuildParam>& build_params = _cmd.Params();

        Array<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
        Array<VkAccelerationStructureBuildRangeInfoKHR*>   build_ranges;

        uint64 scratch_alignment =
            m_device->GetOptionalProperties()
                .acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;

        BufferView    scratch_view    = _cmd.Scratch();
        VulkanBuffer* scratch_buf     = ResourceCast(scratch_view.GetBuffer());
        uint64        scratch_address = scratch_buf->DeviceAddress();
        //align scratch address
        scratch_address = Moer::AlignUp(scratch_address, scratch_alignment);

        build_infos.reserve(build_params.size());
        build_ranges.reserve(build_params.size());

        uint64 scratch_offset = 0;

        for (const auto& build_param : build_params) {
            VulkanRaytracingGeometry* geometry = ResourceCast(build_param.geometry.Get());
            build_ranges.emplace_back(geometry->build_ranges.data());

            VkAccelerationStructureBuildGeometryInfoKHR build_info{
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
            };

            build_info.dstAccelerationStructure = geometry->GetHandle();
            if (build_param.mode == ERaytracingBuildMode::UPDATE) {
                build_info.srcAccelerationStructure = geometry->GetHandle();
            }
            build_info.type          = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
            build_info.geometryCount = geometry->build_geometries.size();
            build_info.pGeometries   = geometry->build_geometries.data();
            build_info.mode = VulkanEnumTranslator::METoVKBuildAccelerationStructureMode(build_param.mode);
            build_info.flags =
                VulkanEnumTranslator::METoVKAccelerationStructureBuildType(geometry->GetInfo().build_flags);
            build_info.scratchData.deviceAddress = scratch_address + scratch_offset;
            build_infos.emplace_back(build_info);

            scratch_offset = Moer::AlignUp(scratch_offset, scratch_alignment);
            scratch_offset += build_param.mode == ERaytracingBuildMode::BUILD ?
                                  geometry->build_sizes_info.buildScratchSize :
                                  geometry->build_sizes_info.updateScratchSize;
        }
        cmd_list.BeginLabel(std::format("BuildBLAS {}", build_infos.size()), {});
        cmd_list.BuildAccelerationStructures(build_infos, build_ranges);
        cmd_list.EndLabel();
    }

    void Visit(const UpdateRaytracingSceneCmd& _cmd) {
        const auto& to_update = _cmd.InstancesToUpdate();

        VulkanBuffer* instance_buffer     = reinterpret_cast<VulkanBuffer*>(_cmd.InstanceBufferHandle());
        VulkanBuffer* scratch_buffer      = reinterpret_cast<VulkanBuffer*>(_cmd.ScratchBufferHandle());
        VulkanAccelerationStructure* tlas = reinterpret_cast<VulkanAccelerationStructure*>(_cmd.TlasHandle());

        if (to_update.size() == 0 && !_cmd.ForceUpdate()) {
            return;
        }
        // VulkanRaytracingScene* scene   = reinterpret_cast<VulkanRaytracingScene*>(_cmd.Handle());

        // Calculate 16-byte aligned offset for TLAS instance data (Vulkan spec requirement)
        constexpr uint64 kInstanceDataAlignment = 256; // 256-byte for AMD GPU compatibility
        uint64           raw_device_address     = instance_buffer->DeviceAddress();
        uint64           aligned_device_address = Moer::AlignUp(raw_device_address, kInstanceDataAlignment);
        uint64           alignment_offset       = aligned_device_address - raw_device_address;

        if (to_update.size() != 0) {
            BufferView staging = command_context.GetTemporaryBufferAllocator().AllocateShaderBuffer(
                to_update.size() * sizeof(VkAccelerationStructureInstanceKHR)
            );
            BufferView indices = command_context.GetTemporaryBufferAllocator().AllocateShaderBuffer(
                to_update.size() * sizeof(uint32) * 2
            );

            Array<std::pair<uint, uint>> to_update_indices(to_update.size());

            for (size_t i = 0; i < to_update.size(); ++i) {
                const auto& id       = to_update[i];
                to_update_indices[i] = {i, id};
            }

            m_device->CopyData(
                staging,
                _cmd.InstanceData().data(),
                to_update.size() * sizeof(VkAccelerationStructureInstanceKHR)
            );
            m_device->CopyData(indices, to_update_indices.data(), to_update.size() * sizeof(uint32) * 2);

            auto& shuffle_sd = m_device->GetInternalShaders().sd_component_shuffle;
            cmd_list.BindPipeline(shuffle_sd.handle);

            ComponentShuffleShader::Arg arg;
            arg.component_cnt = to_update.size();
            arg.stride        = sizeof(VkAccelerationStructureInstanceKHR) >> 2;

            // Create buffer view with alignment offset so data is written at the aligned address
            BufferView aligned_instance_buffer_view(
                instance_buffer,
                alignment_offset,
                (instance_buffer->GetByteSize() - alignment_offset) /
                    sizeof(VkAccelerationStructureInstanceKHR),
                sizeof(VkAccelerationStructureInstanceKHR),
                EPixelFormat::PF_UNDEFINED
            );

            cmd_list.BindShaderArguments(
                shuffle_sd.handle, shuffle_sd.SetArgs(arg, indices, staging, aligned_instance_buffer_view)
            );

            cmd_list.Dispatch((to_update.size() + 63) / 64, 1, 1);

            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT;
            barrier.dstAccessMask       = VK_ACCESS_2_SHADER_READ_BIT;
            barrier.srcStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT_KHR;
            barrier.dstStageMask        = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            barrier.buffer              = instance_buffer->GetHandle();
            barrier.offset              = 0;
            barrier.size                = VK_WHOLE_SIZE;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers    = &barrier;

            vkCmdPipelineBarrier2(cmd_list.GetHandle(), &dependency);
        }
        VkAccelerationStructureBuildGeometryInfoKHR build_info{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR
        };
        build_info.dstAccelerationStructure = tlas->handle;
        build_info.type                     = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        build_info.geometryCount            = 1;
        build_info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; //now force build each frame

        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.flags        = 0;
        geometry.geometry.instances.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        geometry.geometry.instances.arrayOfPointers = VK_FALSE;
        // Use 16-byte aligned device address for TLAS instance data (Vulkan spec requirement)
        // Reuse the aligned address calculated earlier in this function
        geometry.geometry.instances.data.deviceAddress = aligned_device_address;

        VkAccelerationStructureBuildRangeInfoKHR build_range[] = {{}};
        build_range[0].primitiveCount                          = _cmd.InstanceCount();
        VkAccelerationStructureBuildRangeInfoKHR* range        = build_range;
        build_info.pGeometries                                 = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR size_infos{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
        };

        uint instance_count = _cmd.InstanceCount();
        vkGetAccelerationStructureBuildSizesKHR(
            m_device->GetDevice(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &build_info,
            &instance_count,
            &size_infos
        );

        assert(size_infos.accelerationStructureSize > 0 && "Invalid acceleration structure size!");
        assert(
            size_infos.buildScratchSize <= scratch_buffer->GetByteSize() && "Invalid scratch buffer size!"
        );

        const uint64 scratch_alignment =
            m_device->GetOptionalProperties()
                .acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
        build_info.scratchData.deviceAddress =
            Moer::AlignUp(scratch_buffer->DeviceAddress(), scratch_alignment);

        cmd_list.BeginLabel(std::format("UpdateTLAS with {} instances", _cmd.InstanceCount()), {});
        vkCmdBuildAccelerationStructuresKHR(cmd_list.GetHandle(), 1, &build_info, &range);
        cmd_list.EndLabel();
    }

    void Visit(const CustomCmd& _cmd) {
        static float4 custom_color = {0.0f, 1.0f, 1.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, custom_color);
        switch (_cmd.CustomId()) {
            case CustomCmd::CustomCmdId::CUSTOM_RASTER:
                assert(false && "Custom raster draw scene not implemented");
                break;
            case CustomCmd::CustomCmdId::CUSTOM_DISPATCH:
                Visit(static_cast<const VkCustomDispatchCmd&>(_cmd));
                break;
            default:
                assert(false && "Custom Command Not Supported for VkCmdVisitor");
        }
        cmd_list.EndLabel();
    }

    void Visit(const VkCustomDispatchCmd& _cmd) {
        const VkCustomDispatchCmd::VkDispatchContext context = {
            m_device->GetInstance(), m_device->GetGpu(), m_device->GetDevice(), cmd_list.GetHandle()
        };
        _cmd.Execute(context);
    }

    // void Visit(const UpdateDrawStateCmd& _cmd) {
    // }

    // void Visit(const SetParamsCmd& _cmd) {
    //     auto&& args      = std::move(_cmd.StealArgs());
    //     auto&  pso       = _cmd.Pso();
    //     auto   set_param = [&](uint _idx, const TArg& _arg) {
    //         if constexpr (std::is_same_v<TArg, TextureView>) {
    //             pso.SetTexture(_idx, std::get<TextureView>(_arg));
    //         } else if constexpr (std::is_same_v<TArg, BufferView>) {
    //             pso.SetBuffer(_idx, std::get<BufferView>(_arg));
    //         }
    //     };
    //     std::visit([&](auto&& _args) {
    //         using TArgs = std::decay_t<decltype(_args)>;
    //         if constexpr (std::is_same_v<TArgs, ArrayArguments>) {
    //             for (size_t i = 0; i < _args.Size(); ++i) {
    //                 set_param(i, _args[i]);
    //             }
    //             cmd_list.UploadDescriptors(pso.handle);

    //             if (_args.constants.size() > 0) {
    //                 cmd_list.UploadPushConstants(pso.handle, std::span<const uint>(_args.constants.data(), _args.constants.size()));
    //             }
    //         } else if constexpr (std::is_same_v<TArgs, Arguments>) {
    //             for (size_t i = 0; i < _args.Size(); ++i) {
    //                 set_param(i, _args[i]);
    //             }
    //             cmd_list.UploadDescriptors(pso.handle);
    //         }
    //     },
    //                args);

    //     // cmd submit params
    // }

    // void Visit(const SetConstantCmd& _cmd) {
    //     auto& pso  = _cmd.Pso();
    //     auto  data = std::move(_cmd.StealData());
    //     cmd_list.UploadPushConstants(pso.handle, std::span<uint>(data.data(), data.size()));
    //     // cmd submit consants
    // }
};

#pragma endregion

// Validate before native recording or consuming command payloads. Build/update
// commands retain their backend-private protocols; application dependencies
// must already be compiled into the submission.
bool SupportsVulkanCompiledCommands(const CmdSubmit& submit, EQueueType queue) {
    if (!submit.cmds.empty() && submit.resource_state_ownership != ERHIResourceStateOwnership::Compiled) {
        return false;
    }
    for (const auto& command : submit.cmds) {
        if (!command) {
            return false;
        }
        switch (command->Type()) {
            case Command::EType::UploadBuffer:
            case Command::EType::UploadTexture:
            case Command::EType::BufferToBuffer:
            case Command::EType::BufferToTexture:
            case Command::EType::TextureToBuffer:
            case Command::EType::TextureToTexture:
            case Command::EType::CopyBackBuffer:
            case Command::EType::CopyBackTexture:
            case Command::EType::ShaderDispatch:
            case Command::EType::SetDrawState:
            case Command::EType::MultiDraw:
            case Command::EType::ClearResource:
            case Command::EType::Query:
            case Command::EType::Scope:
                break;
            case Command::EType::BuildAccel:
            case Command::EType::BuildTLAS:
            case Command::EType::UpdateBindlessArray:
                // The update command and its private storage currently belong
                // to Graphics. A copy queue cannot execute the shuffle shader.
                if (queue != EQueueType::Graphics) {
                    return false;
                }
                break;
            case Command::EType::Custom: {
                if (static_cast<const CustomCmd&>(*command).CustomId() != CustomCmd::CustomCmdId::CUSTOM_DISPATCH) {
                    return false;
                }
                const auto& custom = static_cast<const VkCustomDispatchCmd&>(*command);
                if (custom.GetQueueType() != queue || !custom.PreservesCompiledResourceStates()) {
                    return false;
                }
                break;
            }
            case Command::EType::Barrier:
                if (!static_cast<const BarrierCmd&>(*command).HasExplicitBarriers()) {
                    return false;
                }
                break;
            default:
                return false;
        }
    }
    return true;
}

void RecordCompiledBindlessUpdate(
    VulkanDevice&                 device,
    VulkanCommandList&            native,
    VkCmdVisitor&                 visitor,
    const UpdateBindlessArrayCmd& command
) {
    auto*                      table = static_cast<VulkanBindlessArray*>(command.Handle());
    VulkanBarrierBatch before, after;
    auto protect = [&](VulkanBuffer* buffer, uint64 offset, uint64 size, VkAccessFlags2 read_access) {
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask       = read_access | VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask        = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer              = buffer->GetHandle();
        barrier.offset              = offset;
        barrier.size                = size;
        before.buffer_barriers.push_back(barrier);
        barrier.srcStageMask  = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = read_access;
        after.buffer_barriers.push_back(barrier);
    };
    // These are table-storage dependencies, not dependencies of the resources
    // named by the table. RDG owns target access declarations and publication
    // order (a Write/Read token suffices on Graphics). Protect only this table's
    // ranges, including when several tables share one resource heap.
    if (command.HasUpdates()) {
        auto* indices = table->GetIndexBuffer();
        protect(indices, 0, indices->GetByteSize(), VK_ACCESS_2_SHADER_READ_BIT);
    }
    const auto& heap = device.GetGlobalDescriptorHeap();
    if (command.HasBufferUpdates()) {
        protect(
            table->GetBufferDescriptorBuffer(),
            table->buffer_descriptors_byte_offset,
            table->handles.size() * heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            table->GetDescriptorReadAccess()
        );
    }
    if (command.HasTextureUpdates()) {
        protect(
            table->GetTextureSamplerDescriptorBuffer(),
            table->texture_offset_in_buffer,
            table->handles.size() * heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            table->GetDescriptorReadAccess()
        );
    }
    before.Record(native);
    // Keep the existing staging, stale-batch rejection, completion token and
    // slot retirement protocol. Capture barriers before the visitor moves data.
    visitor.VisitCmd(&command);
    after.Record(native);
}

void RecordCompiledTlasUpdate(
    VulkanCommandList& native, VkCmdVisitor& visitor, const UpdateRaytracingSceneCmd& command
) {
    if (command.InstancesToUpdate().empty() && !command.ForceUpdate()) { return; }
    VulkanBarrierBatch before;
    const auto protect = [&](VulkanBuffer* buffer, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = stage;
        barrier.dstAccessMask = access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer->GetHandle();
        barrier.size = VK_WHOLE_SIZE;
        before.buffer_barriers.push_back(barrier);
    };
    protect(ResourceCast(command.InstanceBuffer().Get()),
            command.InstancesToUpdate().empty() ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            command.InstancesToUpdate().empty() ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_SHADER_WRITE_BIT);
    protect(ResourceCast(command.ScratchBuffer().Get()), VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR);
    before.Record(native);
    // The visitor records the private upload -> build dependency. Public BLAS
    // input and TLAS output dependencies have already been emitted by RDG.
    visitor.VisitCmd(&command);
}

// Backend-private allocation and explicit barrier preparation. Each staging
// slice is exclusively owned until completion; no application state is inferred.
struct VulkanCommandPreparation {
    VulkanDevice& device;
    VulkanTemporaryBufferAllocator& temporary_buffer_allocator;
    EQueueType current_queue;
    VulkanBarrierRecorder barriers;

    void UploadDependency(const BufferView& view) {
        barriers.Append(ResourceCast(view.GetBuffer()), view.GetByteOffset(), view.GetByteSize(),
            EBarrierQueueTransferPhase::None, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }

    void Visit(const UploadBufferCmd* _cmd) {
        auto data_span  = _cmd->Data();
        auto tmp_buffer = temporary_buffer_allocator.AllocateUploadBuffer(_cmd->ByteSize(), 16);
        device.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
        _cmd->staging_buffer = tmp_buffer;

        UploadDependency(_cmd->staging_buffer);
    }

    void Visit(const UploadTextureCmd* _cmd) {
        const auto data_span  = _cmd->Data();
        auto*      vk_texture = reinterpret_cast<VulkanTexture*>(_cmd->Handle());
        // The staging buffer can be larger than this upload's slice. Validate
        // the supplied data before allocating or copying into that slice.
        ValidateTextureTransferDataSize(*vk_texture, _cmd->Size(), data_span.size_bytes());
        auto tmp_buffer = temporary_buffer_allocator.AllocateUploadBuffer(data_span.size_bytes(), 16);
        device.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
        _cmd->staging_buffer = tmp_buffer;

        UploadDependency(_cmd->staging_buffer);
    }

    void Visit(const CopyBackBufferCmd* _cmd) {
        auto tmp_buffer      = temporary_buffer_allocator.AllocateReadbackBuffer(_cmd->ByteSize(), 16);
        _cmd->staging_buffer = tmp_buffer;

    }

    void Visit(const CopyBackTextureCmd* _cmd) {
        auto* vk_texture = reinterpret_cast<VulkanTexture*>(_cmd->Handle());
        ValidateTextureTransferDataSize(*vk_texture, _cmd->Size(), _cmd->ByteSize());
        auto tmp_buffer      = temporary_buffer_allocator.AllocateReadbackBuffer(_cmd->ByteSize(), 16);
        _cmd->staging_buffer = tmp_buffer;

    }

    void Visit(const BuildAccelerationStructuresCmd* _cmd) {
        const Array<AccelerationStructureBuildParam>& params  = _cmd->Params();
        BufferView&                                   scratch = _cmd->Scratch();
        if (!scratch.GetBuffer()) {
            uint64 scratch_size      = 0;
            uint64 scratch_alignment = 256u;

            for (const AccelerationStructureBuildParam& param : params) {
                auto* vk_geo = ResourceCast(param.geometry.Get());
                scratch_size = Moer::AlignUp(scratch_size, scratch_alignment);
                scratch_size += param.mode == ERaytracingBuildMode::BUILD ?
                                    vk_geo->build_sizes_info.buildScratchSize :
                                    vk_geo->build_sizes_info.updateScratchSize;
            }
            scratch_size += scratch_alignment;

            scratch = temporary_buffer_allocator.AllocateScratch(scratch_size);
        }
    }

    void Visit(const BarrierCmd* _cmd) {
        if (!_cmd->HasExplicitBarriers()) {
            throw std::invalid_argument("Vulkan requires fully specified barriers");
        }

        if (_cmd->GetSrcQueue() != current_queue ||
            _cmd->GetDstQueue() != current_queue) {
            LOG_CRITICAL(
                "Explicit BarrierCmd queue affinity mismatch: command={} "
                "current={}",
                static_cast<uint>(_cmd->GetSrcQueue()),
                static_cast<uint>(current_queue)
            );
            throw std::logic_error("explicit barrier recorded on the wrong queue");
        }

        const auto resolve_transfer =
            [&](const BarrierQueueTransfer& transfer) {
                std::pair<uint32_t, uint32_t> families{
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_QUEUE_FAMILY_IGNORED,
                };
                if (transfer.phase == EBarrierQueueTransferPhase::None) {
                    if (transfer.external) {
                        throw std::invalid_argument("Vulkan external ownership barrier requires a phase");
                    }
                    return families;
                }
                if (transfer.phase != EBarrierQueueTransferPhase::Release &&
                    transfer.phase != EBarrierQueueTransferPhase::Acquire) {
                    throw std::invalid_argument(
                        "Vulkan ownership barrier carries an unknown phase"
                    );
                }
                if (transfer.external) {
                    const bool release  = transfer.phase == EBarrierQueueTransferPhase::Release;
                    const auto managed  = release ? transfer.src_queue : transfer.dst_queue;
                    const auto external = release ? transfer.dst_queue : transfer.src_queue;
                    if (managed != current_queue || external != EQueueType::Ignore) {
                        throw std::invalid_argument("Vulkan external ownership barrier endpoint mismatch"
                        );
                    }
                    const auto family = device.GetQueueFamilyIndex(managed);
                    if (family == VK_QUEUE_FAMILY_IGNORED) {
                        throw std::invalid_argument(
                            "Vulkan external ownership barrier has no native queue"
                        );
                    }
                    return release ? std::pair{family, uint32_t(VK_QUEUE_FAMILY_EXTERNAL)} :
                                     std::pair{uint32_t(VK_QUEUE_FAMILY_EXTERNAL), family};
                }
                if ((transfer.src_queue != EQueueType::Graphics &&
                     transfer.src_queue != EQueueType::Compute &&
                     transfer.src_queue != EQueueType::Copy) ||
                    (transfer.dst_queue != EQueueType::Graphics &&
                     transfer.dst_queue != EQueueType::Compute &&
                     transfer.dst_queue != EQueueType::Copy) ||
                    transfer.src_queue == transfer.dst_queue) {
                    throw std::invalid_argument(
                        "Vulkan ownership barrier requires distinct managed "
                        "Graphics/Compute/Copy endpoints"
                    );
                }
                const EQueueType expected_queue =
                    transfer.phase == EBarrierQueueTransferPhase::Release ?
                        transfer.src_queue :
                        transfer.dst_queue;
                if (current_queue != expected_queue) {
                    throw std::logic_error(
                        "Vulkan ownership barrier recorded on the wrong endpoint"
                    );
                }
                families.first =
                    device.GetQueueFamilyIndex(transfer.src_queue);
                families.second =
                    device.GetQueueFamilyIndex(transfer.dst_queue);
                if (families.first == VK_QUEUE_FAMILY_IGNORED ||
                    families.second == VK_QUEUE_FAMILY_IGNORED ||
                    families.first == families.second) {
                    throw std::invalid_argument(
                        "Vulkan ownership barrier requires two distinct queue families"
                    );
                }
                return families;
            };

        for (const ExplicitBufferBarrier& barrier : _cmd->ExplicitBuffers()) {
            if (barrier.src_state.texture_layout !=
                    ETextureLayout::TEXTURE_LAYOUT_UNDEFINED ||
                barrier.dst_state.texture_layout !=
                    ETextureLayout::TEXTURE_LAYOUT_UNDEFINED) {
                throw std::invalid_argument(
                    "explicit buffer barrier cannot carry a texture layout"
                );
            }
            const auto [src_family, dst_family] =
                resolve_transfer(barrier.queue_transfer);
            barriers.Append(
                reinterpret_cast<VulkanBuffer*>(barrier.handle),
                barrier.offset,
                barrier.byte_size,
                barrier.queue_transfer.phase,
                src_family,
                dst_family,
                VulkanEnumTranslator::METoVkPipelineStageFlags2(
                    barrier.src_state.stage
                ),
                VulkanEnumTranslator::METoVkAccessFlags2(
                    barrier.src_state.access
                ),
                VulkanEnumTranslator::METoVkPipelineStageFlags2(
                    barrier.dst_state.stage
                ),
                VulkanEnumTranslator::METoVkAccessFlags2(
                    barrier.dst_state.access
                )
            );
        }
        for (const ExplicitTextureBarrier& barrier :
             _cmd->ExplicitTextures()) {
            const VkImageLayout src_layout =
                VulkanEnumTranslator::METoVKImageLayout(
                    barrier.src_state.texture_layout
                );
            const VkImageLayout dst_layout =
                VulkanEnumTranslator::METoVKImageLayout(
                    barrier.dst_state.texture_layout
                );
            if (src_layout == VK_IMAGE_LAYOUT_MAX_ENUM ||
                dst_layout == VK_IMAGE_LAYOUT_MAX_ENUM) {
                throw std::invalid_argument(
                    "explicit texture barrier carries an unsupported layout"
                );
            }
            const auto [src_family, dst_family] =
                resolve_transfer(barrier.queue_transfer);
            barriers.Append(
                reinterpret_cast<VulkanTexture*>(barrier.handle),
                VulkanEnumTranslator::METoVKImageAspectFlags(
                    barrier.texture_aspects
                ),
                barrier.mip_level,
                barrier.mip_count,
                barrier.array_layer,
                barrier.array_count,
                barrier.queue_transfer.phase,
                src_family,
                dst_family,
                VulkanEnumTranslator::METoVkPipelineStageFlags2(
                    barrier.src_state.stage
                ),
                VulkanEnumTranslator::METoVkAccessFlags2(
                    barrier.src_state.access
                ),
                src_layout,
                VulkanEnumTranslator::METoVkPipelineStageFlags2(
                    barrier.dst_state.stage
                ),
                VulkanEnumTranslator::METoVkAccessFlags2(
                    barrier.dst_state.access
                ),
                dst_layout
            );
        }
    }
};

void RecordCompiledCommands(
    VulkanDevice&         device,
    VulkanCommandContext& context,
    CmdSubmit&            submit,
    VkCmdVisitor&         visitor,
    EQueueType            queue,
    bool                  suppress_timestamp_queries = false
) {
    VulkanCommandPreparation preparation{device, context.GetTemporaryBufferAllocator(), queue};
    for (const auto& command : submit.cmds) {
        if (suppress_timestamp_queries && command->Type() == Command::EType::Query) {
            continue;
        }
        if (command->Type() == Command::EType::UpdateBindlessArray) {
            RecordCompiledBindlessUpdate(device, context.GetCommandList(), visitor,
                                        static_cast<const UpdateBindlessArrayCmd&>(*command));
            continue;
        }
        if (command->Type() == Command::EType::BuildAccel) {
            const auto* build = static_cast<const BuildAccelerationStructuresCmd*>(command.get());
            preparation.Visit(build);
            // Scratch is backend-private. Vertex/index inputs and BLAS output
            // are exclusively synchronized by the graph's declared resources.
            VulkanBarrierBatch before;
            VkBufferMemoryBarrier2     barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR |
                                    VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = ResourceCast(build->Scratch().GetBuffer())->GetHandle();
            barrier.offset = build->Scratch().byte_offset;
            barrier.size   = build->Scratch().GetByteSize();
            before.buffer_barriers.push_back(barrier);
            before.Record(context.GetCommandList());
            visitor.VisitCmd(command.get());
            continue;
        }
        if (command->Type() == Command::EType::BuildTLAS) {
            RecordCompiledTlasUpdate(context.GetCommandList(), visitor,
                                    static_cast<const UpdateRaytracingSceneCmd&>(*command));
            continue;
        }
        // Only private staging allocations and explicit barriers are prepared.
        // Shader arguments, attachments and bindless membership are not scanned.
        switch (command->Type()) {
            case Command::EType::UploadBuffer:
                preparation.Visit(static_cast<const UploadBufferCmd*>(command.get()));
                break;
            case Command::EType::UploadTexture:
                preparation.Visit(static_cast<const UploadTextureCmd*>(command.get()));
                break;
            case Command::EType::CopyBackBuffer:
                preparation.Visit(static_cast<const CopyBackBufferCmd*>(command.get()));
                break;
            case Command::EType::CopyBackTexture:
                preparation.Visit(static_cast<const CopyBackTextureCmd*>(command.get()));
                break;
            case Command::EType::Barrier:
                preparation.Visit(static_cast<const BarrierCmd*>(command.get()));
                break;
            default:
                break;
        }
        preparation.barriers.Flush(context.GetCommandList());
        visitor.VisitCmd(command.get());
        const BufferView readback = command->Type() == Command::EType::CopyBackBuffer ?
            static_cast<const CopyBackBufferCmd*>(command.get())->staging_buffer :
            command->Type() == Command::EType::CopyBackTexture ?
            static_cast<const CopyBackTextureCmd*>(command.get())->staging_buffer : BufferView{};
        if (readback.GetBuffer()) {
            preparation.barriers.Append(ResourceCast(readback.GetBuffer()), readback.GetByteOffset(), readback.GetByteSize(),
                EBarrierQueueTransferPhase::None, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            preparation.barriers.Flush(context.GetCommandList());
        }
    }
}

void RecordVulkanCopyCommands(
    VulkanDevice& device, VulkanCommandContext& context, CmdSubmit& submit,
    bool suppress_timestamp_queries
) {
    auto& native = context.GetCommandList();
    VkCmdVisitor visitor(device, context, native, submit.cached_args);
    VK_CHECK_RESULT(native.Begin());
    native.BeginLabel("Copy", {0.0f, 1.0f, 1.0f, 1.0f});
    RecordCompiledCommands(device, context, submit, visitor, EQueueType::Copy, suppress_timestamp_queries);
    native.EndLabel();
    VK_CHECK_RESULT(native.End());
}

#pragma region[ VkCommandQueue ]

void VkCommandQueue::Wait(WaitEvent _evt) {
    auto* fence = reinterpret_cast<VulkanTimeline*>(_evt.timeline_handle);
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(_evt, _evt.value, false);

        queue_cv.notify_one();
    }
}

bool WaitForSubmittedDependencies(
    VulkanDevice&           _device,
    const Array<WaitEvent>& _wait_events,
    EQueueType               _queue,
    const std::atomic_bool* _continue_waiting
) {
    if (!_wait_events.empty()) {
#if MOER_VULKAN_TEST_SUPPORT
        NotifyVulkanSubmissionDependencyWaitBegin(
            _queue, static_cast<uint32>(_wait_events.size())
        );
#endif
    }
    for (const WaitEvent& event : _wait_events) {
        auto* fence = reinterpret_cast<VulkanTimeline*>(event.timeline_handle);
        if (fence == nullptr ||
            !fence->WaitSubmitted(event.value, _continue_waiting)) {
            return false;
        }
    }
    return !_device.IsFaulted();
}

VkResult GetRejectedSubmitResult(VulkanDevice& _device) {
    return _device.IsFaulted() ? _device.GetFirstFaultResult() : VK_ERROR_UNKNOWN;
}

VulkanOperationResult MakeUnsubmittedOutcome(
    VkResult _result,
    bool     _recoverable
) {
    if (_result == VK_SUCCESS) {
        _result = VK_ERROR_UNKNOWN;
    }
    return {
        _recoverable ?
            EVulkanOperationStatus::Rejected :
            EVulkanOperationStatus::Faulted,
        _result
    };
}

void MarkSubmissionAccepted(
    VulkanTimeline* _queue_timeline, uint64 _timeline, const Array<SignalEvent>& _signal_events
) {
    _queue_timeline->MarkSubmitted(_timeline);
    for (const SignalEvent& event : _signal_events) {
        reinterpret_cast<VulkanTimeline*>(event.timeline_handle)->MarkSubmitted(event.value);
    }
}

[[nodiscard]] static bool IsPresentationSourceTexture(
    const Texture* _texture
) noexcept {
    return _texture != nullptr &&
           (_texture->GetUsage() &
            ETextureUsageFlags::PRESENTATION_SOURCE) ==
               ETextureUsageFlags::PRESENTATION_SOURCE;
}

[[nodiscard]] static bool IsAcceptedPresentationSourcePublication(
    const ExplicitTextureBarrier& _barrier,
    const VulkanTexture&          _texture
) noexcept {
    return _barrier.publish_external_state &&
           _barrier.queue_transfer.phase ==
               EBarrierQueueTransferPhase::None &&
           _barrier.dst_state.stage ==
               ERHIPipelineStageFlags::PS_TRANSFER &&
           _barrier.dst_state.access ==
               ERHIAccessFlags::TRANSFER_READ &&
           _barrier.dst_state.texture_layout ==
               ETextureLayout::TEXTURE_LAYOUT_COMMON &&
           _barrier.texture_aspects ==
               ETextureAspectFlags::COLOR &&
           _barrier.mip_level == 0 &&
           _barrier.mip_count == _texture.GetNumMips() &&
           _barrier.array_layer == 0 &&
           _barrier.array_count == _texture.GetNumArray();
}

template<typename TTextureVisitor, typename TBindlessVisitor>
static void VisitPresentationTextureArgument(
    const TArg&        _argument,
    TTextureVisitor&   _texture_visitor,
    TBindlessVisitor&  _bindless_visitor
) {
    std::visit(
        [&](const auto& _resource) {
            using TResource = std::decay_t<decltype(_resource)>;
            if constexpr (std::is_same_v<TResource, TextureView>) {
                _texture_visitor(_resource.GetTexture());
            } else if constexpr (
                std::is_same_v<TResource, TextureViewArray>
            ) {
                for (const TextureView& view : _resource) {
                    _texture_visitor(view.GetTexture());
                }
            } else if constexpr (
                std::is_same_v<TResource, BindlessArrayRef>
            ) {
                if (_resource) {
                    _bindless_visitor(_resource);
                }
            }
        },
        _argument
    );
}

template<typename TTextureVisitor, typename TBindlessVisitor>
static void VisitPresentationTextureArguments(
    const ArrayArguments& _arguments,
    TTextureVisitor&      _texture_visitor,
    TBindlessVisitor&     _bindless_visitor
) {
    for (const TArg& argument : _arguments.args) {
        VisitPresentationTextureArgument(
            argument,
            _texture_visitor,
            _bindless_visitor
        );
    }
}

template<typename TTextureVisitor, typename TBindlessVisitor>
static void VisitPresentationTextureArguments(
    const ArrayArguments& _arguments,
    const TCachedArgArray&,
    TTextureVisitor&      _texture_visitor,
    TBindlessVisitor&     _bindless_visitor
) {
    VisitPresentationTextureArguments(
        _arguments, _texture_visitor, _bindless_visitor
    );
}

template<typename TTextureVisitor, typename TBindlessVisitor>
static void VisitPresentationTextureArguments(
    const TShaderArgArray& _arguments,
    const TCachedArgArray& _cached_arguments,
    TTextureVisitor&       _texture_visitor,
    TBindlessVisitor&      _bindless_visitor
) {
    if (std::holds_alternative<ArrayArguments>(_arguments)) {
        VisitPresentationTextureArguments(
            std::get<ArrayArguments>(_arguments),
            _texture_visitor,
            _bindless_visitor
        );
        return;
    }
    if (std::holds_alternative<ArrayArgReference>(_arguments)) {
        const uint index =
            std::get<ArrayArgReference>(_arguments).handle;
        if (index < _cached_arguments.size()) {
            VisitPresentationTextureArguments(
                _cached_arguments[index],
                _texture_visitor,
                _bindless_visitor
            );
        }
    }
}

// Freeze source command order before translation can
// change execution topology. Direct texture uses become concrete events;
// bindless uses remain symbolic until Submission can observe the last
// natively accepted descriptor membership.
VulkanPresentationSourceStateProgram
BuildPresentationSourceStateProgram(
    const CmdSubmit& _submit,
    EQueueType       _queue
) {
    VulkanPresentationSourceStateProgram program{};
    program.events.reserve(_submit.cmds.size());

    const auto record_texture =
        [&](Texture* _texture, bool _ready) {
            if (!IsPresentationSourceTexture(_texture)) {
                return;
            }
            program.events.emplace_back(
                VulkanPresentationSourceStateEvent{
                    .type =
                        EVulkanPresentationSourceStateEventType::
                            TextureState,
                    .texture = TextureRef(_texture),
                    .ready   = _ready,
                }
            );
        };
    const auto record_bindless =
        [&](const BindlessArrayRef& _array) {
            if (!_array) {
                return;
            }
            program.events.emplace_back(
                VulkanPresentationSourceStateEvent{
                    .type =
                        EVulkanPresentationSourceStateEventType::
                            BindlessAccess,
                    .bindless_array = _array,
                }
            );
            program.has_bindless_state = true;
        };
    const auto record_view =
        [&](const TextureView& _view) {
            record_texture(_view.GetTexture(), false);
        };
    const auto record_shader_arguments =
        [&](const auto& _arguments,
            UnorderedSet<BindlessArray*>& _seen_bindless) {
            auto record_unique_bindless =
                [&](const BindlessArrayRef& _array) {
                    if (_array &&
                        _seen_bindless.emplace(
                            _array.Get()
                        ).second) {
                        record_bindless(_array);
                    }
                };
            auto record_direct_texture =
                [&](Texture* _texture) {
                    record_texture(_texture, false);
                };
            VisitPresentationTextureArguments(
                _arguments,
                _submit.cached_args,
                record_direct_texture,
                record_unique_bindless
            );
        };
    const auto record_render_pass =
        [&](const RenderPassInfo& _render_pass) {
            if (_render_pass.depth_attachment.Valid()) {
                record_texture(
                    _render_pass.depth_attachment.target, false
                );
            }
            for (const ColorAttachment& attachment :
                 _render_pass.color_attachments) {
                record_texture(attachment.target, false);
            }
        };
    const auto record_barrier =
        [&](const BarrierCmd& _command, uint64 _handle) {
            auto* texture =
                reinterpret_cast<VulkanTexture*>(_handle);
            if (!IsPresentationSourceTexture(texture)) {
                return;
            }

            size_t reference_count = 0;
            const ExplicitTextureBarrier*
                explicit_publication = nullptr;
            for (const TextureBarrier& barrier :
                 _command.ReadTextures()) {
                if (barrier.handle == _handle) {
                    ++reference_count;
                }
            }
            for (const TextureBarrier& barrier :
                 _command.WriteTextures()) {
                if (barrier.handle == _handle) {
                    ++reference_count;
                }
            }
            for (const ExplicitTextureBarrier& barrier :
                 _command.ExplicitTextures()) {
                if (barrier.handle == _handle) {
                    ++reference_count;
                    explicit_publication = &barrier;
                }
            }

            bool ready = false;
            if (_queue == EQueueType::Graphics &&
                reference_count == 1) {
                ready = explicit_publication != nullptr &&
                        IsAcceptedPresentationSourcePublication(*explicit_publication, *texture);
            }
            record_texture(texture, ready);
        };

    for (const UniquePtr<Command>& command_owner :
         _submit.cmds) {
        const Command* command = command_owner.get();
        if (command == nullptr) {
            continue;
        }
        switch (command->Type()) {
            case Command::EType::CopyBackTexture: {
                const auto& copy =
                    *static_cast<const CopyBackTextureCmd*>(command);
                record_texture(
                    reinterpret_cast<Texture*>(copy.Handle()), false
                );
                break;
            }
            case Command::EType::BufferToTexture: {
                const auto& copy =
                    *static_cast<const CopyBufferToTextureCmd*>(command);
                record_texture(
                    reinterpret_cast<Texture*>(copy.DstHandle()), false
                );
                break;
            }
            case Command::EType::TextureToBuffer: {
                const auto& copy =
                    *static_cast<const CopyTextureToBufferCmd*>(command);
                record_texture(
                    reinterpret_cast<Texture*>(copy.SrcHandle()), false
                );
                break;
            }
            case Command::EType::UploadTexture: {
                const auto& upload =
                    *static_cast<const UploadTextureCmd*>(command);
                record_texture(
                    reinterpret_cast<Texture*>(upload.Handle()), false
                );
                break;
            }
            case Command::EType::TextureToTexture: {
                const auto& copy =
                    *static_cast<const CopyTextureCmd*>(command);
                record_texture(
                    reinterpret_cast<Texture*>(copy.SrcHandle()), false
                );
                record_texture(
                    reinterpret_cast<Texture*>(copy.DstHandle()), false
                );
                break;
            }
            case Command::EType::ShaderDispatch: {
                const auto& dispatch =
                    *static_cast<const DispatchCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                record_shader_arguments(
                    dispatch.Args(_submit.cached_args),
                    seen_bindless
                );
                break;
            }
            case Command::EType::TraceRay: {
                const auto& trace =
                    *static_cast<const TraceRayCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                record_shader_arguments(
                    trace.Args(), seen_bindless
                );
                break;
            }
            case Command::EType::Barrier: {
                const auto& barrier_command =
                    *static_cast<const BarrierCmd*>(command);
                for (const TextureBarrier& barrier :
                     barrier_command.ReadTextures()) {
                    record_barrier(
                        barrier_command, barrier.handle
                    );
                }
                for (const TextureBarrier& barrier :
                     barrier_command.WriteTextures()) {
                    record_barrier(
                        barrier_command, barrier.handle
                    );
                }
                for (const ExplicitTextureBarrier& barrier :
                     barrier_command.ExplicitTextures()) {
                    record_barrier(
                        barrier_command, barrier.handle
                    );
                }
                break;
            }
            case Command::EType::QueueTransfer: {
                const auto& transfer =
                    *static_cast<const QueueTransferCmd*>(command);
                if (transfer.IsImport()) {
                    for (const auto& [view, state] :
                         transfer.ImportTextures()) {
                        (void)state;
                        record_view(view);
                    }
                } else {
                    for (const auto& [view, state] :
                         transfer.ExportTextures()) {
                        (void)state;
                        record_view(view);
                    }
                }
                break;
            }
            case Command::EType::SetDrawState: {
                const auto& draw =
                    *static_cast<const SetDrawStateCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                record_shader_arguments(
                    draw.Args(), seen_bindless
                );
                record_render_pass(draw.RenderPassInfo());
                break;
            }
            case Command::EType::MultiDraw: {
                const auto& draw =
                    *static_cast<const MultiDrawCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                for (const DrawBatchElement& element :
                     draw.draw_batch.draw_cmds) {
                    record_shader_arguments(
                        element.args, seen_bindless
                    );
                }
                record_render_pass(draw.RenderPassInfo());
                break;
            }
            case Command::EType::ClearResource: {
                const auto& clear =
                    *static_cast<const ClearResourceCmd*>(command);
                if (clear.IsTexture()) {
                    record_view(clear.Texture());
                }
                break;
            }
            case Command::EType::Custom: {
                const auto& custom =
                    *static_cast<const CustomCmd*>(command);
                if (custom.CustomId() ==
                    CustomCmd::CustomCmdId::CUSTOM_DISPATCH) {
                    UnorderedSet<BindlessArray*> seen_bindless{};
                    auto record_unique_bindless =
                        [&](const BindlessArrayRef& _array) {
                            if (_array &&
                                seen_bindless.emplace(
                                    _array.Get()
                                ).second) {
                                record_bindless(_array);
                            }
                        };
                    auto record_custom_texture =
                        [&](Texture* _texture) {
                            record_texture(_texture, false);
                        };
                    static_cast<const CustomDispatchCmd&>(custom).IterateArgs(
                        [&](const TArg& argument, ERHIAccessFlags) {
                            VisitPresentationTextureArgument(
                                argument,
                                record_custom_texture,
                                record_unique_bindless
                            );
                        }
                    );
                } else {
                    // Vulkan cannot currently execute CUSTOM_RASTER, and an
                    // unknown opaque command has no typed resource
                    // declaration from which to prove that it leaves an
                    // accepted presentation source untouched. Fail closed
                    // before native recording instead of preserving stale
                    // readiness.
                    throw std::logic_error(
                        "unsupported opaque custom command in "
                        "presentation-state collection"
                    );
                }
                break;
            }
            case Command::EType::UpdateBindlessArray: {
                const auto& update_command =
                    *static_cast<const UpdateBindlessArrayCmd*>(
                        command
                    );
                BindlessArrayRef bindless_array(
                    update_command.Handle()
                );
                if (!bindless_array) {
                    throw std::logic_error(
                        "bindless update has no array"
                    );
                }
                for (const BindlessArray::UpdateCmd& update :
                     update_command.UpdateCommands()) {
                    std::visit(
                        [&](const auto& _update) {
                            using TUpdate =
                                std::decay_t<decltype(_update)>;
                            if constexpr (
                                std::is_same_v<
                                    TUpdate,
                                    BindlessArray::
                                        TextureUpdateInfo>) {
                                if (!IsPresentationSourceTexture(
                                        _update.texture.Get()
                                    )) {
                                    return;
                                }
                                program.events.emplace_back(
                                    VulkanPresentationSourceStateEvent{
                                        .type =
                                            EVulkanPresentationSourceStateEventType::
                                                BindlessTextureMembership,
                                        .texture = _update.texture,
                                        .bindless_array =
                                            bindless_array,
                                        .bindless_slot =
                                            _update.array_idx,
                                        .membership_delta =
                                            _update.free ? -1 : 1,
                                    }
                                );
                                program.has_bindless_state = true;
                            }
                        },
                        update
                    );
                }
                break;
            }
            case Command::EType::UploadBuffer:
            case Command::EType::CopyBackBuffer:
            case Command::EType::BufferToBuffer:
            case Command::EType::BuildAccel:
            case Command::EType::BuildTLAS:
            case Command::EType::Query:
            case Command::EType::Scope:
                break;
            case Command::EType::SetGeometryPassDrawState:
            case Command::EType::Count:
                throw std::logic_error(
                    "unsupported command in presentation-state collection"
                );
        }
    }
    return program;
}

static std::mutex& PresentationSourceBindlessSubmissionMutex() {
    static std::mutex mutex;
    return mutex;
}

VulkanPresentationSourceStateTransaction
EvaluatePresentationSourceStateProgram(
    const VulkanPresentationSourceStateProgram& _program
) {
    using Membership =
        VulkanBindlessArray::PresentationSourceMembership;
    struct WorkingMembership {
        BindlessArrayRef            bindless_array{};
        std::shared_ptr<Membership> membership{};
    };

    VulkanPresentationSourceStateTransaction transaction{};
    transaction.delta.reserve(_program.events.size());
    if (_program.has_bindless_state) {
        // The runtime has one Submission owner, but standalone queue use can
        // still enter this path directly. Keep snapshot -> vkQueueSubmit ->
        // accepted publication one global transaction in both modes.
        transaction.submission_lock =
            std::unique_lock<std::mutex>(
                PresentationSourceBindlessSubmissionMutex()
            );
    }

    Array<WorkingMembership> working_memberships{};
    UnorderedMap<VulkanBindlessArray*, size_t>
        working_membership_indices{};

    const auto& get_working_membership =
        [&](const BindlessArrayRef& _array) -> WorkingMembership& {
            if (!_array) {
                throw std::logic_error(
                    "presentation-state program references a null "
                    "bindless array"
                );
            }
            auto* bindless =
                static_cast<VulkanBindlessArray*>(_array.Get());
            const auto existing =
                working_membership_indices.find(bindless);
            if (existing != working_membership_indices.end()) {
                return working_memberships[existing->second];
            }

            const auto accepted =
                bindless
                    ->SnapshotAcceptedPresentationSourceMembership();
            if (!accepted) {
                throw std::logic_error(
                    "bindless array has no accepted presentation "
                    "membership state"
                );
            }
            const size_t index = working_memberships.size();
            working_memberships.emplace_back(
                WorkingMembership{
                    .bindless_array = _array,
                    .membership =
                        std::make_shared<Membership>(*accepted),
                }
            );
            working_membership_indices.emplace(bindless, index);
            return working_memberships.back();
        };

    const auto record_texture =
        [&](const TextureRef& _texture, bool _ready) {
            if (!_texture ||
                !IsPresentationSourceTexture(_texture.Get())) {
                return;
            }
            auto existing = std::find_if(
                transaction.delta.begin(),
                transaction.delta.end(),
                [&](const VulkanPresentationSourceStateDelta& _entry) {
                    return _entry.texture.Get() == _texture.Get();
                }
            );
            if (existing != transaction.delta.end()) {
                existing->ready = _ready;
                return;
            }
            transaction.delta.emplace_back(
                VulkanPresentationSourceStateDelta{
                    .texture = _texture,
                    .ready   = _ready,
                }
            );
        };

    for (const VulkanPresentationSourceStateEvent& event :
         _program.events) {
        switch (event.type) {
            case EVulkanPresentationSourceStateEventType::
                TextureState:
                record_texture(event.texture, event.ready);
                break;
            case EVulkanPresentationSourceStateEventType::
                BindlessAccess: {
                const WorkingMembership& working =
                    get_working_membership(event.bindless_array);
                for (const auto& [texture, record] :
                     working.membership->textures) {
                    (void)texture;
                    if (record.count != 0) {
                        record_texture(record.texture, false);
                    }
                }
                break;
            }
            case EVulkanPresentationSourceStateEventType::
                BindlessTextureMembership: {
                if (!event.texture ||
                    !IsPresentationSourceTexture(
                        event.texture.Get()
                    ) ||
                    event.bindless_slot == 0 ||
                    (event.membership_delta != 1 &&
                     event.membership_delta != -1)) {
                    throw std::logic_error(
                        "invalid bindless presentation membership "
                        "mutation"
                    );
                }
                WorkingMembership& working =
                    get_working_membership(event.bindless_array);
                auto& membership = *working.membership;
                const auto decrement_texture =
                    [&](const TextureRef& _texture) {
                        const auto texture =
                            membership.textures.find(
                                _texture.Get()
                            );
                        if (texture ==
                                membership.textures.end() ||
                            texture->second.count == 0) {
                            throw std::logic_error(
                                "bindless presentation membership "
                                "slot/count mismatch"
                            );
                        }
                        if (--texture->second.count == 0) {
                            membership.textures.erase(texture);
                        }
                    };
                const auto increment_texture =
                    [&](const TextureRef& _texture) {
                        auto [texture, inserted] =
                            membership.textures.try_emplace(
                                _texture.Get(),
                                VulkanBindlessArray::
                                    PresentationSourceMembershipRecord{
                                        .texture = _texture,
                                        .count   = 0,
                                    }
                            );
                        (void)inserted;
                        if (texture->second.count ==
                            std::numeric_limits<uint32>::max()) {
                            throw std::logic_error(
                                "bindless presentation membership "
                                "count overflow"
                            );
                        }
                        ++texture->second.count;
                    };
                auto slot = membership.slots.find(
                    event.bindless_slot
                );
                if (event.membership_delta > 0) {
                    if (slot != membership.slots.end()) {
                        if (slot->second.Get() ==
                            event.texture.Get()) {
                            break;
                        }
                        decrement_texture(slot->second);
                        slot->second = event.texture;
                    } else {
                        membership.slots.emplace(
                            event.bindless_slot,
                            event.texture
                        );
                    }
                    increment_texture(event.texture);
                    break;
                }
                // Clearing a slot which was never natively published is an
                // accepted idempotent descriptor update. This can occur when
                // a pending add is canceled/rejected before its later free.
                if (slot == membership.slots.end()) {
                    break;
                }
                if (slot->second.Get() != event.texture.Get()) {
                    throw std::logic_error(
                        "bindless presentation free targets a "
                        "different accepted slot resource"
                    );
                }
                decrement_texture(slot->second);
                membership.slots.erase(slot);
                break;
            }
        }
    }

    transaction.membership_commits.reserve(
        working_memberships.size()
    );
    for (WorkingMembership& working : working_memberships) {
        transaction.membership_commits.emplace_back(
            VulkanPresentationSourceMembershipCommit{
                .bindless_array =
                    std::move(working.bindless_array),
                .membership =
                    std::shared_ptr<const Membership>(
                        std::move(working.membership)
                    ),
            }
        );
    }
    return transaction;
}

static void CommitAcceptedPresentationSourceStateDelta(
    const Array<VulkanPresentationSourceStateDelta>& _delta
) noexcept {
    for (const VulkanPresentationSourceStateDelta& entry :
         _delta) {
        if (!entry.texture) {
            continue;
        }
        static_cast<VulkanTexture*>(entry.texture.Get())
            ->PublishPresentationSourceReady(entry.ready);
    }
}

void CommitAcceptedPresentationSourceStateTransaction(
    VulkanPresentationSourceStateTransaction& _transaction
) noexcept {
    assert(
        _transaction.membership_commits.empty() ||
        _transaction.submission_lock.owns_lock()
    );
    for (VulkanPresentationSourceMembershipCommit& commit :
         _transaction.membership_commits) {
        if (!commit.bindless_array || !commit.membership) {
            continue;
        }
        static_cast<VulkanBindlessArray*>(
            commit.bindless_array.Get()
        )->PublishAcceptedPresentationSourceMembership(
            std::move(commit.membership)
        );
    }
    CommitAcceptedPresentationSourceStateDelta(
        _transaction.delta
    );
}

static bool IsRecoverableUnsubmittedSignal(
    VulkanDevice& _device, const VulkanOperationResult& _outcome
) {
    // A skipped Present reserves a logical value without submitting a GPU
    // signal. Cancel only that value; later submissions still use this fence.
    return (_outcome.status == EVulkanOperationStatus::Rejected ||
            _outcome.status == EVulkanOperationStatus::Retry ||
            _outcome.status == EVulkanOperationStatus::Recreate) &&
           !_device.IsFaulted() && _outcome.result != VK_ERROR_DEVICE_LOST;
}

void TerminalizeUnsubmittedSignal(
    VulkanDevice&                _device,
    VulkanTimeline*                 _fence,
    uint64                       _value,
    const VulkanOperationResult& _outcome
) {
    if (_fence == nullptr) {
        return;
    }
    if (IsRecoverableUnsubmittedSignal(_device, _outcome)) {
        _fence->Reject(_value);
    } else {
        const VkResult result =
            _outcome.result != VK_SUCCESS ? _outcome.result : VK_ERROR_UNKNOWN;
        _fence->Fail(result);
    }
}

void TerminalizeUnsubmittedSignals(
    VulkanDevice&                _device,
    const Array<SignalEvent>&    _signal_events,
    const VulkanOperationResult& _outcome
) {
    for (const SignalEvent& event : _signal_events) {
        TerminalizeUnsubmittedSignal(
            _device,
            reinterpret_cast<VulkanTimeline*>(event.timeline_handle),
            event.value,
            _outcome
        );
    }
}

QueryPublishBatch PublishUnsubmittedQueryErrors(
    CmdSubmit&                 _submit,
    VulkanCommandContextBatch* _command_contexts,
    std::string_view           _reason
) noexcept {
    const QueryPublishBatch query_batch =
        _submit.query_publish_batch.Valid() ?
            _submit.query_publish_batch :
            QueryBackendAccess::BeginPublishBatch();

    if (_command_contexts != nullptr) {
        for (auto& command_context : _command_contexts->submitted) {
            if (command_context) {
                command_context->PublishQueriesAfterGpuCompletion(query_batch, false, _reason);
            }
        }
        for (auto& command_context : _command_contexts->abandoned) {
            if (command_context) {
                command_context->PublishQueriesAfterGpuCompletion(query_batch, false, _reason);
            }
        }
    }
    // The submit-token pass is also the malformed/native-record fallback in
    // the opposite direction: a token without a command context record still
    // becomes terminal in the same transaction.
    _submit.PublishPendingQueryErrors(_reason, query_batch);
    return query_batch;
}

VkCommandQueue::VkCommandQueue(VulkanDevice& _device, EQueueType _type) :
    CommandQueue(),
    vk_device(_device),
    queue(_type, _device),
    timestamp_pool(
        _device,
        VK_QUERY_TYPE_TIMESTAMP,
        s_queue_max_frame_in_flight * s_query_max_storage * 4,
        _type
    ),
    profiler_storage(timestamp_pool) {
    timeline = MoerNew(VulkanTimeline(vk_device));

    try {
        completion_worker_running = true;
        completion_thread =
            std::jthread(&VkCommandQueue::CompletionThreadMain, this);
    } catch (...) {
        {
            std::lock_guard lock(event_mutex);
            completion_worker_running = false;
        }
        queue_cv.notify_all();
        if (completion_thread.joinable()) {
            completion_thread.join();
        }

        MoerDelete(timeline);
        timeline = nullptr;
        throw;
    }
}

VkCommandQueue::~VkCommandQueue() {
    Sync();

    // Presentation can still own swapchain images after Copy completion.
    if (queue.GetType() == EQueueType::Graphics) {
        vk_device.WaitIdle();
    }

    {
        std::unique_lock<std::mutex> lock(event_mutex);
        completion_worker_running = false;
    }
    queue_cv.notify_all();
    completion_thread.join();

    Array<VulkanCommandContext*> recycled_command_contexts;
    command_contexts.PopAll(recycled_command_contexts);
    for (auto* command_context : recycled_command_contexts) {
        MoerDelete(command_context);
    }

    command_context_quarantine.clear();
    MoerDelete(timeline);
    vk_device.RecordQueueSyncComplete();
}

bool VkCommandQueue::ClaimRuntimeOwnership() {
    bool expected = false;
    if (executor_claimed.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        try {
            LOG_INFO(
                "[RHIExecutor][Vulkan] queue={} ownership=Runtime",
                static_cast<uint32>(queue.GetType())
            );
        } catch (...) {
        }
        runtime_dependency_waits_enabled.store(true, std::memory_order_release);
        return true;
    }
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] queue={} is already claimed by an executor",
            static_cast<uint32>(queue.GetType())
        );
    } catch (...) {
    }
    return false;
}

void VkCommandQueue::ReleaseRuntimeOwnership() noexcept {
    CancelRuntimeDependencyWaits();
    bool expected = true;
    const bool released = executor_claimed.compare_exchange_strong(
        expected,
        false,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
    assert(released && "only the owning Vulkan runtime may release a queue claim");
}

void VkCommandQueue::CancelRuntimeDependencyWaits() noexcept {
    runtime_dependency_waits_enabled.store(false, std::memory_order_release);
}

WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] direct Execute is unsupported; "
            "publish work through RHIExecutor"
        );
    } catch (...) {
    }
    RejectForRuntime(std::move(_submit), VK_ERROR_UNKNOWN, true);
    return {0, 0};
}

std::optional<VkCommandQueue::CurrentVulkanRecordedSubmit>
VkCommandQueue::TranslateForRuntime(CmdSubmit&& _submit) noexcept {
    assert(
        executor_claimed.load(std::memory_order_acquire) &&
        "runtime translate requires an exclusively claimed queue"
    );
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "runtime command translation must run on the Translate owner"
    );
    auto execution_lease = runtime_execution_gate.Acquire();
    uint64 current_timeline = 0;
    std::optional<CurrentVulkanRecordedSubmit> recorded{};
    try {
        std::unique_lock<std::mutex> execution_lock(exec_mtx);
        current_timeline =
            last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        RecordSubmissionCommands(
            std::move(_submit),
            current_timeline,
            current_timeline,
            recorded
        );
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] command translation threw: {}",
                error.what()
            );
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] command translation threw");
        } catch (...) {
        }
    }
    if (recorded) {
        recorded->execution_lease = std::move(execution_lease);
        return recorded;
    }

    // Unexpected translate failures still travel through Submission as a
    // terminal packet. This lets the executor publish the whole failed batch
    // suffix before the current source becomes visible to Completion
    // callbacks. current_timeline remains zero only if queue ownership could
    // not be entered at all; that packet still has no native work to replay.
    CurrentVulkanRecordedSubmit rejected{
        std::move(_submit),
        VulkanOperationContext{
            .operation   = EVulkanFaultOperation::QueueSubmit,
            .queue_type  = queue.GetType(),
            .queue       = queue.GetHandle(),
            .timeline    = current_timeline,
            .work_serial = current_timeline,
        },
        current_timeline
    };
    rejected.execution_lease       = std::move(execution_lease);
    rejected.retirement_outcome    = {
        EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN
    };
    rejected.native_submit_resolved = true;
    vk_device.RecordRejectedSubmit();
    return rejected;
}

VulkanRuntimeSubmissionResult VkCommandQueue::SubmitRecordedForRuntime(
    CurrentVulkanRecordedSubmit           _recorded,
    const VulkanRuntimePreCompletionHook* _pre_completion
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "runtime native submit must run on the Submission owner"
    );
    assert(
        _recorded.execution_lease.OwnsGate() &&
        "runtime recorded submit must retain queue execution ownership"
    );
    const uint64 submitted_timeline = _recorded.timeline;
    const bool split_profiling_frame =
        _recorded.submit.EmitsProfilingQueries() &&
        _recorded.submit.ProfilingPhase() != ERHIProfilingPhase::Complete;

    if (!_recorded.native_submit_resolved) {
        try {
            std::unique_lock<std::mutex> execution_lock(exec_mtx);
            const VulkanOperationResult submit_result =
                SubmitRecorded(
                    _recorded,
                    &runtime_dependency_waits_enabled
                );
            if (split_profiling_frame && !submit_result.Succeeded()) {
                ResetSplitProfilingCpuFrame();
            }
        } catch (const std::exception& error) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] recorded submission threw before retirement: {}",
                    error.what()
                );
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] recorded submission threw before retirement"
                );
            } catch (...) {
            }
        }
    }

    if (!_recorded.native_submit_resolved) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedSubmit();
        _recorded.retirement_outcome =
            MakeUnsubmittedOutcome(VK_ERROR_UNKNOWN, false);
        _recorded.recoverable_rejection = false;
        _recorded.native_submit_resolved = true;
    }

    VulkanRuntimeSubmissionResult runtime_result{
        .outcome               = _recorded.retirement_outcome,
        .recoverable_rejection = _recorded.recoverable_rejection,
    };
    if (_recorded.retirement_outcome.Succeeded()) {
        runtime_result.completion = WaitEvent{uint64(timeline), submitted_timeline};
    }
    if (_pre_completion != nullptr &&
        _pre_completion->callback != nullptr) {
        _pre_completion->callback(
            _pre_completion->context, runtime_result
        );
    }
    if (!_recorded.completion_committed) {
        CommitRuntimeSubmitCompletion(_recorded, _recorded.retirement_outcome);
    }
    if (split_profiling_frame && !_recorded.retirement_outcome.Succeeded()) {
        ResetSplitProfilingCpuFrame();
    }
    return runtime_result;
}

void VkCommandQueue::RejectRecordedForRuntime(
    CurrentVulkanRecordedSubmit&& _recorded,
    VkResult                       _result,
    bool                           _recoverable
) noexcept {
    assert(
        _recorded.execution_lease.OwnsGate() &&
        "a failed Translate -> Submission handoff must retain queue ownership"
    );
    ResetSplitProfilingCpuFrame();
    if (!_recorded.native_submit_resolved) {
        vk_device.RecordRejectedSubmit();
        _recorded.retirement_outcome =
            MakeUnsubmittedOutcome(_result, _recoverable);
        _recorded.recoverable_rejection = _recoverable;
        _recorded.native_submit_resolved = true;
    }
    if (!_recorded.completion_committed) {
        CommitRuntimeSubmitCompletion(_recorded, _recorded.retirement_outcome);
    }
}

void VkCommandQueue::RejectForRuntime(
    CmdSubmit&& _submit,
    VkResult    _result,
    bool        _recoverable,
    VulkanBatchCompletionTicket _batch_completion
) noexcept {
    // This request never acquired recording ownership. Reject only its own
    // work; an active Translate/Submission packet may still own profiling state.
    const VulkanOperationResult outcome =
        MakeUnsubmittedOutcome(_result, _recoverable);
    vk_device.RecordRejectedSubmit();

    try {
        const std::string_view rejection_reason =
            "Vulkan runtime rejected the submit before GPU completion";
        const GpuCompletionPublishBatch completion_batch =
            _submit.gpu_completion_publish_batch.Valid() ?
                _submit.gpu_completion_publish_batch :
                GpuCompletionBackendAccess::BeginPublishBatch();
        _submit.PublishPendingGpuCompletionErrors(
            rejection_reason, completion_batch
        );
        PublishUnsubmittedQueryErrors(
            _submit,
            nullptr,
            rejection_reason
        );
        Array<SignalEvent> rejected_signals =
            std::move(_submit.signal_events);
        Array<FenceRef> rejected_signal_keepalives =
            std::move(_submit.signal_rejection_keepalives);
        _submit.signal_events.clear();
        _submit.signal_rejection_keepalives.clear();
        TerminalizeUnsubmittedSignals(
            vk_device,
            rejected_signals,
            outcome
        );
        std::unique_lock<std::mutex> lock(event_mutex);
        const uint64 retirement_serial = retirement_enqueued_serial + 1;
        const std::shared_ptr<VulkanBatchCompletionGroup>
            completion_group = _batch_completion.Valid() ?
                _batch_completion.group :
                nullptr;
        event_queue.emplace_back(
            std::in_place_type<VulkanSubmitCompletionBatch>,
            0,
            true,
            retirement_serial,
            outcome,
            VulkanOperationContext{
                .operation  = EVulkanFaultOperation::QueueSubmit,
                .queue_type = queue.GetType(),
                .queue      = queue.GetHandle(),
            },
            false,
            false,
            std::move(_submit),
            VulkanCommandContextBatch{},
            VulkanDescriptorBufferLease{},
            Array<RHIResource*>{},
            std::move(_batch_completion)
        );
        if (completion_group) {
            std::erase_if(
                batch_completion_settlements,
                [](const VulkanBatchCompletionSettlement& settlement) {
                    return settlement.group.expired();
                }
            );
            batch_completion_settlements.emplace_back(
                VulkanBatchCompletionSettlement{
                    .retirement_serial = retirement_serial,
                    .group             = completion_group,
                }
            );
        }
        retirement_enqueued_serial = retirement_serial;
        (void)rejected_signal_keepalives;
    } catch (...) {
        // Continuing would destroy callbacks/signals outside Completion and
        // violate the ownership contract. Allocation failure is therefore
        // treated as an unrecoverable runtime bookkeeping failure.
        std::terminate();
    }
    queue_cv.notify_one();
}

VulkanRuntimeSubmissionResult VkCommandQueue::PresentForRuntime(
    SwapchainRef      _swapchain,
    TextureView       _source,
    PresentReceiptRef _receipt
) noexcept {
    assert(
        executor_claimed.load(std::memory_order_acquire) &&
        "runtime Present requires an exclusively claimed queue"
    );
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "runtime Present must run on the Submission owner"
    );
    try {
        if (vk_device.IsFaulted()) {
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(
                _receipt, false, EPresentStatus::Faulted, EPresentStage::Validation
            );
            return {.outcome = {EVulkanOperationStatus::Rejected, vk_device.GetFirstFaultResult()}};
        }
        TextureRef                                     source_texture{_source.texture};
#if MOER_VULKAN_TEST_SUPPORT
        const VulkanScriptedPresentOverrideForTesting* scripted_override =
            GetVulkanScriptedPresentOverrideForTesting();
#endif
        const VulkanPresentSourceContract source_contract =
            ValidatePresentSourceContract(_source, _swapchain.Get());
        if (!source_contract.valid) {
#if MOER_VULKAN_TEST_SUPPORT
            InvokePresentSourceRejectionCallbackForTesting(scripted_override);
#endif
            bool                        recoverable_rejection = false;
            const VulkanOperationResult rejection =
                ClassifyPresentSourceRejection(vk_device, recoverable_rejection);
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(
                _receipt, false, PresentStatusFromVulkanOutcome(rejection), EPresentStage::Validation
            );
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] rejected Present source contract: {}", source_contract.reason
                );
            } catch (...) {
            }
            return {
                .outcome               = rejection,
                .recoverable_rejection = recoverable_rejection,
            };
        }
        auto* vk_source_texture = static_cast<VulkanTexture*>(_source.texture);
        if (!vk_source_texture->IsPresentationSourceReady()
#if MOER_VULKAN_TEST_SUPPORT
            && (scripted_override == nullptr || scripted_override->require_present_source_ready)
#endif
        ) {
#if MOER_VULKAN_TEST_SUPPORT
            InvokePresentSourceRejectionCallbackForTesting(scripted_override);
#endif
            bool                        recoverable_rejection = false;
            const VulkanOperationResult rejection =
                ClassifyPresentSourceRejection(vk_device, recoverable_rejection);
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(
                _receipt, false, PresentStatusFromVulkanOutcome(rejection), EPresentStage::Validation
            );
            try {
                LOG_ERROR("[RHIExecutor][Vulkan] rejected Present source before an "
                          "accepted producer export published GENERAL / TRANSFER_READ");
            } catch (...) {
            }
            return {
                .outcome               = rejection,
                .recoverable_rejection = recoverable_rejection,
            };
        }

        auto                         execution_lease = runtime_execution_gate.Acquire();
        std::unique_lock<std::mutex> execution_lock(exec_mtx);
        const uint64 current_timeline = last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
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
            CurrentVulkanRecordedSubmit recorded{CmdSubmit{{}, {}, {}, {}}, context, current_timeline};
            recorded.submit.callbacks.emplace_back([swapchain = std::move(_swapchain),
                                                    source    = std::move(source_texture)] {});
            recorded.deferred_releases       = TakeDeferredReleases();
            recorded.native_submit_resolved  = true;
            recorded.retirement_outcome      = scripted.outcome;
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
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Present owner setup threw before timeline reservation");
        } catch (...) {
        }
        return {
            .outcome =
                {EVulkanOperationStatus::Rejected,
                 vk_device.IsFaulted() ? vk_device.GetFirstFaultResult() : VK_ERROR_UNKNOWN}
        };
    }
}

VulkanOperationResult VkCommandQueue::SubmitRecorded(
    CurrentVulkanRecordedSubmit& _recorded,
    const std::atomic_bool*       _continue_waiting
) {
    assert(
        !_recorded.native_submit_resolved &&
        !_recorded.completion_committed &&
        "SubmitRecorded cannot replay a terminal Vulkan packet"
    );
    assert(
        _recorded.has_commands == !_recorded.ordered_cmd_lists.empty() &&
        "recorded Vulkan submit command-list ownership mismatch"
    );

    const auto end_tag = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VulkanOperationResult submit_outcome{};
    if (!WaitForSubmittedDependencies(
            vk_device,
            _recorded.submit.wait_events,
            queue.GetType(),
            _continue_waiting
        )) {
        vk_device.RecordRejectedSubmit();
        const VkResult rejected_result = GetRejectedSubmitResult(vk_device);
        submit_outcome = {EVulkanOperationStatus::Rejected, rejected_result};
        _recorded.recoverable_rejection = !vk_device.IsFaulted();
        _recorded.native_submit_resolved = true;
        _recorded.retirement_outcome     = submit_outcome;
    } else {
        std::optional<VulkanPresentationSourceStateTransaction>
            presentation_transaction{};
        const auto reject_presentation_transaction =
            [&](const VulkanOperationResult& _outcome,
                std::string_view             _reason) {
                try {
                    LOG_ERROR(
                        "[RHIExecutor][Vulkan] rejected before native "
                        "submit: timeline={} reason={} VkResult={}",
                        _recorded.timeline,
                        _reason,
                        static_cast<int32>(_outcome.result)
                    );
                } catch (...) {
                }
                vk_device.RecordRejectedSubmit();
                submit_outcome = _outcome;
                _recorded.recoverable_rejection =
                    _outcome.status ==
                        EVulkanOperationStatus::Rejected &&
                    !vk_device.IsFaulted() &&
                    _outcome.result != VK_ERROR_DEVICE_LOST;
                _recorded.native_submit_resolved = true;
                _recorded.retirement_outcome     = _outcome;
            };
        try {
            presentation_transaction.emplace(
                EvaluatePresentationSourceStateProgram(
                    _recorded.presentation_source_program
                )
            );
        } catch (const std::logic_error& error) {
            reject_presentation_transaction(
                VulkanOperationResult{
                    EVulkanOperationStatus::Rejected,
                    VK_ERROR_FEATURE_NOT_PRESENT
                },
                error.what()
            );
        } catch (const std::bad_alloc& error) {
            reject_presentation_transaction(
                VulkanOperationResult{
                    EVulkanOperationStatus::Rejected,
                    VK_ERROR_OUT_OF_HOST_MEMORY
                },
                error.what()
            );
        } catch (const std::exception& error) {
            reject_presentation_transaction(
                VulkanOperationResult{
                    EVulkanOperationStatus::Faulted,
                    VK_ERROR_UNKNOWN
                },
                error.what()
            );
        } catch (...) {
            reject_presentation_transaction(
                VulkanOperationResult{
                    EVulkanOperationStatus::Faulted,
                    VK_ERROR_UNKNOWN
                },
                "presentation-state transaction evaluation failed"
            );
        }

        if (!_recorded.native_submit_resolved) {
            queue.Signal(timeline, _recorded.timeline, end_tag);
            for (auto& event : _recorded.submit.wait_events) {
                queue.Wait(
                    reinterpret_cast<VulkanTimeline*>(
                        event.timeline_handle
                    ),
                    event.value,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                );
            }
            for (auto& event : _recorded.submit.signal_events) {
                queue.Signal(
                    reinterpret_cast<VulkanTimeline*>(
                        event.timeline_handle
                    ),
                    event.value,
                    end_tag
                );
            }
            for (const VkSemaphoreSubmitInfo& wait : _recorded.native_waits) {
                queue.Wait(wait.semaphore, wait.stageMask);
            }
            for (const VkSemaphoreSubmitInfo& signal : _recorded.native_signals) {
                queue.Signal(signal.semaphore, signal.stageMask);
            }
            submit_outcome =
                _recorded.has_commands ?
                    queue.Submit(
                        std::span<VulkanCommandList* const>(
                            _recorded.ordered_cmd_lists.data(), _recorded.ordered_cmd_lists.size()
                        ),
                        _recorded.context
                    ) :
                    queue.SubmitEmpty(_recorded.context);
            _recorded.native_submit_resolved = true;
            // Persist the native result before any signal-fence bookkeeping
            // can throw. Once vkQueueSubmit2 accepts the packet, every later
            // exception must still retire it as GPU-submitted.
            _recorded.retirement_outcome = submit_outcome;
            if (submit_outcome.Succeeded()) {
                // Descriptor membership and texture readiness publish as one
                // accepted transaction before any host fence is observable.
                CommitAcceptedPresentationSourceStateTransaction(
                    *presentation_transaction
                );
                MarkSubmissionAccepted(
                    timeline,
                    _recorded.timeline,
                    _recorded.submit.signal_events
                );
            }
        }
    }
    return submit_outcome;
}

void VkCommandQueue::CommitRuntimeSubmitCompletion(
    CurrentVulkanRecordedSubmit& _recorded,
    const VulkanOperationResult&  _outcome
) noexcept {
    assert(!_recorded.completion_committed);
    try {
        if (!_outcome.Succeeded()) {
            _recorded.command_contexts.abandoned.reserve(
                _recorded.command_contexts.abandoned.size() + _recorded.command_contexts.submitted.size()
            );
            const std::string_view rejection_reason =
                "Vulkan submission was rejected before GPU completion";
            const GpuCompletionPublishBatch completion_batch =
                _recorded.submit.gpu_completion_publish_batch.Valid() ?
                    _recorded.submit.gpu_completion_publish_batch :
                    GpuCompletionBackendAccess::BeginPublishBatch();
            _recorded.submit.PublishPendingGpuCompletionErrors(
                rejection_reason, completion_batch
            );
            PublishUnsubmittedQueryErrors(_recorded.submit, &_recorded.command_contexts, rejection_reason);
            TerminalizeUnsubmittedSignal(
                vk_device,
                timeline,
                _recorded.timeline,
                _outcome
            );
            Array<SignalEvent> rejected_signals =
                std::move(_recorded.submit.signal_events);
            Array<FenceRef> rejected_signal_keepalives =
                std::move(
                    _recorded.submit.signal_rejection_keepalives
                );
            _recorded.submit.signal_events.clear();
            _recorded.submit.signal_rejection_keepalives.clear();
            TerminalizeUnsubmittedSignals(
                vk_device, rejected_signals, _outcome
            );
            for (auto& command_context : _recorded.command_contexts.submitted) {
                _recorded.command_contexts.abandoned.emplace_back(std::move(command_context));
            }
            _recorded.command_contexts.submitted.clear();
            (void)rejected_signal_keepalives;
        }

        std::unique_lock<std::mutex> lock(event_mutex);
        const uint64 retirement_serial = retirement_enqueued_serial + 1;
        const std::shared_ptr<VulkanBatchCompletionGroup>
            completion_group = _recorded.batch_completion.Valid() ?
                _recorded.batch_completion.group :
                nullptr;
        event_queue.emplace_back(
            std::in_place_type<VulkanSubmitCompletionBatch>,
            _recorded.timeline,
            true,
            retirement_serial,
            _outcome,
            _recorded.context,
            _outcome.Succeeded(),
            _outcome.Succeeded(),
            std::move(_recorded.submit),
            std::move(_recorded.command_contexts),
            std::move(_recorded.descriptor_lease),
            std::move(_recorded.deferred_releases),
            std::move(_recorded.batch_completion)
        );
        if (completion_group) {
            std::erase_if(
                batch_completion_settlements,
                [](const VulkanBatchCompletionSettlement& settlement) {
                    return settlement.group.expired();
                }
            );
            batch_completion_settlements.emplace_back(
                VulkanBatchCompletionSettlement{
                    .retirement_serial = retirement_serial,
                    .group             = completion_group,
                }
            );
        }
        retirement_enqueued_serial     = retirement_serial;
        _recorded.completion_committed = true;
    } catch (...) {
        // The packet may own command pools already submitted to the GPU. It is
        // never safe to unwind and destroy it on the Submission owner.
        std::terminate();
    }
    queue_cv.notify_one();
}

void VkCommandQueue::RecordSubmissionCommands(
    CmdSubmit&& _submit,
    uint64 _timeline,
    uint64 _serial,
    std::optional<CurrentVulkanRecordedSubmit>& _recorded
) {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "command recording must run on the Translate owner"
    );

    const ERHIProfilingPhase profiling_phase = _submit.ProfilingPhase();
    const bool emits_profiling_queries       = _submit.EmitsProfilingQueries();
    const bool begins_profiling_frame        = _submit.BeginsProfilingFrame();
    const bool ends_profiling_frame          = _submit.EndsProfilingFrame();
    const bool complete_profiling_frame =
        profiling_phase == ERHIProfilingPhase::Complete;

    const VulkanOperationContext context{
        .operation   = EVulkanFaultOperation::QueueSubmit,
        .queue_type  = queue.GetType(),
        .queue       = queue.GetHandle(),
        .timeline    = _timeline,
        .work_serial = _serial,
    };
    if (vk_device.IsFaulted()) {
        ResetSplitProfilingCpuFrame();
        vk_device.RecordRejectedSubmit();
        const VkResult rejected_result = vk_device.GetFirstFaultResult();
        CurrentVulkanRecordedSubmit rejected{
            std::move(_submit), context, _timeline
        };
        rejected.retirement_outcome = {
            EVulkanOperationStatus::Rejected, rejected_result
        };
        rejected.native_submit_resolved = true;
        _recorded.emplace(std::move(rejected));
        return;
    }

    size_t timestamp_query_slot_count = 0;
    size_t occlusion_query_pair_count = 0;
    for (const UniquePtr<Command>& command : _submit.cmds) {
        if (!command || command->Type() != Command::EType::Query) {
            continue;
        }
        const auto& query = *static_cast<const QueryCmd*>(command.get());
        if (query.IsTimestamp()) {
            ++timestamp_query_slot_count;
        } else if (query.IsOcclusion() && query.IsBegin()) {
            ++occlusion_query_pair_count;
        }
    }
    auto reject_before_native_record =
        [&](const VulkanOperationResult&     _outcome,
            std::string_view                 _reason,
            UniquePtr<VulkanCommandContext>* _abandoned_command_context = nullptr) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] rejected before native record: "
                    "timeline={} reason={} VkResult={}",
                    _timeline,
                    _reason,
                    static_cast<int32_t>(_outcome.result)
                );
            } catch (...) {
            }

            ResetSplitProfilingCpuFrame();
            Array<UniquePtr<VulkanCommandContext>> abandoned_command_contexts;
            if (_abandoned_command_context != nullptr && *_abandoned_command_context) {
                // Complete every allocation before CmdSubmit moves into the
                // terminal packet. If reserve/emplace fails, the submit is
                // still owned by recording and cannot notify on Translate.
                abandoned_command_contexts.reserve(1);
                abandoned_command_contexts.emplace_back(std::move(*_abandoned_command_context));
            }
            CurrentVulkanRecordedSubmit rejected{
                std::move(_submit), context, _timeline
            };
            rejected.command_contexts.abandoned = std::move(abandoned_command_contexts);
            rejected.retirement_outcome     = _outcome;
            rejected.recoverable_rejection =
                _outcome.status == EVulkanOperationStatus::Rejected &&
                !vk_device.IsFaulted() &&
                _outcome.result != VK_ERROR_DEVICE_LOST;
            rejected.native_submit_resolved = true;
            vk_device.RecordRejectedSubmit();
            assert(
                !_recorded.has_value() &&
                "pre-record rejection cannot overwrite a recorded packet"
            );
            _recorded.emplace(std::move(rejected));
        };

    VulkanPresentationSourceStateProgram
        presentation_source_program{};
    try {
        presentation_source_program =
            BuildPresentationSourceStateProgram(
                _submit, queue.GetType()
            );
    } catch (const std::logic_error& error) {
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Rejected,
                VK_ERROR_FEATURE_NOT_PRESENT
            },
            error.what()
        );
        return;
    } catch (const std::bad_alloc& error) {
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Rejected,
                VK_ERROR_OUT_OF_HOST_MEMORY
            },
            error.what()
        );
        return;
    } catch (const std::exception& error) {
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Faulted,
                VK_ERROR_UNKNOWN
            },
            error.what()
        );
        return;
    } catch (...) {
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Faulted,
                VK_ERROR_UNKNOWN
            },
            "presentation-state delta construction failed"
        );
        return;
    }

    if (timestamp_query_slot_count != 0 &&
        vk_device.GetTimestampValidBits(queue.GetType()) == 0) {
        // Queue capability is known before any command buffer begins. Treat
        // unsupported timestamps as a recoverable query-submit rejection, not
        // as a native-record/device fault.
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Rejected,
                VK_ERROR_FEATURE_NOT_PRESENT
            },
            "queue family does not support timestamp queries"
        );
        return;
    }
    if (occlusion_query_pair_count != 0 &&
        queue.GetType() != EQueueType::Graphics) {
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Rejected,
                VK_ERROR_FEATURE_NOT_PRESENT
            },
            "occlusion queries require the Graphics queue"
        );
        return;
    }
    if (timestamp_query_slot_count >
        std::numeric_limits<uint32>::max()) {
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Rejected,
                VK_ERROR_OUT_OF_POOL_MEMORY
            },
            "timestamp query slot count exceeds uint32 capacity"
        );
        return;
    }
    if (occlusion_query_pair_count >
        std::numeric_limits<uint32>::max()) {
        reject_before_native_record(
            VulkanOperationResult{
                EVulkanOperationStatus::Rejected,
                VK_ERROR_OUT_OF_POOL_MEMORY
            },
            "occlusion query pair count exceeds uint32 capacity"
        );
        return;
    }

    const bool compiled_commands = _submit.resource_state_ownership == ERHIResourceStateOwnership::Compiled;
    if (!compiled_commands && !_submit.cmds.empty()) {
        reject_before_native_record(
            {EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
            "submission did not provide a compiled resource plan"
        );
        return;
    }
    if (compiled_commands && !SupportsVulkanCompiledCommands(_submit, queue.GetType())) {
        reject_before_native_record(
            {EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT},
            "compiled submission contains an unsupported command or inferred barrier"
        );
        return;
    }
    VulkanDescriptorBufferLease                          descriptor_lease;
    std::shared_ptr<const VulkanDescriptorRecordingPlan> descriptor_plan;
    auto                                                 release_unsubmitted_descriptors = OnScopeExit([&] {
        // Moving the lease into a
        // recorded packet transfers retirement to existing Completion handling.
        if (descriptor_lease) {
            vk_device.GetGlobalDescriptorHeap().RecycleDescriptorBufferLease(std::move(descriptor_lease));
        }
    });
    if (queue.GetType() != EQueueType::Copy) {
        try {
            descriptor_lease = vk_device.GetGlobalDescriptorHeap().CreateDescriptorBufferLease();
            descriptor_plan = PrepareDescriptorRecordingPlan(vk_device, _submit, descriptor_lease);
        } catch (const std::bad_alloc&) {
            reject_before_native_record(
                {EVulkanOperationStatus::Rejected, VK_ERROR_OUT_OF_HOST_MEMORY},
                "descriptor reservation metadata allocation failed"
            );
            return;
        }
        if (!descriptor_plan) {
            reject_before_native_record(
                vk_device.IsFaulted() ?
                    VulkanOperationResult{EVulkanOperationStatus::Faulted, vk_device.GetFirstFaultResult()} :
                    VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_OUT_OF_POOL_MEMORY},
                "descriptor batch does not fit available storage (capacity or contiguous bind size)"
            );
            return;
        }
    }

    Timer timer{};

    //Get Allocators for buffer, texture and commandlist
    auto  command_context_ptr = std::move(AcquireCommandContext());
    auto& vk_command_context  = *command_context_ptr;
#if MOER_VULKAN_TEST_SUPPORT
    if (timestamp_query_slot_count != 0) {
        const VulkanScriptedQueryPreparationOverrideForTesting*
            scripted_override =
                GetVulkanScriptedQueryPreparationOverrideForTesting();
        if (scripted_override != nullptr) {
            VkResult scripted_result = scripted_override->callback(
                scripted_override->context,
                queue.GetType(),
                _timeline,
                static_cast<uint32>(timestamp_query_slot_count)
            );
            if (scripted_result != VK_SUCCESS) {
                // This seam must never masquerade as a native device loss.
                // Keep malformed test callbacks inside the recoverable path.
                if (scripted_result == VK_ERROR_DEVICE_LOST) {
                    scripted_result = VK_ERROR_FEATURE_NOT_PRESENT;
                }
                reject_before_native_record(
                    VulkanOperationResult{EVulkanOperationStatus::Rejected, scripted_result},
                    "scripted timestamp query preparation rejection",
                    &command_context_ptr
                );
                return;
            }
        }
    }
#endif
    try {
        // Size (and lazily create) its query pools before Begin() so
        // a legal large query packet cannot fail after native recording has
        // already produced side effects. Timestamp boundaries consume one
        // slot each; an occlusion begin/end pair shares one slot.
        vk_command_context.EnsureQueryCapacity(timestamp_query_slot_count, occlusion_query_pair_count);
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] query pool preparation "
                "failed: {}",
                error.what()
            );
        } catch (...) {
        }
        const bool device_faulted = vk_device.IsFaulted();
        reject_before_native_record(
            VulkanOperationResult{
                device_faulted ? EVulkanOperationStatus::Faulted : EVulkanOperationStatus::Rejected,
                device_faulted ? vk_device.GetFirstFaultResult() : VK_ERROR_OUT_OF_POOL_MEMORY
            },
            "query pool preparation failed",
            &command_context_ptr
        );
        return;
    } catch (...) {
        const bool device_faulted = vk_device.IsFaulted();
        reject_before_native_record(
            VulkanOperationResult{
                device_faulted ? EVulkanOperationStatus::Faulted : EVulkanOperationStatus::Rejected,
                device_faulted ? vk_device.GetFirstFaultResult() : VK_ERROR_OUT_OF_POOL_MEMORY
            },
            "query pool preparation failed",
            &command_context_ptr
        );
        return;
    }

    //Visitor for actual command recording
    VkCmdVisitor visitor(
        vk_device,
        vk_command_context,
        vk_command_context.GetCommandList(),
        _submit.cached_args,
        &profiler_storage,
        emits_profiling_queries
    );

    // Start batch-local recording and its debug region.
    const bool has_cmd = !_submit.cmds.empty();
    struct SerialNativeRecordFailure {
        std::string_view reason;
        VkResult        result{VK_ERROR_UNKNOWN};
    };
    auto reject_serial_recording = [&](std::string_view _reason, VkResult _result) {
        if (_result == VK_SUCCESS) {
            _result = VK_ERROR_UNKNOWN;
        }
        try {
            LOG_ERROR(
                "[SerialRecord] batch {} rejected: reason={} VkResult={}",
                _serial,
                _reason,
                static_cast<int32_t>(_result)
            );
        } catch (...) {
        }
        ResetSplitProfilingCpuFrame();
        // Recording may already have executed CPU-side preprocessing or
        // transient allocation. Replaying the packet is therefore unsafe.
        vk_device.TryLatchFirstFault(context, _result, false, false, true);
        vk_device.RecordRejectedSubmit();

        Array<UniquePtr<VulkanCommandContext>> abandoned_command_contexts;
        if (command_context_ptr) {
            abandoned_command_contexts.push_back(std::move(command_context_ptr));
        }
        Array<RHIResource*> deferred_releases = TakeDeferredReleases();
        CurrentVulkanRecordedSubmit rejected{
            std::move(_submit), context, _timeline
        };
        rejected.command_contexts.abandoned = std::move(abandoned_command_contexts);
        rejected.descriptor_lease     = std::move(descriptor_lease);
        rejected.deferred_releases    = std::move(deferred_releases);
        rejected.retirement_outcome = {
            EVulkanOperationStatus::Rejected, _result
        };
        rejected.native_submit_resolved = true;
        _recorded.emplace(std::move(rejected));
    };

    auto record_serial_commands = [&] {
    if (has_cmd) {
        vk_command_context.GetCommandList().SetDescriptorRecordingPlan(descriptor_plan);
        const VkResult begin_result = vk_command_context.GetCommandList().Begin();
        if (begin_result != VK_SUCCESS) {
            throw SerialNativeRecordFailure{"command-buffer-begin-failed", begin_result};
        }
        if (begins_profiling_frame) {

            profiler_storage.CollectProfiling(vk_command_context.GetCommandList().GetHandle());

            {
                std::unique_lock<std::mutex> profiler_lock(profiler_mutex);
                cached_profiler_entry = profiler_storage.GetProfilerEntry();
            }
            profiler_storage.BeginProfilerSession(vk_command_context.GetCommandList(), "Graphics Exec");

            if (complete_profiling_frame) {
                ResetSplitProfilingCpuFrame();
                timer.Start();
            } else {
                BeginSplitProfilingCpuFrame();
            }
        }

        const std::string_view queue_label = !_submit.debug_label.empty() ?
                                                 std::string_view(_submit.debug_label) :
                                             queue.GetType() == EQueueType::Graphics ? "Graphics Exec" :
                                             queue.GetType() == EQueueType::Compute  ? "Compute Exec" :
                                                                                       "Copy Exec";
        const float4 queue_label_color = !_submit.debug_label.empty() ?
                                             _submit.debug_label_color :
                                             float4{1.0f, 0.0f, 0.0f, 1.0f};
        vk_command_context.GetCommandList().BeginLabel(queue_label, queue_label_color);
    }

    RecordCompiledCommands(vk_device, vk_command_context, _submit, visitor, queue.GetType());
    if (has_cmd) {
        if (ends_profiling_frame) {
            profiler_storage.EndProfilerSession(vk_command_context.GetCommandList(), "Graphics Exec");

        }
        vk_command_context.GetCommandList().EndLabel();
        const VkResult end_result = vk_command_context.GetCommandList().End();
        if (end_result != VK_SUCCESS) {
            throw SerialNativeRecordFailure{"command-buffer-end-failed", end_result};
        }
        if (queue.GetType() != EQueueType::Copy) {

            vk_device.GetGlobalDescriptorHeap().FlushDescriptorWrites(descriptor_lease);
        }

    }
    };

    try {
        record_serial_commands();
    } catch (const SerialNativeRecordFailure& failure) {
        reject_serial_recording(failure.reason, failure.result);
        return;
    } catch (const StaleBindlessUpdateBatch& error) {
        try {
            LOG_WARNING(
                "[SerialRecord] rejected stale bindless batch: {}", error.what()
            );
        } catch (...) {
        }
        reject_serial_recording(
            "stale-bindless-update", VK_ERROR_VALIDATION_FAILED_EXT
        );
        return;
    } catch (const std::exception& error) {
        reject_serial_recording(error.what(), VK_ERROR_OUT_OF_POOL_MEMORY);
        return;
    } catch (...) {
        reject_serial_recording("unknown-recording-exception", VK_ERROR_UNKNOWN);
        return;
    }

    Array<RHIResource*> deferred_releases = TakeDeferredReleases();
    Array<VulkanCommandList*> serial_cmd_lists;
    VulkanCommandContextBatch serial_command_contexts;
    if (has_cmd) {
        serial_cmd_lists.reserve(1);
        serial_cmd_lists.emplace_back(&vk_command_context.GetCommandList());
    }
    serial_command_contexts.submitted.reserve(1);
    serial_command_contexts.submitted.emplace_back(std::move(command_context_ptr));

    // From this point onward every ownership member is moved through noexcept
    // constructors/assignments. No allocation may unwind a moved CmdSubmit on
    // the Translate owner.
    CurrentVulkanRecordedSubmit recorded_submit{
        std::move(_submit), context, _timeline
    };
    recorded_submit.presentation_source_program =
        std::move(presentation_source_program);
    recorded_submit.has_commands = has_cmd;
    recorded_submit.ordered_cmd_lists =
        std::move(serial_cmd_lists);
    recorded_submit.command_contexts  = std::move(serial_command_contexts);
    recorded_submit.descriptor_lease  = std::move(descriptor_lease);
    recorded_submit.deferred_releases = std::move(deferred_releases);
    _recorded.emplace(std::move(recorded_submit));

    if (has_cmd && complete_profiling_frame) {
        timer.Stop();
        profiler_storage.RegisterCpuTimestamp("Queue Execution", timer.ElapsedMilliseconds());
        profiler_storage.AdvanceFrame();
    } else if (has_cmd && ends_profiling_frame) {
        EndSplitProfilingCpuFrame();
    }
}

void VkCommandQueue::Present(SwapchainRef, TextureView, PresentReceiptRef _receipt) {
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] direct Present is unsupported; "
            "publish work through RHIExecutor"
        );
    } catch (...) {
    }
    vk_device.RecordRejectedPresent();
    ResolvePresentReceiptNoexcept(
        _receipt, false, EPresentStatus::Rejected, EPresentStage::Validation
    );
}

VulkanRuntimeSubmissionResult VkCommandQueue::PresentSwapchain(
    SwapchainRef&&    _sc,
    TextureRef&&      _source_texture,
    TextureView       _view,
    PresentReceiptRef _receipt,
    uint64            _timeline,
    uint64            _serial
) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "Present must run on the Submission owner"
    );

    const VulkanOperationContext context{
        .operation   = EVulkanFaultOperation::PresentSubmit,
        .queue_type  = queue.GetType(),
        .queue       = queue.GetHandle(),
        .timeline    = _timeline,
        .work_serial = _serial,
    };
    CurrentVulkanRecordedSubmit recorded{CmdSubmit{{}, {}, {}, {}}, context, _timeline};
    auto*                       sc = ResourceCast(_sc.Get());
    VulkanOperationResult final_outcome{EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
    bool                  present_accepted      = false;
    bool                  recoverable_rejection = false;
    EPresentStatus        receipt_status        = EPresentStatus::Unresolved;
    EPresentStage         receipt_stage         = EPresentStage::None;

    try {
        // Retain Copy inputs through the ordinary GPU completion callbacks.
        recorded.submit.callbacks.emplace_back([swapchain = std::move(_sc),
                                                source    = std::move(_source_texture)] {});
#if MOER_VULKAN_TEST_SUPPORT
        const auto* scripted_override = GetVulkanScriptedPresentOverrideForTesting();
#endif
        const auto  source_contract   = ValidatePresentSourceContract(_view, sc);
        if (vk_device.IsFaulted()) {
            vk_device.RecordRejectedPresent();
            final_outcome = {EVulkanOperationStatus::Rejected, vk_device.GetFirstFaultResult()};
            AccumulatePresentReceiptOutcome(
                receipt_status, receipt_stage, final_outcome, EPresentStage::Validation
            );
        } else if (!source_contract.valid ||
                   !static_cast<VulkanTexture*>(_view.texture)->IsPresentationSourceReady()) {
#if MOER_VULKAN_TEST_SUPPORT
            InvokePresentSourceRejectionCallbackForTesting(scripted_override);
#endif
            final_outcome = ClassifyPresentSourceRejection(vk_device, recoverable_rejection);
            vk_device.RecordRejectedPresent();
            AccumulatePresentReceiptOutcome(
                receipt_status, receipt_stage, final_outcome, EPresentStage::Validation
            );
            LOG_ERROR(
                "[RHIExecutor][Vulkan] rejected Present source contract: {}",
                source_contract.valid ? "producer export is not ready" : source_contract.reason
            );
        } else {
            recorded.command_contexts.submitted.push_back(AcquireCommandContext());
            auto& vk_command_context = *recorded.command_contexts.submitted.back();
            const auto acquire            = sc->AcquireNextImage(
                vk_command_context.GetImageReadySemaphore(), kPresentAcquireTimeoutNanoseconds
            );
            final_outcome = acquire.outcome;
            AccumulatePresentReceiptOutcome(
                receipt_status, receipt_stage, acquire.outcome, EPresentStage::Acquire
            );
            if (acquire.HasImage()) {
                // An acquired binary semaphore cannot be reused if this
                // recording never reaches the GPU wait operation.
                vk_command_context.MarkImageAcquired();
                auto&      vk_cmd_list   = vk_command_context.GetCommandList();
                VulkanBarrierRecorder barriers;
                auto*      vk_src_tex    = static_cast<VulkanTexture*>(_view.texture);
                const uint idx           = acquire.image_index;
                auto*      swapchain_tex = ResourceCast(sc->GetSwapchainImage(idx).texture);
                VK_CHECK_RESULT(vk_cmd_list.Begin());
                const std::string present_label = std::format("Present: {}", vk_src_tex->GetName());
                vk_cmd_list.BeginLabel(present_label, {0.0f, 0.85f, 0.95f, 1.0f});
                // The producer exports GENERAL; Present consumes that boundary.
                barriers.Append(
                    vk_src_tex,
                    VulkanEnumTranslator::METoVKImageAspectFlags(vk_src_tex->GetAspectFlags()),
                    0,
                    1,
                    0,
                    1,
                    EBarrierQueueTransferPhase::None,
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_QUEUE_FAMILY_IGNORED,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                    VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL
                );
                barriers.Append(swapchain_tex, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                    EBarrierQueueTransferPhase::None, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                barriers.Flush(vk_cmd_list);
                vk_cmd_list.InsertLabel("Copy Present Image", GpuMarkerPalette::Transfer());
                vk_cmd_list.CopyTexture(
                    vk_src_tex,
                    swapchain_tex,
                    _view.extent,
                    {0, 0, 0},
                    {0, 0, 0},
                    0,
                    0,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                );
                barriers.Append(swapchain_tex, VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1,
                    EBarrierQueueTransferPhase::None, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                barriers.Flush(vk_cmd_list);
                vk_cmd_list.EndLabel();
                VK_CHECK_RESULT(vk_cmd_list.End());

                recorded.ordered_cmd_lists.push_back(&vk_cmd_list);
                recorded.has_commands = true;
                recorded.native_waits.push_back(VkSemaphoreSubmitInfo{
                    .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = acquire.ready_semaphore,
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                });
                recorded.native_signals.push_back(VkSemaphoreSubmitInfo{
                    .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                    .semaphore = sc->GetRenderFinishedSemaphore(idx),
                    .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                });
                recorded.deferred_releases = TakeDeferredReleases();
                // Keep Copy inputs alive until Present has been issued, even
                // if GPU execution finishes immediately.
                const VulkanOperationResult copy_result = SubmitRecorded(recorded);
                final_outcome          = copy_result;
                recoverable_rejection  = recorded.recoverable_rejection;
                AccumulatePresentReceiptOutcome(
                    receipt_status, receipt_stage, final_outcome, EPresentStage::Submit
                );
                if (copy_result.Succeeded()) {
                    const auto result = sc->Present(queue.GetHandle(), idx, _timeline, _serial);
                    present_accepted  = result.accepted;
                    AccumulatePresentReceiptOutcome(
                        receipt_status, receipt_stage, result.outcome, EPresentStage::Present
                    );
                    if (!result.outcome.Succeeded()) {
                        final_outcome = result.outcome;
                    }
                }
            } else {
                recorded.deferred_releases = TakeDeferredReleases();
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
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Present threw before retirement: {}", error.what());
        } catch (...) {
        }
    } catch (...) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedPresent();
        final_outcome = {EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
        AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Submit);
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Present threw before retirement");
        } catch (...) {
        }
    }

    if (!recorded.native_submit_resolved) {
        recorded.retirement_outcome     = final_outcome;
        recorded.native_submit_resolved = true;
    }
    const bool gpu_submitted = recorded.retirement_outcome.Succeeded();
    // A Present failure must never overwrite an accepted Copy's GPU outcome.
    CommitRuntimeSubmitCompletion(recorded, recorded.retirement_outcome);
    if (receipt_status == EPresentStatus::Unresolved) {
        AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Submit);
    }
    ResolvePresentReceiptNoexcept(_receipt, present_accepted, receipt_status, receipt_stage);
    VulkanRuntimeSubmissionResult result{
        .outcome               = final_outcome,
        .recoverable_rejection = recoverable_rejection,
    };
    if (gpu_submitted) {
        result.completion = WaitEvent{uint64(timeline), _timeline};
    }
    return result;
}

void VkCommandQueue::Sync() {
    if (GetCurrentRHIThreadRole() == ERHIThreadRole::Completion) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] queue={} Completion owner cannot block "
                "on a retirement queue",
                static_cast<uint32>(queue.GetType())
            );
        } catch (...) {
        }
        return;
    }
    uint64 target_timeline = 0;
    {
        std::unique_lock<std::mutex> lock(exec_mtx);
        target_timeline = last_frame.load(std::memory_order_relaxed);
    }

    CompleteAll(target_timeline);

    if (vk_device.IsFaulted()) {
        return;
    }

    Array<RHIResource*> deleted_resources;
    if (queue.GetType() != EQueueType::Graphics) {
        return;
    }
    {
        std::unique_lock<std::mutex> lock(exec_mtx);
        if (last_frame.load(std::memory_order_relaxed) == target_timeline) {
            vk_device.deferred_release_queue.PopAll(deleted_resources);
        }
    }
    for (auto* resource : deleted_resources) {
        MoerDelete(resource);
    }
}

UniquePtr<VulkanCommandContext> VkCommandQueue::AcquireCommandContext() {
    // Completion publishes only successfully completed and reset command contexts
    // into this pool. Pool membership is therefore the reuse gate; if
    // Completion is delayed by unrelated CPU callbacks, allocate overflow
    // storage instead of stalling the Translate owner.
    auto command_context = std::move(UniquePtr<VulkanCommandContext>(command_contexts.Pop()));
    if (command_context) {
        return std::move(command_context);
    }
    return MakeUnique<VulkanCommandContext>(&vk_device, queue.GetType());
}

Array<RHIResource*> VkCommandQueue::TakeDeferredReleases() {
    if (queue.GetType() != EQueueType::Graphics) {
        return {};
    }

    Array<RHIResource*> deleted_resources;
    // Recorded and submitted commands retain their resources through
    // RHICommandLifetime until Completion finishes all callback tiers.
    vk_device.deferred_release_queue.PopAll(deleted_resources);

    return deleted_resources;
}

void VkCommandQueue::EnqueueCompletionMarker(uint64 _timeline) {
    const uint64 retirement_serial = retirement_enqueued_serial + 1;
    event_queue.emplace_back(
        VulkanCompletionMarker{}, _timeline, true, retirement_serial
    );
    retirement_enqueued_serial = retirement_serial;
}

void VkCommandQueue::CompletionThreadMain() {
    Platform::SetCurrentThreadName(
        queue.GetType() == EQueueType::Graphics ? "Vulkan Gfx Completion" : "Vulkan Compute Completion"
    );
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Completion);
    RHIThreadHeartbeatScope heartbeat(
        ERHIThreadRole::Completion,
        HeartbeatDomainForQueue(queue.GetType()),
        ERHIHeartbeatStage::PollCompletion
    );

    bool batch_gpu_success = false;
    bool batch_release_safe = false;
    while (true) {
        heartbeat.Pulse(ERHIHeartbeatStage::PollCompletion);
        std::optional<QueueEvent> evt;
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            RHIThreadHeartbeatWaitLock wait_lock(
                lock, heartbeat, ERHIHeartbeatStage::PollCompletion
            );
            queue_cv.wait(wait_lock, [this]() {
                return !completion_worker_running || !event_queue.empty();
            });
            if (!completion_worker_running && event_queue.empty()) {
                heartbeat.Pulse(ERHIHeartbeatStage::Shutdown);
                break;
            }
            if (event_queue.empty()) {
                continue;
            }

            evt.emplace(std::move(event_queue.front()));
            event_queue.pop_front();
        }

        heartbeat.Pulse(ERHIHeartbeatStage::AwaitCompletion);
        const uint64 event_timeline = evt->timeline;
        std::visit(
            Overload{
                [this, event_timeline, &batch_gpu_success, &batch_release_safe](
                    VulkanSubmissionEvent& _submission
                ) {
                    batch_gpu_success = false;
                    batch_release_safe =
                        !_submission.gpu_submitted &&
                        (_submission.outcome.status == EVulkanOperationStatus::Retry ||
                         _submission.outcome.status == EVulkanOperationStatus::Recreate);
                    if (!_submission.gpu_submitted) {
                        if (_submission.outcome.status ==
                            EVulkanOperationStatus::Faulted) {
                            timeline->Fail(_submission.outcome.result);
                        } else if (
                            _submission.outcome.status ==
                            EVulkanOperationStatus::Rejected
                        ) {
                            timeline->Reject(event_timeline);
                        }
                        return;
                    }

                    if (timeline->Wait(event_timeline) && !vk_device.IsDeviceLost()) {
                        batch_gpu_success = true;
                        batch_release_safe = true;
                    } else {
                        timeline->Fail(timeline->QueryGpuCompletion(event_timeline));
                    }
                },
                [this, event_timeline, &batch_gpu_success, &batch_release_safe](
                    VulkanSubmitCompletionBatch& _batch
                ) {
                    batch_gpu_success = false;
                    batch_release_safe =
                        !_batch.gpu_submitted &&
                        (_batch.outcome.status == EVulkanOperationStatus::Retry ||
                         _batch.outcome.status == EVulkanOperationStatus::Recreate);

                    VkResult completion_result = _batch.outcome.result;
                    if (completion_result == VK_SUCCESS && !_batch.gpu_submitted) {
                        completion_result = VK_ERROR_UNKNOWN;
                    }
                    const bool recoverable_rejection =
                        !_batch.gpu_submitted &&
                        _batch.outcome.status ==
                            EVulkanOperationStatus::Rejected &&
                        !vk_device.IsFaulted() &&
                        _batch.outcome.result != VK_ERROR_DEVICE_LOST;

                    if (_batch.gpu_submitted) {
                        if (timeline->Wait(event_timeline) && !vk_device.IsFaulted()) {
                            batch_gpu_success = true;
                            batch_release_safe = true;
                            completion_result = VK_SUCCESS;

                            for (auto& command_context : _batch.command_contexts.submitted) {
                                const VkResult query_result =
                                    command_context->PrepareQueriesAfterGpuCompletion(_batch.context);
                                if (query_result != VK_SUCCESS) {
                                    completion_result = query_result;
                                    batch_gpu_success = false;
                                    batch_release_safe = false;
                                    break;
                                }
                            }
                            if (batch_gpu_success && vk_device.IsFaulted()) {
                                completion_result =
                                    vk_device.GetFirstFaultResult();
                                batch_gpu_success = false;
                                batch_release_safe = false;
                            }

                            if (!batch_gpu_success && _batch.owns_queue_timeline) {
                                timeline->Fail(completion_result);
                            }
                        } else {
                            // Wait failed or the device faulted; read the recorded error.
                            completion_result = timeline->QueryGpuCompletion(event_timeline);
                            if (_batch.owns_queue_timeline) {
                                timeline->Fail(completion_result);
                            }
                        }
                    } else if (_batch.owns_queue_timeline) {
                        if (recoverable_rejection) {
                            timeline->Reject(event_timeline);
                        } else {
                            timeline->Fail(completion_result);
                        }
                    }

                    for (const SignalEvent& event : _batch.submit.signal_events) {
                        auto* fence =
                            reinterpret_cast<VulkanTimeline*>(event.timeline_handle);
                        if (fence == nullptr) {
                            continue;
                        }
                        if (batch_gpu_success) {
                            fence->RecordGpuCompletion(event.value);
                        } else if (recoverable_rejection) {
                            fence->Reject(event.value);
                        } else {
                            fence->Fail(completion_result);
                        }
                    }

                    const QueryPublishBatch query_batch =
                        _batch.submit.query_publish_batch.Valid() ?
                            _batch.submit.query_publish_batch :
                            QueryBackendAccess::BeginPublishBatch();
                    const std::string_view query_failure_reason =
                        batch_gpu_success ?
                            "Vulkan query did not resolve after GPU completion" :
                            "Vulkan submission did not reach successful GPU completion";
                    for (auto& command_context : _batch.command_contexts.submitted) {
                        command_context->PublishQueriesAfterGpuCompletion(
                            query_batch, batch_gpu_success, query_failure_reason
                        );
                    }
                    for (auto& command_context : _batch.command_contexts.abandoned) {
                        command_context->PublishQueriesAfterGpuCompletion(
                            query_batch, false, "Vulkan query recorder was abandoned before native submission"
                        );
                    }
                    // Every CmdSubmit token must become terminal even if a
                    // backend bug failed to associate it with a native
                    // command context record. Publish the fallback in the same
                    // transaction, but defer all notifications.
                    _batch.submit.PublishPendingQueryErrors(
                        query_failure_reason, query_batch
                    );

                    const GpuCompletionPublishBatch
                        gpu_completion_batch =
                            _batch.submit.
                                    gpu_completion_publish_batch.Valid() ?
                                _batch.submit.
                                    gpu_completion_publish_batch :
                                GpuCompletionBackendAccess::
                                    BeginPublishBatch();
                    _batch.submit.PublishPendingGpuCompletionOutcome(
                        batch_gpu_success,
                        "Vulkan submission did not reach successful GPU completion",
                        gpu_completion_batch
                    );

                    bool recycle_batch =
                        batch_gpu_success && !vk_device.IsFaulted();
                    if (recycle_batch) {
                        for (auto& command_context : _batch.command_contexts.submitted) {
                            recycle_batch = command_context->CompleteSuccessCallbacks() && recycle_batch;
                        }
                    }

                    if (_batch.descriptor_lease) {
                        // A failed completion wait does not prove the GPU stopped
                        // reading. Keep those ranges unavailable until heap teardown.
                        if (!_batch.gpu_submitted || batch_gpu_success) {
                            vk_device.GetGlobalDescriptorHeap().RecycleDescriptorBufferLease(
                                std::move(_batch.descriptor_lease)
                            );
                        }
                        _batch.descriptor_lease.reset();
                    }

                    if (!_batch.gpu_submitted) {
                        FinalizeRejectedBindlessUpdates(_batch.submit);
                    }
                    Array<std::function<void()>> callbacks{};
                    Array<std::function<void()>> success_callbacks{};
                    std::optional<
                        VulkanBatchCompletionGroup::Participant>
                        grouped_completion{};
                    if (_batch.batch_completion.Valid()) {
                        grouped_completion.emplace();
                        grouped_completion->lifetime = std::move(_batch.submit.lifetime);
                        grouped_completion->gpu_completion_tokens =
                            std::move(
                                _batch.submit.gpu_completion_tokens
                            );
                        grouped_completion->gpu_completion_batch =
                            gpu_completion_batch;
                        grouped_completion->query_tokens =
                            std::move(_batch.submit.query_tokens);
                        grouped_completion->query_batch = query_batch;
                        grouped_completion->callbacks =
                            std::move(_batch.submit.callbacks);
                        grouped_completion->success_callbacks =
                            std::move(_batch.submit.success_callbacks);
                        grouped_completion->deferred_releases =
                            std::move(_batch.deferred_releases);
                        grouped_completion->device       = &vk_device;
                        grouped_completion->gpu_success  =
                            batch_gpu_success;
                        grouped_completion->release_safe =
                            batch_release_safe;
                        _batch.submit.gpu_completion_publish_batch =
                            {};
                        _batch.submit.query_publish_batch = {};
                    } else {
                        callbacks =
                            std::move(_batch.submit.callbacks);
                        success_callbacks =
                            std::move(_batch.submit.success_callbacks);
                    }
                    _batch.submit.cmds.clear();
                    _batch.submit.cached_args.clear();
                    _batch.submit.segments.clear();
                    _batch.submit.wait_events.clear();
                    _batch.submit.signal_events.clear();
                    _batch.submit.debug_label.clear();

                    if (!grouped_completion.has_value()) {
                        _batch.submit.NotifyPendingGpuCompletions(
                            gpu_completion_batch
                        );
                        _batch.submit.NotifyPendingQueries(query_batch);
                        for (auto& command_context : _batch.command_contexts.submitted) {
                            command_context->NotifyQueriesAfterGpuCompletion(query_batch);
                        }
                        for (auto& command_context : _batch.command_contexts.abandoned) {
                            command_context->NotifyQueriesAfterGpuCompletion(query_batch);
                        }
                        _batch.submit.query_tokens.clear();
                    }

                    if (recycle_batch) {
                        for (auto& command_context : _batch.command_contexts.submitted) {
                            recycle_batch = command_context->Reset() && recycle_batch;
                        }
                    }
                    if (recycle_batch && !vk_device.IsFaulted()) {
                        for (auto& command_context : _batch.command_contexts.submitted) {
                            command_contexts.Push(command_context.release());
                        }
                    } else {
                        for (auto& command_context : _batch.command_contexts.submitted) {
                            if (!command_context) {
                                continue;
                            }
                            vk_device.RecordAllocatorQuarantine();
                            vk_device.RecordSkippedCommandPoolReset();
                            command_context_quarantine.emplace_back(std::move(command_context));
                        }
                    }
                    for (auto& command_context : _batch.command_contexts.abandoned) {
                        if (!vk_device.IsFaulted() && command_context->ResetAbandoned()) {
                            command_contexts.Push(command_context.release());
                        } else if (command_context) {
                            vk_device.RecordAllocatorQuarantine();
                            vk_device.RecordSkippedCommandPoolReset();
                            command_context_quarantine.emplace_back(std::move(command_context));
                        }
                    }

                    if (grouped_completion.has_value()) {
                        VulkanBatchCompletionTicket ticket =
                            std::move(_batch.batch_completion);
                        ticket.group->Arrive(
                            ticket.participant_index,
                            std::move(*grouped_completion)
                        );
                    } else {
                        InvokeCallbacksNoexcept(
                            callbacks, "[VulkanQueue] runtime completion"
                        );
                        if (batch_gpu_success) {
                            InvokeCallbacksNoexcept(
                                success_callbacks,
                                "[VulkanQueue] runtime success completion"
                            );
                        }

                        if (batch_gpu_success || batch_release_safe) {
                            for (auto* resource : _batch.deferred_releases) {
                                MoerDelete(resource);
                            }
                        } else {
                            for (auto* resource : _batch.deferred_releases) {
                                vk_device.EnqueueDeferredRelease(resource);
                            }
                        }
                    }
                    _batch.deferred_releases.clear();
                    callbacks.clear();
                    success_callbacks.clear();
                    // CompleteAll may permit device teardown as soon as the
                    // retirement serial is published below.
                    _batch.submit.lifetime.reset();
                },
                [this, &batch_gpu_success](UniquePtr<VulkanCommandContext>& _command_context) {
                    if (batch_gpu_success && !vk_device.IsFaulted()) {
                        if (_command_context->CompleteSuccess() && _command_context->Reset()) {
                            command_contexts.Push(_command_context.release());
                            return;
                        }
                    }
                    vk_device.RecordAllocatorQuarantine();
                    vk_device.RecordSkippedCommandPoolReset();
                    command_context_quarantine.emplace_back(std::move(_command_context));
                },
                [this, &batch_gpu_success](VulkanCommandContextBatch& _batch) {
                    bool recycle_batch = batch_gpu_success && !vk_device.IsFaulted();
                    if (recycle_batch) {
                        for (auto& command_context : _batch.submitted) {
                            recycle_batch = command_context->CompleteSuccess() && recycle_batch;
                        }
                    }
                    if (recycle_batch) {
                        for (auto& command_context : _batch.submitted) {
                            recycle_batch = command_context->Reset() && recycle_batch;
                        }
                    }

                    if (recycle_batch && !vk_device.IsFaulted()) {
                        for (auto& command_context : _batch.submitted) {
                            command_contexts.Push(command_context.release());
                        }
                    } else {
                        for (auto& command_context : _batch.submitted) {
                            if (command_context) {
                                vk_device.RecordAllocatorQuarantine();
                                vk_device.RecordSkippedCommandPoolReset();
                                command_context_quarantine.emplace_back(std::move(command_context));
                            }
                        }
                    }

                    for (auto& command_context : _batch.abandoned) {
                        if (!vk_device.IsFaulted() && command_context->ResetAbandoned()) {
                            command_contexts.Push(command_context.release());
                        } else if (command_context) {
                            vk_device.RecordAllocatorQuarantine();
                            vk_device.RecordSkippedCommandPoolReset();
                            command_context_quarantine.emplace_back(std::move(command_context));
                        }
                    }
                },
                [this, &batch_gpu_success, &batch_release_safe](VulkanDescriptorBufferLease& _lease) {
                    if (batch_gpu_success || batch_release_safe) {
                        vk_device.GetGlobalDescriptorHeap().RecycleDescriptorBufferLease(std::move(_lease));
                    }
                },

                [&batch_gpu_success](VulkanCallbackBatch& _batch) {
                    if (!_batch.success_only || batch_gpu_success) {
                        InvokeCallbacksNoexcept(
                            _batch.callbacks, "[VulkanQueue] completion"
                        );
                    }
                },
                [this, &batch_gpu_success, &batch_release_safe](VulkanDeferredReleaseBatch& _batch) {
                    if (batch_gpu_success || batch_release_safe) {
                        for (auto* resource : _batch.resources) {
                            MoerDelete(resource);
                        }
                        return;
                    }
                    for (auto* resource : _batch.resources) {
                        vk_device.EnqueueDeferredRelease(resource);
                    }
                },
                [](VulkanCompletionMarker&) {},
                [this, &batch_gpu_success](SignalEvent& _evt) {
                    auto* fence = reinterpret_cast<VulkanTimeline*>(_evt.timeline_handle);
                    if (batch_gpu_success) {
                        fence->RecordGpuCompletion(_evt.value);
                    } else if (vk_device.IsFaulted()) {
                        fence->Fail(vk_device.GetFirstFaultResult());
                    }
                },
                [](WaitEvent&) {},
                [](VulkanTimeline*) {
                    assert(false && "Invalid event");
                }
            },
            evt->event
        );

        if (evt->wake_thread) {
            uint64 completed = cpu_settled_frame.load(std::memory_order_relaxed);
            while (completed < event_timeline &&
                   !cpu_settled_frame.compare_exchange_weak(
                       completed, event_timeline, std::memory_order_release, std::memory_order_relaxed
                   )) {}
            const uint64 retirement_serial = evt->retirement_serial;
            uint64 settled_serial =
                retirement_settled_serial.load(std::memory_order_relaxed);
            while (settled_serial < retirement_serial &&
                   !retirement_settled_serial.compare_exchange_weak(
                       settled_serial,
                       retirement_serial,
                       std::memory_order_release,
                       std::memory_order_relaxed
                   )) {}
            queue_cv.notify_all();
        }
    }
}

void VkCommandQueue::CompleteAll(uint64 _timeline) {
    Array<std::shared_ptr<VulkanBatchCompletionGroup>>
        completion_groups{};
    uint64 target_retirement_serial = 0;
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        target_retirement_serial = retirement_enqueued_serial;
        for (const VulkanBatchCompletionSettlement& settlement :
             batch_completion_settlements) {
            if (settlement.retirement_serial >
                target_retirement_serial) {
                continue;
            }
            if (auto group = settlement.group.lock()) {
                completion_groups.emplace_back(std::move(group));
            }
        }
#if MOER_VULKAN_TEST_SUPPORT
        NotifyVulkanQueueLocalSyncWait(
            queue.GetType(),
            target_retirement_serial,
            completion_groups.size()
        );
#endif
        queue_cv.wait(
            lock,
            [this, _timeline, target_retirement_serial]() {
                return cpu_settled_frame.load(
                           std::memory_order_acquire
                       ) >= _timeline &&
                       retirement_settled_serial.load(
                           std::memory_order_acquire
                       ) >= target_retirement_serial;
            }
        );
    }

    // A grouped packet can finish its queue-local retirement before the last
    // sibling owner releases the batch-wide Query/callback/deferred-release
    // tiers. External queue Sync must include that release, while Completion
    // owners remain strictly non-blocking.
    for (const auto& group : completion_groups) {
        group->WaitUntilSettled();
    }

    std::unique_lock<std::mutex> lock(event_mutex);
    std::erase_if(
        batch_completion_settlements,
        [target_retirement_serial](
            const VulkanBatchCompletionSettlement& settlement
        ) {
            return settlement.retirement_serial <=
                   target_retirement_serial;
        }
    );
}

#pragma endregion

} // namespace Moer::Render
