#include "VulkanQueue.h"
#include "PixelFormat.h"
#include "RHICmdReorderer.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanParallelRecordWorkEstimator.h"
#include "VulkanRHIResource.h"
#include "VulkanSerialGolden.h"
#include "VulkanSubmissionDiagnostics.h"
#include "rhi/ExternalCpuJoinPool.h"
#include "misc/Alignment.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "misc/Traits.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIIO.h"
#include "rhi/RHIResource.h"

#include "VulkanCustomCommand.h"
#include "shader/ShaderPipeline.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
namespace Moer::Render {
namespace {

std::atomic<const VulkanNativeSubmissionObserver*>
    g_native_submission_observer{nullptr};
std::atomic<const VulkanSubmissionDependencyWaitObserver*>
    g_submission_dependency_wait_observer{nullptr};
std::atomic<const VulkanBackendSyncWaitObserver*>
    g_backend_sync_wait_observer{nullptr};
std::atomic<const VulkanQueueLocalSyncWaitObserver*>
    g_queue_local_sync_wait_observer{nullptr};
std::atomic<const VulkanScriptedQueryPreparationOverrideForTesting*>
    g_scripted_query_preparation_override{nullptr};
std::atomic<const VulkanScriptedPresentOverrideForTesting*>
    g_scripted_present_override{nullptr};

struct VulkanPresentSourceContract {
    bool        valid{false};
    const char* reason{"unknown present-source contract failure"};
};

void InvokePresentSourceRejectionCallbackForTesting(
    const VulkanScriptedPresentOverrideForTesting* _override
) noexcept {
    if (_override != nullptr &&
        _override->before_source_rejection != nullptr) {
        _override->before_source_rejection(_override->context);
    }
}

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

    const FormatInfo& source =
        g_platform_pixel_formats[source_index];
    const FormatInfo& destination =
        g_platform_pixel_formats[destination_index];
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

void NotifyNativeSubmission(
    EQueueType                   _queue,
    VkQueue                      _native_queue,
    const VulkanOperationResult& _outcome,
    bool                         _empty_submit
) noexcept {
    const VulkanNativeSubmissionObserver* observer =
        g_native_submission_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanNativeSubmissionEvent{
            .queue               = _queue,
            .native_queue_handle = reinterpret_cast<uint64>(_native_queue),
            .thread_id           = Platform::GetCurrentThreadID(),
            .thread_role         = GetCurrentRHIThreadRole(),
            .outcome             = _outcome,
            .empty_submit        = _empty_submit,
        }
    );
}

void NotifyQueueLocalSyncWait(
    EQueueType _queue,
    uint64     _target_retirement_serial,
    size_t     _completion_group_count
) noexcept {
    const VulkanQueueLocalSyncWaitObserver* observer =
        g_queue_local_sync_wait_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanQueueLocalSyncWaitEvent{
            .queue       = _queue,
            .thread_id   = Platform::GetCurrentThreadID(),
            .thread_role = GetCurrentRHIThreadRole(),
            .target_retirement_serial =
                _target_retirement_serial,
            .completion_group_count = static_cast<uint32>(
                std::min(
                    _completion_group_count,
                    static_cast<size_t>(
                        std::numeric_limits<uint32>::max()
                    )
                )
            ),
        }
    );
}

} // namespace

void NotifyVulkanSubmissionDependencyWaitBlocked(
    EQueueType _queue,
    uint32     _dependency_count
) noexcept {
    const VulkanSubmissionDependencyWaitObserver* observer =
        g_submission_dependency_wait_observer.load(
            std::memory_order_acquire
        );
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanSubmissionDependencyWaitEvent{
            .queue            = _queue,
            .thread_id        = Platform::GetCurrentThreadID(),
            .thread_role      = GetCurrentRHIThreadRole(),
            .dependency_count = _dependency_count,
        }
    );
}

void NotifyVulkanBackendSyncWait() noexcept {
    const VulkanBackendSyncWaitObserver* observer =
        g_backend_sync_wait_observer.load(std::memory_order_acquire);
    if (observer == nullptr) {
        return;
    }
    observer->callback(
        observer->context,
        VulkanBackendSyncWaitEvent{
            .thread_id   = Platform::GetCurrentThreadID(),
            .thread_role = GetCurrentRHIThreadRole(),
        }
    );
}

bool TryInstallVulkanNativeSubmissionObserver(
    const VulkanNativeSubmissionObserver* _observer
) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanNativeSubmissionObserver* expected = nullptr;
    return g_native_submission_observer.compare_exchange_strong(
        expected,
        _observer,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanNativeSubmissionObserver(
    const VulkanNativeSubmissionObserver* _observer
) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanNativeSubmissionObserver* expected = _observer;
    return g_native_submission_observer.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool TryInstallVulkanScriptedQueryPreparationOverrideForTesting(
    const VulkanScriptedQueryPreparationOverrideForTesting* _override
) noexcept {
    if (_override == nullptr || _override->callback == nullptr) {
        return false;
    }
    const VulkanScriptedQueryPreparationOverrideForTesting* expected =
        nullptr;
    return g_scripted_query_preparation_override.compare_exchange_strong(
        expected,
        _override,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanScriptedQueryPreparationOverrideForTesting(
    const VulkanScriptedQueryPreparationOverrideForTesting* _override
) noexcept {
    if (_override == nullptr) {
        return false;
    }
    const VulkanScriptedQueryPreparationOverrideForTesting* expected =
        _override;
    return g_scripted_query_preparation_override.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool TryInstallVulkanScriptedPresentOverrideForTesting(
    const VulkanScriptedPresentOverrideForTesting* _override
) noexcept {
    if (_override == nullptr || _override->callback == nullptr) {
        return false;
    }
    const VulkanScriptedPresentOverrideForTesting* expected = nullptr;
    return g_scripted_present_override.compare_exchange_strong(
        expected,
        _override,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanScriptedPresentOverrideForTesting(
    const VulkanScriptedPresentOverrideForTesting* _override
) noexcept {
    if (_override == nullptr) {
        return false;
    }
    const VulkanScriptedPresentOverrideForTesting* expected = _override;
    return g_scripted_present_override.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool TryLatchVulkanDeviceFaultForTesting(VkResult _result) noexcept {
    try {
        auto* device = static_cast<VulkanDevice*>(
            RenderDevice::Get().GetImpl()
        );
        if (device == nullptr) {
            return false;
        }
        const VulkanOperationContext context{
            .operation  = EVulkanFaultOperation::PresentSubmit,
            .queue_type = EQueueType::Graphics,
        };
        return device->TryLatchFirstFault(
                   context, _result, true, false, true
               ) ||
               device->IsFaulted();
    } catch (...) {
        return false;
    }
}

bool TryInstallVulkanSubmissionDependencyWaitObserver(
    const VulkanSubmissionDependencyWaitObserver* _observer
) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanSubmissionDependencyWaitObserver* expected = nullptr;
    return g_submission_dependency_wait_observer.compare_exchange_strong(
        expected,
        _observer,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanSubmissionDependencyWaitObserver(
    const VulkanSubmissionDependencyWaitObserver* _observer
) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanSubmissionDependencyWaitObserver* expected = _observer;
    return g_submission_dependency_wait_observer.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool TryInstallVulkanBackendSyncWaitObserver(
    const VulkanBackendSyncWaitObserver* _observer
) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanBackendSyncWaitObserver* expected = nullptr;
    return g_backend_sync_wait_observer.compare_exchange_strong(
        expected,
        _observer,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanBackendSyncWaitObserver(
    const VulkanBackendSyncWaitObserver* _observer
) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanBackendSyncWaitObserver* expected = _observer;
    return g_backend_sync_wait_observer.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

bool TryInstallVulkanQueueLocalSyncWaitObserver(
    const VulkanQueueLocalSyncWaitObserver* _observer
) noexcept {
    if (_observer == nullptr || _observer->callback == nullptr) {
        return false;
    }
    const VulkanQueueLocalSyncWaitObserver* expected = nullptr;
    return g_queue_local_sync_wait_observer.compare_exchange_strong(
        expected,
        _observer,
        std::memory_order_release,
        std::memory_order_relaxed
    );
}

bool RemoveVulkanQueueLocalSyncWaitObserver(
    const VulkanQueueLocalSyncWaitObserver* _observer
) noexcept {
    if (_observer == nullptr) {
        return false;
    }
    const VulkanQueueLocalSyncWaitObserver* expected = _observer;
    return g_queue_local_sync_wait_observer.compare_exchange_strong(
        expected,
        nullptr,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
}

#pragma region[ utils ]

VkRenderingAttachmentInfo FromColorAttachmentInfo(const ColorAttachment& _attachment) {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
    attachment_info.imageView = vk_texture->GetView(
        static_cast<uint8>(_attachment.mip_level), 1, static_cast<uint8>(_attachment.array_layer), 1
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

    auto* vk_pso = reinterpret_cast<VulkanPipelineState*>(pipeline.handle);
    return vk_pso->UsesStencilAttachment();
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
    attachment_info.imageView = vk_texture->GetView(
        static_cast<uint8>(_attachment.mip_level), 1, static_cast<uint8>(_attachment.array_layer), 1
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

static bool IsBufferTextureWrite(VulkanShaderResourceState _state) {
    switch (_state.resource_type) {
        case SRT_UAV:
            return true;
        default:
            return false;
    }
}

static bool IsBufferTextureWrite(uint64 _flags) {
    return IsBufferTextureWrite(VulkanShaderResourceState(_flags));
}

static bool IsTextureSampled(uint64 _flags) {
    VulkanShaderResourceState state(_flags);
    return state.b_sampled;
}

static bool IsResourceInBindlessArray(uint64 _res, uint64 _bdls_handle) {
    VulkanBindlessArray* bindless_array = reinterpret_cast<VulkanBindlessArray*>(_bdls_handle);
    return bindless_array->IsResourceAllocated(_res);
}

static bool IsBufferTextureRead(uint64 _flags) {
    VulkanShaderResourceState state(_flags);
    switch (state.resource_type) {
        case SRT_SRV:
            return true;
        default:
            return false;
    }
}

static void LockBindlessArray(uint64 _handle) {
    VulkanBindlessArray* bdls_array = reinterpret_cast<VulkanBindlessArray*>(_handle);
    bdls_array->Lock();
}

static void UnlockBindlessArray(uint64 _handle) {
    VulkanBindlessArray* bdls_array = reinterpret_cast<VulkanBindlessArray*>(_handle);
    bdls_array->Unlock();
}

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

static void FinalizeRejectedBindlessUpdates(const CmdSubmit& _submit) {
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

static void InvokeCallbacksNoexcept(
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

    // Every participant has already published its Fence and Query terminal
    // state. Release Query callbacks first across the whole batch, then the
    // two ordinary callback tiers, and keep resources alive until all user
    // code has returned. The last arriver is itself a Completion owner.
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

static void ResolvePresentReceiptNoexcept(
    const PresentReceiptRef& _receipt,
    bool                     _submitted,
    bool                     _recreate_swapchain
) noexcept {
    if (!_receipt) {
        return;
    }
    try {
        _receipt->Resolve(_submitted, _recreate_swapchain);
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
        std::runtime_error(
            "stale bindless update batch cannot be submitted with dependent commands"
        ) {}
};

#pragma endregion

#pragma region[ preprocessor ]
struct VkCmdPreprocessor {
    VkTracker&       tracker;
    FunctionTable    m_funcs;
    VulkanAllocator& allocator;
    VulkanDevice&    device;
    EQueueType       current_queue;

    UnorderedSet<uint64> writed_buffer_resources;
    UnorderedSet<TextureSubresourceKeyT<VulkanTexture>, TextureSubresourceKeyHashT<VulkanTexture>>
                           writed_texture_resources;
    const TCachedArgArray& cached_args;

    VkCmdPreprocessor(
        VulkanDevice&          _device,
        VkTracker&             _tracker,
        VulkanAllocator&       _allocator,
        FunctionTable          _funcs,
        const TCachedArgArray& _cached_args,
        EQueueType             _current_queue = EQueueType::Graphics
    ) :
        device(_device),
        tracker(_tracker),
        allocator(_allocator),
        m_funcs(_funcs),
        cached_args(_cached_args),
        current_queue(_current_queue) {}

    bool OverlapsCurrentTextureWrite(
        const TextureSubresourceKeyT<VulkanTexture>& _candidate
    ) const {
        return std::any_of(
            writed_texture_resources.begin(),
            writed_texture_resources.end(),
            [&](const TextureSubresourceKeyT<VulkanTexture>& written) {
                return TextureSubresourceRangesOverlap(
                    written, _candidate
                );
            }
        );
    }

    void HandleBindless(BindlessArrayRef _bindless_array, VkPipelineStageFlagBits2 _pipeline_stages) {
        EPassType pass_type = EPassType::Graphics;
        if (_pipeline_stages == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT) {
            pass_type = EPassType::Compute;
        } else if (_pipeline_stages == VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR) {
            pass_type = EPassType::Raytracing;
        }
        if (!_bindless_array) {
            return;
        }
        auto* vk_bindless_array = reinterpret_cast<VulkanBindlessArray*>(_bindless_array.Get());

        Moer::Array<TextureSubresourceKeyT<VulkanTexture>> to_read_textures;
        Moer::Array<VulkanBuffer*> to_read_buffers;
        {
            // Allocation/unbind/update can run on a producer thread while the
            // RHI coordinator preprocesses this packet. Observe both maps from
            // one stable snapshot; worker recorders never access them.
            auto bindless_lock = vk_bindless_array->AcquireLock();
            for (const auto& key : tracker.GetWritedStateTextures()) {
                if (vk_bindless_array->IsTextureViewAllocated(
                        uint64(key.texture),
                        key.mip_level,
                        key.mip_count,
                        key.array_layer,
                        key.array_count
                    ) &&
                    !OverlapsCurrentTextureWrite(key)) {
                    to_read_textures.push_back(key);
                }
            }
            for (const auto& buffer : tracker.GetWritedStateBuffers()) {
                if (vk_bindless_array->IsResourceAllocated(uint64(buffer)) &&
                    !writed_buffer_resources.contains(uint64(buffer))) {
                    to_read_buffers.push_back(buffer);
                }
            }
        }
        if (!to_read_textures.empty()) {
            for (const auto& key : to_read_textures) {
                auto access = tracker.ReadTexture(key.texture, ETextureState::SAMPLE, pass_type);
                tracker.RecordState(
                    key.texture,
                    std::get<0>(access),
                    std::get<1>(access),
                    std::get<2>(access),
                    key.mip_level,
                    key.mip_count,
                    key.array_layer,
                    key.array_count
                );
            }
        }
        if (!to_read_buffers.empty()) {
            for (const auto& i : to_read_buffers) {
                tracker.RecordState(i, VK_ACCESS_2_SHADER_READ_BIT, _pipeline_stages);
            }
        }
        tracker.RecordState(
            vk_bindless_array->bindless_texture_descs,
            VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT,
            _pipeline_stages
        );
        tracker.RecordState(
            vk_bindless_array->bindless_buffer_descs,
            VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT,
            _pipeline_stages
        );
        tracker.RecordState(
            vk_bindless_array->bindless_array_buffer, VK_ACCESS_2_SHADER_READ_BIT, _pipeline_stages
        );
    }

    static VkAccessFlagBits2 GetBufferAccess(VulkanShaderResourceState _flag) {
        switch (_flag.resource_type) {
            case SRT_CBV:
                return VK_ACCESS_2_SHADER_READ_BIT;
            case SRT_SRV:
                return VK_ACCESS_2_SHADER_READ_BIT;
            case SRT_UAV:
                return VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
            default:
                return VK_ACCESS_2_SHADER_READ_BIT;
        }
    }

    static VkAccessFlagBits2 GetTextureAccess(VulkanShaderResourceState _flag) {
        switch (_flag.resource_type) {
            // case SRT_SAMPLER:
            //     return VK_ACCESS_2_SHADER_READ_BIT;
            case SRT_SRV:
                return VK_ACCESS_2_SHADER_READ_BIT;
            case SRT_UAV:
                return VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
            default:
                assert(false && "Invalid texture resource type");
                return VK_ACCESS_2_SHADER_READ_BIT;
        }
    }

    static VkImageLayout GetTextureLayout(VulkanShaderResourceState _flag) {
        switch (_flag.resource_type) {
            // case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SAMPLER:
            //     return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            case SRT_SRV: {
                if (_flag.b_sampled)
                    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                return VK_IMAGE_LAYOUT_GENERAL;
            }

            case SRT_UAV:
                return VK_IMAGE_LAYOUT_GENERAL;
            default:
                assert(false && "Invalid texture resource type");
                return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    void VisitArgs(const TArg& _arg, VulkanShaderResourceState _flag, VkPipelineStageFlagBits2 _pipelines) {
        if (_pipelines == VK_PIPELINE_STAGE_2_NONE)
            return;
        std::visit(
            [&](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, BufferView>) {
                    if (_flag.resource_type == SRT_INVALID)
                        return;
                    auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_arg.GetBuffer());
                    tracker.RecordState(vk_buffer, GetBufferAccess(_flag), _pipelines);
                    if (IsBufferTextureWrite(_flag)) {
                        writed_buffer_resources.insert(uint64(vk_buffer));
                    }
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    if (_flag.resource_type == SRT_INVALID)
                        return;
                    auto* vk_texture = ResourceCast(_arg.GetTexture());
                    tracker.RecordState(
                        vk_texture,
                        GetTextureAccess(_flag),
                        GetTextureLayout(_flag),
                        _pipelines,
                        _arg.mip_level,
                        _arg.num_mips,
                        _arg.array_layer,
                        _arg.num_array
                    );
                    if (IsBufferTextureWrite(_flag)) {
                        ValidateSubresourceRange(
                            _arg.texture, _arg.mip_level, _arg.num_mips, _arg.array_layer, _arg.num_array
                        );
                        TextureSubresourceKeyT<VulkanTexture> key{
                            vk_texture, _arg.mip_level, _arg.num_mips, _arg.array_layer, _arg.num_array
                        };
                        writed_texture_resources.insert(key);
                    }
                } else if constexpr (std::is_same_v<T, TextureViewArray>) {
                    for (auto&& i : _arg) {
                        VisitArgs(i, _flag, _pipelines);
                    }
                } else if constexpr (std::is_same_v<T, BufferViewArray>) {
                    for (auto&& i : _arg) {
                        VisitArgs(i, _flag, _pipelines);
                    }
                }

                else if constexpr (std::is_same_v<T, BindlessArrayRef>) {
                    // HandleBindless(_arg, _pipelines);
                    assert(false && "Bindless array not supported");
                } else if constexpr (std::is_same_v<T, RaytracingTlasRef>) {
                    VulkanAccelerationStructure* vk_as = ResourceCast(_arg.Get());
                    tracker.RecordState(
                        vk_as->underlying_buffer, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, _pipelines
                    );
                }
            },
            _arg
        );
    }

    bool VisitCmd(const Command* _cmd) {
        assert(_cmd != nullptr);
        switch (_cmd->Type()) {
            case Command::EType::UploadBuffer:
                Visit(static_cast<const UploadBufferCmd*>(_cmd));
                break;
            case Command::EType::UploadTexture:
                Visit(static_cast<const UploadTextureCmd*>(_cmd));
                break;
            case Command::EType::BufferToBuffer:
                Visit(static_cast<const CopyBufferCmd*>(_cmd));
                break;
            case Command::EType::BufferToTexture:
                Visit(static_cast<const CopyBufferToTextureCmd*>(_cmd));
                break;
            case Command::EType::TextureToBuffer:
                Visit(static_cast<const CopyTextureToBufferCmd*>(_cmd));
                break;
            case Command::EType::TextureToTexture:
                Visit(static_cast<const CopyTextureCmd*>(_cmd));
                break;
            case Command::EType::CopyBackBuffer:
                Visit(static_cast<const CopyBackBufferCmd*>(_cmd));
                break;
            case Command::EType::CopyBackTexture:
                Visit(static_cast<const CopyBackTextureCmd*>(_cmd));
                break;
            case Command::EType::ShaderDispatch:
                Visit(static_cast<const DispatchCmd*>(_cmd));
                break;
            case Command::EType::SetDrawState:
                Visit(static_cast<const SetDrawStateCmd*>(_cmd));
                break;
            case Command::EType::MultiDraw:
                Visit(static_cast<const MultiDrawCmd*>(_cmd));
                break;
            // case Command::EType::SetGeometryPassDrawState:
            //     Visit(static_cast<const SetGeometryPassDrawStateCmd*>(_cmd));
            //     break;
            case Command::EType::BuildAccel:
                Visit(static_cast<const BuildAccelerationStructuresCmd*>(_cmd));
                break;
            case Command::EType::BuildTLAS:
                Visit(static_cast<const UpdateRaytracingSceneCmd*>(_cmd));
                break;
            case Command::EType::Barrier:
                Visit(static_cast<const BarrierCmd*>(_cmd));
                break;
            case Command::EType::QueueTransfer:
                Visit(static_cast<const QueueTransferCmd*>(_cmd));
                break;
            case Command::EType::UpdateBindlessArray:
                Visit(static_cast<const UpdateBindlessArrayCmd*>(_cmd));
                break;

            case Command::EType::ClearResource:
                Visit(static_cast<const ClearResourceCmd*>(_cmd));
                break;
            case Command::EType::Custom:
                Visit(static_cast<const CustomCmd*>(_cmd));
                break;
            case Command::EType::Query:
            case Command::EType::Scope:
                // Query/marker commands have no resource-state dependency.
                break;
            default:
                assert(false && "Invalid command type");
        }
        return false;
    }

    void Visit(const UploadBufferCmd* _cmd) {
        auto data_span  = _cmd->Data();
        auto tmp_buffer = allocator.AllocateUploadBuffer(_cmd->ByteSize(), 16);
        device.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
        _cmd->staging_buffer = tmp_buffer;

        auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->Handle());
        tracker.RecordState(vk_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        //Register Inner Buffer Ranges for queue execution sync
        tracker.RegisterFlushBufferRange(
            _cmd->staging_buffer,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT
        );
    }
    void Visit(const UploadTextureCmd* _cmd) {
        auto data_span  = _cmd->Data();
        auto tmp_buffer = allocator.AllocateUploadBuffer(data_span.size_bytes(), 16);
        device.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
        _cmd->staging_buffer = tmp_buffer;

        auto* vk_texture = reinterpret_cast<VulkanTexture*>(_cmd->Handle());
        tracker.RecordState(
            vk_texture,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            _cmd->MipLevel(),
            1,                 // mip_count
            _cmd->array_layer, // array_layer
            1                  // array_count
        );
        tracker.RegisterFlushBufferRange(
            _cmd->staging_buffer,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT
        );
    }
    void Visit(const CopyBufferCmd* _cmd) {
        auto* vk_src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->SrcHandle());
        auto* vk_dst_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->DstHandle());
        tracker.RecordState(vk_src_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        tracker.RecordState(vk_dst_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    }
    void Visit(const CopyTextureCmd* _cmd) {
        auto* vk_src_texture = reinterpret_cast<VulkanTexture*>(_cmd->SrcHandle());
        auto* vk_dst_texture = reinterpret_cast<VulkanTexture*>(_cmd->DstHandle());
        tracker.RecordState(
            vk_src_texture,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            _cmd->SrcMipLevel()
        );
        tracker.RecordState(
            vk_dst_texture,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            _cmd->DstMipLevel()
        );
    }
    void Visit(const CopyBufferToTextureCmd* _cmd) {
        auto* vk_src_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->SrcHandle());
        auto* vk_dst_texture = reinterpret_cast<VulkanTexture*>(_cmd->DstHandle());
        tracker.RecordState(vk_src_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        tracker.RecordState(
            vk_dst_texture,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            _cmd->MipLevel(),
            1,                 // mip_count
            _cmd->array_layer, // array_layer
            1                  // array_count
        );
    }

    void Visit(const CopyTextureToBufferCmd* _cmd) {
        auto* vk_src_texture = reinterpret_cast<VulkanTexture*>(_cmd->SrcHandle());
        auto* vk_dst_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->DstHandle());
        tracker.RecordState(
            vk_src_texture,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            _cmd->MipLevel()
        );
        tracker.RecordState(vk_dst_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
    }

    void Visit(const CopyBackBufferCmd* _cmd) {
        auto tmp_buffer      = allocator.AllocateReadbackBuffer(_cmd->ByteSize(), 16);
        _cmd->staging_buffer = tmp_buffer;

        auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->Handle());
        tracker.RecordState(vk_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        tracker.RegisterFlushBufferRange(
            _cmd->staging_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT
        );
    }

    void Visit(const CopyBackTextureCmd* _cmd) {
        auto tmp_buffer      = allocator.AllocateReadbackBuffer(_cmd->Data().size_bytes(), 16);
        _cmd->staging_buffer = tmp_buffer;

        auto* vk_texture = reinterpret_cast<VulkanTexture*>(_cmd->Handle());
        tracker.RecordState(
            vk_texture,
            VK_ACCESS_2_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            _cmd->MipLevel()
        );
        tracker.RegisterFlushBufferRange(
            _cmd->staging_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT
        );
    }

    void Visit(const DispatchCmd* _cmd) {
        std::visit(
            [this](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, uint3>) {
                    return;
                } else if constexpr (std::is_same_v<T, BufferView>) {
                    tracker.RecordState(
                        reinterpret_cast<VulkanBuffer*>(_arg.GetBuffer()),
                        VK_ACCESS_2_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
                    );
                }
            },
            _cmd->Param()
        );

        const auto& pipeline = _cmd->Pipeline();

        writed_buffer_resources.clear();
        writed_texture_resources.clear();

        auto func = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx))
                VisitArgs(
                    _arg,
                    pipeline.binding_infos[_idx].state_flags,
                    pipeline.binding_infos[_idx].pipeline_flags
                );
        };
        auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx))
                HandleBindless(std::get<BindlessArrayRef>(_arg), pipeline.binding_infos[_idx].pipeline_flags);
        };

        IterateArgs(_cmd->Args(cached_args), func, bdls_post_func);

        // auto func = [&](const TArg& _arg, ParamInfoFlags _flag) {
        //     VisitArgs(_arg, _flag.state_flags, _flag.pipeline_flags);
        // };
        // _cmd->IterateArgs(func);
    }

    // void Visit(const SetParamsCmd* _cmd) {
    // }
    // void Visit(const SetConstantCmd* _cmd) {
    // }
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

            scratch = allocator.AllocateScratch(scratch_size);
        }
        tracker.RecordState(
            ResourceCast(scratch.GetBuffer()),
            {VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR,
             VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
        );
        for (const AccelerationStructureBuildParam& param : params) {
            auto* vk_geo    = ResourceCast(param.geometry.Get());
            auto* vk_buffer = vk_geo->GetUnderlyingBuffer();

            tracker.RecordState(
                vk_buffer,
                {VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
            );

            tracker.EmplaceWriteBLAS(uint64(vk_geo));
        }

        for (auto& vtx : _cmd->VtxBuffers()) {
            auto* vk_buffer = ResourceCast(vtx);
            tracker.RecordState(
                vk_buffer,
                {VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
            );
        }

        for (auto& idx : _cmd->IdxBuffers()) {
            auto* idx_buffer = ResourceCast(idx);
            tracker.RecordState(
                idx_buffer,
                {VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
            );
        }
    }

    void Visit(const UpdateRaytracingSceneCmd* _cmd) {
        if (_cmd->InstancesToUpdate().empty() && !_cmd->ForceUpdate()) {
            return;
        }
        //instance buffer
        VulkanBuffer* instance_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->InstanceBufferHandle());
        VulkanBuffer* scratch_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->ScratchBufferHandle());
        VulkanAccelerationStructure* tlas =
            reinterpret_cast<VulkanAccelerationStructure*>(_cmd->TlasHandle());

        if (_cmd->ForceUpdate() && _cmd->InstancesToUpdate().empty()) {
            tracker.RecordState(
                instance_buffer,
                {VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                 VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
            );
        } else {
            tracker.RecordState(
                instance_buffer, {VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT}
            );
        }
        // tracker.RecordState(instance_buffer, {VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT});
        tracker.RecordState(
            scratch_buffer,
            {VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
                 VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
             VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
        );
        tracker.RecordState(
            tlas->underlying_buffer,
            {VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR,
             VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
        );

        for (const uint64& handle : tracker.GetWriteBLASStates()) {
            if (_cmd->HasGeometry(handle)) {
                VulkanRaytracingGeometry* vk_geo = reinterpret_cast<VulkanRaytracingGeometry*>(handle);

                tracker.RecordState(
                    vk_geo->GetUnderlyingBuffer(),
                    {VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                     VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR}
                );
            }
        }
    }

    void Visit(const BarrierCmd* _cmd) {
        if (_cmd->HasExplicitBarriers()) {
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
                        return families;
                    }
                    if (transfer.phase != EBarrierQueueTransferPhase::Release &&
                        transfer.phase != EBarrierQueueTransferPhase::Acquire) {
                        throw std::invalid_argument(
                            "Vulkan ownership barrier carries an unknown phase"
                        );
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
                tracker.EmitExplicitBarrier(
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
                tracker.EmitExplicitBarrier(
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
            return;
        }

        if (!_cmd->IsQueueTransition()) {

            for (auto& barrier : _cmd->ReadBuffers()) {
                auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
                tracker.RecordState(
                    vk_buffer, tracker.ReadBuffer(vk_buffer, barrier.state, barrier.pass_type)
                );
            }
            for (auto& barrier : _cmd->WriteBuffers()) {
                auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
                tracker.RecordState(
                    vk_buffer, tracker.WriteBuffer(vk_buffer, barrier.state, barrier.pass_type)
                );
            }

            for (auto& barrier : _cmd->ReadTextures()) {
                auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                auto  access     = tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type);
                tracker.RecordState(
                    vk_texture,
                    std::get<0>(access),
                    std::get<1>(access),
                    std::get<2>(access),
                    static_cast<uint8>(barrier.mip_level),
                    static_cast<uint8>(barrier.mip_cnt),
                    static_cast<uint8>(barrier.array_layer),
                    static_cast<uint8>(barrier.array_cnt)
                );
            }
            for (auto& barrier : _cmd->WriteTextures()) {
                auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                auto  access     = tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type);
                tracker.RecordState(
                    vk_texture,
                    std::get<0>(access),
                    std::get<1>(access),
                    std::get<2>(access),
                    static_cast<uint8>(barrier.mip_level),
                    static_cast<uint8>(barrier.mip_cnt),
                    static_cast<uint8>(barrier.array_layer),
                    static_cast<uint8>(barrier.array_cnt)
                );
            }

            return;
        }

        uint src_queue_family = VK_QUEUE_FAMILY_IGNORED;
        uint dst_queue_family = VK_QUEUE_FAMILY_IGNORED;

        auto get_queue_idx = [&](EQueueType _type) {
            switch (_type) {
                case EQueueType::Graphics:
                    return device.GetQueueFamilyIndex(VK_QUEUE_GRAPHICS_BIT);
                    break;
                case EQueueType::Compute:
                    return device.GetQueueFamilyIndex(VK_QUEUE_COMPUTE_BIT);
                    break;
                case EQueueType::Copy:
                    return device.GetQueueFamilyIndex(VK_QUEUE_TRANSFER_BIT);
                    break;
                case EQueueType::Ignore:
                    return VK_QUEUE_FAMILY_IGNORED;
                default:
                    assert(false && "Invalid queue type");
            }
            return VK_QUEUE_FAMILY_IGNORED;
        };

        src_queue_family = get_queue_idx(_cmd->GetSrcQueue());
        dst_queue_family = get_queue_idx(_cmd->GetDstQueue());

        for (auto& barrier : _cmd->ReadBuffers()) {
            auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
            tracker.RecordState(
                vk_buffer,
                tracker.ReadBuffer(vk_buffer, barrier.state, barrier.pass_type),
                src_queue_family,
                dst_queue_family
            );
        }
        for (auto& barrier : _cmd->WriteBuffers()) {
            auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
            tracker.RecordState(
                vk_buffer,
                tracker.WriteBuffer(vk_buffer, barrier.state, barrier.pass_type),
                src_queue_family,
                dst_queue_family
            );
        }

        for (auto& barrier : _cmd->ReadTextures()) {
            auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
            auto  access     = tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type);
            tracker.RecordState(
                vk_texture,
                std::get<0>(access),
                std::get<1>(access),
                std::get<2>(access),
                static_cast<uint8>(barrier.mip_level),
                static_cast<uint8>(barrier.mip_cnt),
                static_cast<uint8>(barrier.array_layer),
                static_cast<uint8>(barrier.array_cnt),
                src_queue_family,
                dst_queue_family
            );
        }
        for (auto& barrier : _cmd->WriteTextures()) {
            auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
            auto  access     = tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type);
            tracker.RecordState(
                vk_texture,
                std::get<0>(access),
                std::get<1>(access),
                std::get<2>(access),
                static_cast<uint8>(barrier.mip_level),
                static_cast<uint8>(barrier.mip_cnt),
                static_cast<uint8>(barrier.array_layer),
                static_cast<uint8>(barrier.array_cnt),
                src_queue_family,
                dst_queue_family
            );
        }

        //queue transition
    }

    void Visit(const QueueTransferCmd* _cmd) {
        const EQueueType src_queue =
            _cmd->IsImport() ? _cmd->src_queue : current_queue;
        const EQueueType dst_queue =
            _cmd->IsImport() ? current_queue : _cmd->dst_queue;
        if (src_queue == EQueueType::Ignore || dst_queue == EQueueType::Ignore) {
            throw std::invalid_argument("queue transfer requires two concrete queues");
        }
        const uint src_queue_family = device.GetQueueFamilyIndex(src_queue);
        const uint dst_queue_family = device.GetQueueFamilyIndex(dst_queue);
        const uint current_queue_family = device.GetQueueFamilyIndex(current_queue);
        const uint expected_queue_family =
            _cmd->IsImport() ? dst_queue_family : src_queue_family;
        if (current_queue_family != expected_queue_family) {
            LOG_CRITICAL(
                "QueueTransferCmd affinity mismatch: import={} src={}({}) "
                "dst={}({}) current={}({})",
                _cmd->IsImport(),
                static_cast<uint>(src_queue),
                src_queue_family,
                static_cast<uint>(dst_queue),
                dst_queue_family,
                static_cast<uint>(current_queue),
                current_queue_family
            );
            throw std::logic_error(
                "queue transfer barrier recorded by a non-endpoint queue family"
            );
        }

        if (_cmd->IsImport()) {
            for (auto& barrier : _cmd->ImportTextures()) {
                auto* vk_texture = ResourceCast(barrier.texture.GetTexture());
                auto  access     = tracker.ReadTexture(vk_texture, barrier.state);
                tracker.QueueTransferAcquireResource(
                    vk_texture,
                    src_queue_family,
                    dst_queue_family,
                    vk_texture->GetQueuePreferredLayout(src_queue),
                    std::get<1>(access),
                    VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                );
            }

            for (auto& barrier : _cmd->ImportBuffers()) {
                auto* vk_buffer = ResourceCast(barrier.buffer.GetBuffer());
                auto  access    = tracker.ReadBuffer(vk_buffer, barrier.state);
                tracker.QueueTransferAcquireResource(
                    vk_buffer,
                    src_queue_family,
                    dst_queue_family,
                    VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                );
            }

        } else {
            for (auto& barrier : _cmd->ExportTextures()) {
                auto* vk_texture = ResourceCast(barrier.texture.GetTexture());
                auto  access     = tracker.ReadTexture(vk_texture, barrier.state);
                tracker.QueueTransferReleaseResource(
                    vk_texture,
                    src_queue_family,
                    dst_queue_family,
                    vk_texture->GetQueuePreferredLayout(src_queue),
                    std::get<1>(access)
                );
            }

            for (auto& barrier : _cmd->ExportBuffers()) {
                auto* vk_buffer = ResourceCast(barrier.buffer.GetBuffer());
                auto  access    = tracker.ReadBuffer(vk_buffer, barrier.state);
                tracker.QueueTransferReleaseResource(
                    vk_buffer,
                    src_queue_family,
                    dst_queue_family
                );
            }
        }
    }
    void Visit(const SetDrawStateCmd* _cmd) {

        const auto& vbs = _cmd->VertexBuffers();
        for (const auto& vb : vbs) {
            auto* vk_buffer = ResourceCast(vb.first);
            tracker.RecordState(
                vk_buffer, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
            );
        }
        const auto& ibs = _cmd->IndexBuffers();
        for (const auto& ib : ibs) {
            auto* vk_buffer = ResourceCast(ib.first);
            tracker.RecordState(vk_buffer, VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
        }

        const auto& indirect_buffers = _cmd->IndirectBuffers();
        for (const auto& ib : indirect_buffers) {
            auto* vk_buffer = ResourceCast(ib.first);
            tracker.RecordState(
                vk_buffer, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
            );
        }

        const auto& count_buffers = _cmd->DrawCountBuffers();
        for (const auto& ib : count_buffers) {
            auto* vk_buffer = ResourceCast(ib.first);
            tracker.RecordState(
                vk_buffer, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
            );
        }

        // auto func = [&](const TArg& _arg, ParamInfoFlags _flag) {
        //     VisitArgs(_arg, (VulkanShaderResourceState)_flag.state_flags, _flag.pipeline_flags);
        // };
        // _cmd->IterateArgs(func);

        writed_buffer_resources.clear();
        writed_texture_resources.clear();

        const auto& pipeline = _cmd->Pipeline();
        auto        func     = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx))
                VisitArgs(
                    _arg,
                    pipeline.binding_infos[_idx].state_flags,
                    pipeline.binding_infos[_idx].pipeline_flags
                );
        };
        auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
            if (pipeline.valid_bits & (1 << _idx))
                HandleBindless(std::get<BindlessArrayRef>(_arg), pipeline.binding_infos[_idx].pipeline_flags);
        };

        _cmd->IterateArgs(func, bdls_post_func);

        for (const auto& rt : _cmd->RenderPassInfo().color_attachments) {
            auto* vk_texture = ResourceCast(rt.target);
            auto  action     = rt.action;
            bool  b_load     = GetLoadOp(action) == EAttachmentLoadOp::LOAD;
            bool  b_store    = GetStoreOp(action) == EAttachmentStoreOp::STORE;
            tracker.RecordState(
                vk_texture,
                (b_load ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
                    (b_store ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                static_cast<uint8>(rt.mip_level),
                1,
                static_cast<uint8>(rt.array_layer),
                1
            );
        }
        if (_cmd->RenderPassInfo().depth_attachment.Valid()) {
            auto* vk_texture      = ResourceCast(_cmd->RenderPassInfo().depth_attachment.target);
            auto  action          = _cmd->RenderPassInfo().depth_attachment.action;
            bool  b_depth_load    = GetLoadOp(GetDepthAction(action)) == EAttachmentLoadOp::LOAD;
            bool  b_depth_store   = GetStoreOp(GetDepthAction(action)) == EAttachmentStoreOp::STORE;
            bool  b_stencil_load  = GetLoadOp(GetStencilAction(action)) == EAttachmentLoadOp::LOAD;
            bool  b_stencil_store = GetStoreOp(GetStencilAction(action)) == EAttachmentStoreOp::STORE;
            bool  b_read          = b_depth_load || b_stencil_load;
            bool  b_write         = b_depth_store || b_stencil_store;
            tracker.RecordState(
                vk_texture,
                (b_read ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
                    (b_write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                static_cast<uint8>(_cmd->RenderPassInfo().depth_attachment.mip_level),
                1,
                static_cast<uint8>(_cmd->RenderPassInfo().depth_attachment.array_layer),
                1
            );
        }
    }

    void Visit(const MultiDrawCmd* _cmd) {
        const auto& vbs = _cmd->VertexBuffers();
        for (const auto& vb : vbs) {
            auto* vk_buffer = ResourceCast(vb.first);
            tracker.RecordState(
                vk_buffer, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT
            );
        }
        const auto& ibs = _cmd->IndexBuffers();
        for (const auto& ib : ibs) {
            auto* vk_buffer = ResourceCast(ib.first);
            tracker.RecordState(vk_buffer, VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
        }

        const auto& indirect_buffers = _cmd->IndirectBuffers();
        for (const auto& ib : indirect_buffers) {
            auto* vk_buffer = ResourceCast(ib.first);
            tracker.RecordState(
                vk_buffer, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT
            );
        }

        writed_buffer_resources.clear();
        writed_texture_resources.clear();

        UnorderedSet<const ArrayArguments*> temp_arg_batch;
        for (const auto& draw_cmd : _cmd->draw_batch.draw_cmds) {
            const auto& pipeline = draw_cmd.handle;
            auto        func     = [&](const TArg& _arg, uint _idx) {
                if (pipeline.valid_bits & (1 << _idx))
                    VisitArgs(
                        _arg,
                        pipeline.binding_infos[_idx].state_flags,
                        pipeline.binding_infos[_idx].pipeline_flags
                    );
            };
            auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
                if (pipeline.valid_bits & (1 << _idx))
                    HandleBindless(
                        std::get<BindlessArrayRef>(_arg), pipeline.binding_infos[_idx].pipeline_flags
                    );
            };
            const ArrayArguments* arg = std::holds_alternative<ArrayArguments>(draw_cmd.args) ?
                                            &std::get<ArrayArguments>(draw_cmd.args) :
                                            (std::holds_alternative<ArrayArgReference>(draw_cmd.args) ?
                                                 &cached_args[std::get<ArrayArgReference>(draw_cmd.args)()] :
                                                 nullptr);

            auto iter = temp_arg_batch.emplace(arg);
            if (arg && iter.second) {
                IterateArgs(*arg, func, bdls_post_func);
            }
        }

        // _cmd->IterateArgs(func, bdls_post_func);

        for (const auto& rt : _cmd->RenderPassInfo().color_attachments) {
            auto* vk_texture = ResourceCast(rt.target);
            auto  action     = rt.action;
            bool  b_load     = GetLoadOp(action) == EAttachmentLoadOp::LOAD;
            bool  b_store    = GetStoreOp(action) == EAttachmentStoreOp::STORE;
            tracker.RecordState(
                vk_texture,
                (b_load ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
                    (b_store ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                static_cast<uint8>(rt.mip_level),
                1,
                static_cast<uint8>(rt.array_layer),
                1
            );
        }
        if (_cmd->RenderPassInfo().depth_attachment.Valid()) {
            auto* vk_texture      = ResourceCast(_cmd->RenderPassInfo().depth_attachment.target);
            auto  action          = _cmd->RenderPassInfo().depth_attachment.action;
            bool  b_depth_load    = GetLoadOp(GetDepthAction(action)) == EAttachmentLoadOp::LOAD;
            bool  b_depth_store   = GetStoreOp(GetDepthAction(action)) == EAttachmentStoreOp::STORE;
            bool  b_stencil_load  = GetLoadOp(GetStencilAction(action)) == EAttachmentLoadOp::LOAD;
            bool  b_stencil_store = GetStoreOp(GetStencilAction(action)) == EAttachmentStoreOp::STORE;
            bool  b_read          = b_depth_load || b_stencil_load;
            bool  b_write         = b_depth_store || b_stencil_store;
            tracker.RecordState(
                vk_texture,
                (b_read ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
                    (b_write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                static_cast<uint8>(_cmd->RenderPassInfo().depth_attachment.mip_level),
                1,
                static_cast<uint8>(_cmd->RenderPassInfo().depth_attachment.array_layer),
                1
            );
        }
    }

    // void Visit(const SetGeometryPassDrawStateCmd* _cmd) {
    //     const auto& vbs = _cmd->VertexBuffers();
    //     for (const auto& vb : vbs) {
    //         auto* vk_buffer = ResourceCast(vb.first);
    //         tracker.RecordState(vk_buffer, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
    //     }
    //     const auto& ibs = _cmd->IndexBuffers();
    //     for (const auto& ib : ibs) {
    //         auto* vk_buffer = ResourceCast(ib.first);
    //         tracker.RecordState(vk_buffer, VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
    //     }

    //     writed_resources.clear();

    //     auto func = [&](const TArg& _arg, uint _idx) {
    //         for (const auto& [bitmask, pso] : _cmd->PipelineMap()) {
    //             if (pso.valid_bits & (1 << _idx))
    //                 VisitArgs(_arg, pso.binding_infos[_idx].state_flags, pso.binding_infos[_idx].pipeline_flags);
    //         }
    //     };
    //     auto bdls_post_func = [&](const TArg& _arg, uint _idx) {
    //         for (const auto& [bitmask, pso] : _cmd->PipelineMap()) {
    //             if (pso.valid_bits & (1 << _idx))
    //                 HandleBindless(std::get<BindlessArrayRef>(_arg), pso.binding_infos[_idx].pipeline_flags);
    //         }
    //     };

    //     _cmd->IterateArgs(func, bdls_post_func);

    //     for (const auto& rt : _cmd->RenderPassInfo().color_attachments) {
    //         auto* vk_texture = ResourceCast(rt.target);
    //         auto  action     = rt.action;
    //         bool  b_load     = GetLoadOp(action) == EAttachmentLoadOp::LOAD;
    //         bool  b_store    = GetStoreOp(action) == EAttachmentStoreOp::STORE;
    //         tracker.RecordState(
    //             vk_texture,
    //             (b_load ? VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
    //                 (b_store ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
    //             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    //             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    //             0,
    //             1);
    //     }
    //     if (_cmd->RenderPassInfo().depth_attachment.Valid()) {
    //         auto* vk_texture      = ResourceCast(_cmd->RenderPassInfo().depth_attachment.target);
    //         auto  action          = _cmd->RenderPassInfo().depth_attachment.action;
    //         bool  b_depth_load    = GetLoadOp(GetDepthAction(action)) == EAttachmentLoadOp::LOAD;
    //         bool  b_depth_store   = GetStoreOp(GetDepthAction(action)) == EAttachmentStoreOp::STORE;
    //         bool  b_stencil_load  = GetLoadOp(GetStencilAction(action)) == EAttachmentLoadOp::LOAD;
    //         bool  b_stencil_store = GetStoreOp(GetStencilAction(action)) == EAttachmentStoreOp::STORE;
    //         bool  b_read          = b_depth_load || b_stencil_load;
    //         bool  b_write         = b_depth_store || b_stencil_store;
    //         tracker.RecordState(
    //             vk_texture,
    //             (b_read ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT : VK_ACCESS_2_NONE) |
    //                 (b_write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_NONE),
    //             VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
    //             VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
    //             0,
    //             1);
    //     }
    // }

    void Visit(const UpdateBindlessArrayCmd* _cmd) {
        //use dispatch in the future
        // auto* vk_bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd->Handle());
        // tracker.RecordState(vk_bindless_array, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

        if (!_cmd->HasUpdates()) {
            return;
        }

        VulkanBindlessArray* vk_bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd->Handle());
        tracker.RecordState(
            vk_bindless_array->bindless_array_buffer,
            VK_ACCESS_2_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
        );

        if (_cmd->HasBufferUpdates()) {
            tracker.RecordState(
                vk_bindless_array->bindless_buffer_descs,
                VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
            );
        }

        if (_cmd->HasTextureUpdates()) {
            tracker.RecordState(
                vk_bindless_array->bindless_texture_descs,
                VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT
            );
        }
    }

    void Visit(const ClearResourceCmd* _cmd) {
        std::visit(
            [&](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, TextureView>) {
                    auto* vk_texture = ResourceCast(_arg.GetTexture());
                    tracker.RecordState(
                        vk_texture,
                        VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                        _arg.mip_level,
                        _arg.num_mips,
                        _arg.array_layer,
                        _arg.num_array
                    );
                } else if constexpr (std::is_same_v<T, BufferView>) {
                    auto* vk_buffer = ResourceCast(_arg.GetBuffer());
                    tracker.RecordState(
                        vk_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT
                    );
                }
            },
            _cmd->Resource()
        );
    }

    void Visit(const CustomCmd* _cmd) {
        switch (_cmd->CustomId()) {
            case CustomCmd::CustomCmdId::CUSTOM_RASTER:
                assert(false && "Custom raster draw scene not implemented");
                break;
            case CustomCmd::CustomCmdId::CUSTOM_DISPATCH:
                Visit(static_cast<const CustomDispatchCmd*>(_cmd));
                break;
            default:
                assert(false && "Invalid Custom Command for VkCmdPreprocessor");
        }
    }

    void Visit(const CustomDispatchCmd* _cmd) {
        auto func = [&](const TArg& _arg, ParamInfoFlags _flag) {
            VisitArgs(_arg, (VulkanShaderResourceState)_flag.state_flags, _flag.pipeline_flags);
        };
        _cmd->IterateArgs(func);
    }
};

#pragma endregion

#pragma region[ command visitor ]

class VkCmdVisitor : VulkanDeviceObject {
    enum class EState {
        Barrier,
        Draw,
        Common
    } state = EState::Common;
    VulkanCmdList&         cmd_list;
    VulkanAllocator&       allocator;
    VkTracker&             tracker;
    const TCachedArgArray& cached_args;
    ProfilerStorage*       profiler = nullptr;
    bool                   query_profiling_enabled = false;

    class ScopedNativeLabel {
    public:
        ScopedNativeLabel(
            VulkanCmdList&   _cmd_list,
            std::string_view _name,
            float4           _color
        ) : cmd_list(_cmd_list) {
            cmd_list.BeginLabel(_name, _color);
        }

        ~ScopedNativeLabel() {
            cmd_list.EndLabel();
        }

        ScopedNativeLabel(const ScopedNativeLabel&)            = delete;
        ScopedNativeLabel& operator=(const ScopedNativeLabel&) = delete;

    private:
        VulkanCmdList& cmd_list;
    };

public:
    VkCmdVisitor(
        VulkanDevice&          _device,
        VulkanAllocator&       _allocator,
        VkTracker&             _tracker,
        VulkanCmdList&         _cmd_list,
        const TCachedArgArray& _cached_args,
        ProfilerStorage*       _profiler = nullptr,
        bool                   _query_profiling_enabled = false
    ) :
        VulkanDeviceObject(&_device),
        allocator(_allocator),
        tracker(_tracker),
        cmd_list(_cmd_list),
        cached_args(_cached_args),
        profiler(_profiler),
        query_profiling_enabled(_query_profiling_enabled) {}

    void VisitCmd(const Command* _cmd) {
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
            _cmd.Data().size_bytes(),
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

        // tracker.RegisterFlushBuffer(tmp_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        // tracker.DispatchBarriers(cmd_list);
        cmd_list.CopyBuffer(
            src_buffer,
            reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            _cmd.ByteSize(),
            _cmd.Offset(),
            tmp_buffer.GetByteOffset()
        );

        // LOG_INFO("copyback temp buffer handle {} offset {} size {}", (uint64)ResourceCast(tmp_buffer.GetBuffer())->GetHandle(), tmp_buffer.GetByteOffset(), tmp_buffer.GetByteSize());

        // tracker.RegisterFlushBuffer(VulkanBuffer *_buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage)
        allocator.AddOnComplete([tmp_buffer, &cmd_list(cmd_list), src_data(_cmd.Data())]() {
            cmd_list.CopyData(src_data, tmp_buffer, tmp_buffer.GetByteSize());
        });
    }

    void Visit(const CopyBackTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
        auto           tmp_buffer  = _cmd.staging_buffer;

        // tracker.RegisterFlushBuffer(tmp_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        // tracker.DispatchBarriers(cmd_list);
        cmd_list.CopyTextureToBuffer(
            src_texture,
            reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            _cmd.Data().size_bytes(),
            _cmd.Offset(),
            tmp_buffer.GetByteOffset(),
            _cmd.Size(),
            _cmd.MipLevel()
        );

        // LOG_INFO("copyback temp buffer handle {} offset {} size {}", (uint64)ResourceCast(tmp_buffer.GetBuffer())->GetHandle(), tmp_buffer.GetByteOffset(), tmp_buffer.GetByteSize());

        allocator.AddOnComplete([tmp_buffer, &cmd_list(cmd_list), src_data(_cmd.Data())]() {
            cmd_list.CopyData(src_data.data(), tmp_buffer, tmp_buffer.GetByteSize());
        });
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
            _cmd.ByteSize(),
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
            _cmd.ByteSize(),
            _cmd.SrcOffset(),
            _cmd.DstOffset(),
            _cmd.Size(),
            _cmd.MipLevel()
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
        cmd_list.SetPso(_cmd.Pipeline());
        const auto& args = _cmd.Args(cached_args);

        cmd_list.BindDescriptors(pso, args);

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
        //     tracker.RecordState(vk_buffer, tracker.ReadBuffer(vk_buffer, buffer.state, buffer.pass_type));
        // }
        // for (const auto& buffer : write_buffers) {
        //     auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(buffer.handle);
        //     tracker.RecordState(vk_buffer, tracker.WriteBuffer(vk_buffer, buffer.state, buffer.pass_type));
        // }
        // for (const auto& texture : read_textures) {
        //     auto* vk_texture = reinterpret_cast<VulkanTexture*>(texture.handle);
        //     tracker.RecordState(vk_texture, tracker.ReadTexture(vk_texture, texture.state, texture.pass_type));
        // }
        // for (const auto& texture : write_textures) {
        //     auto* vk_texture = reinterpret_cast<VulkanTexture*>(texture.handle);
        //     tracker.RecordState(vk_texture, tracker.WriteTexture(vk_texture, texture.state, texture.pass_type));
        // }
    }

    void Visit(const SetDrawStateCmd& _cmd) {

        static float4 draw_color = {0.0f, 1.0f, 0.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, draw_color);
        state = EState::Draw;

        const auto&     args = _cmd.Args();
        const PipelineHandle& pso  = _cmd.Pipeline();

        const auto& pass_info = _cmd.RenderPassInfo();

        uint tex_min_width  = pass_info.render_area.extent.width + pass_info.render_area.offset.x;
        uint tex_min_height = pass_info.render_area.extent.height + pass_info.render_area.offset.y;

        Array<VkRenderingAttachmentInfo> color_attachments(pass_info.color_attachments.size());
        for (size_t i = 0; i < pass_info.color_attachments.size(); ++i) {
            color_attachments[i] = FromColorAttachmentInfo(pass_info.color_attachments[i]);

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
        std::optional<VkRenderingAttachmentInfo> depth_stencil_attachment;
        bool                                     uses_stencil_attachment = false;
        if (pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
            uses_stencil_attachment  = FormatHasStencil(pass_info.depth_attachment.target->GetFormat()) &&
                                      PipelineUsesStencilAttachment(_cmd.Pipeline());
        }

        VkRenderingInfo dynamic_rendering_info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderArea =
                {.offset = {pass_info.render_area.offset.x, pass_info.render_area.offset.y},
                 .extent = {pass_info.render_area.extent.width, pass_info.render_area.extent.height}},
            .layerCount           = 1,
            .colorAttachmentCount = uint(pass_info.color_attachments.size()),
            .pColorAttachments    = color_attachments.data(),
            .pDepthAttachment =
                depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr,
            .pStencilAttachment = (depth_stencil_attachment.has_value() && uses_stencil_attachment) ?
                                      &depth_stencil_attachment.value() :
                                      nullptr
        };

        cmd_list.BeginRendering(std::move(dynamic_rendering_info));

        cmd_list.SetPso(_cmd.Pipeline());

        cmd_list.BindDescriptors(pso, args);

        if (args.constants.size() > 0) {
            cmd_list.UploadPushConstants(
                pso, std::span<const uint>(args.constants.data(), args.constants.size())
            );
        }
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

        cmd_list.SetViewPort(viewport);
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

                cmd_list.SetVertexBuffers(
                    0,
                    num_of_vertex_buffers,
                    std::span<VkBuffer>(vertex_buffers.data(), num_of_vertex_buffers),
                    std::span<VkDeviceSize>(vtx_offsets.data(), num_of_vertex_buffers)
                );
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
                                cmd_list.DrawIndexedIndirectCnt(
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
                                cmd_list.DrawIndirectCnt(
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
        static float4 draw_color = {0.0f, 1.0f, 0.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, draw_color);
        state = EState::Draw;

        const auto&                      pass_info = _cmd.RenderPassInfo();
        Array<VkRenderingAttachmentInfo> color_attachments(pass_info.color_attachments.size());
        for (size_t i = 0; i < pass_info.color_attachments.size(); ++i) {
            color_attachments[i] = FromColorAttachmentInfo(pass_info.color_attachments[i]);
        }
        std::optional<VkRenderingAttachmentInfo> depth_stencil_attachment;
        bool                                     uses_stencil_attachment = false;
        if (pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
            uses_stencil_attachment  = FormatHasStencil(pass_info.depth_attachment.target->GetFormat()) &&
                                      DrawBatchUsesStencilAttachment(_cmd.draw_batch);
        }

        VkRenderingInfo dynamic_rendering_info{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderArea =
                {.offset = {pass_info.render_area.offset.x, pass_info.render_area.offset.y},
                 .extent = {pass_info.render_area.extent.width, pass_info.render_area.extent.height}},
            .layerCount           = 1,
            .colorAttachmentCount = uint(pass_info.color_attachments.size()),
            .pColorAttachments    = color_attachments.data(),
            .pDepthAttachment =
                depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr,
            .pStencilAttachment = (depth_stencil_attachment.has_value() && uses_stencil_attachment) ?
                                      &depth_stencil_attachment.value() :
                                      nullptr
        };

        cmd_list.BeginRendering(std::move(dynamic_rendering_info));
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
        cmd_list.SetViewPort(viewport);
        cmd_list.SetScissor({rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height});

        for (const DrawBatchElement& draw_cmd : _cmd.draw_batch.draw_cmds) {

            cmd_list.SetPso(draw_cmd.handle);
            const ArrayArguments* arg = std::holds_alternative<ArrayArguments>(draw_cmd.args) ?
                                            &std::get<ArrayArguments>(draw_cmd.args) :
                                            (std::holds_alternative<ArrayArgReference>(draw_cmd.args) ?
                                                 &cached_args[std::get<ArrayArgReference>(draw_cmd.args)()] :
                                                 nullptr);

            PipelineHandle& pipeline = *const_cast<PipelineHandle*>(&draw_cmd.handle);
            cmd_list.BindDescriptors(pipeline, *arg);

            if (arg && arg->constants.size() > 0) {
                cmd_list.UploadPushConstants(
                    pipeline, std::span<const uint>(arg->constants.data(), arg->constants.size())
                );
            }
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

                                cmd_list.SetVertexBuffers(
                                    0,
                                    num_of_vertex_buffers,
                                    std::span<VkBuffer>(vertex_buffers.data(), num_of_vertex_buffers),
                                    std::span<VkDeviceSize>(vtx_offsets.data(), num_of_vertex_buffers)
                                );
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
                                                cmd_list.DrawIndexedIndirectCnt(
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
                                                cmd_list.DrawIndirectCnt(
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

    //     cmd_list.BeginRendering(std::move(dynamic_rendering_info));

    //     for (const auto& [bitmask, pso] : _cmd.PipelineMap()) {
    //         cmd_list.SetPso(pso);
    //         cmd_list.BindDescriptors(pso, args);

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

    //         cmd_list.SetViewPort(viewport);
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
                    cmd_list.ClearTexture(vk_texture, value, range);
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
        allocator.AddOnComplete(
            [array_ref = std::move(array_ref),
             updates = std::move(updates),
             finalized = std::move(finalized),
             update_is_current]() mutable {
                FinalizeBindlessUpdatesOnce(
                    array_ref, updates, finalized, update_is_current
                );
            }
        );
        if (!update_is_current) {
            LOG_WARNING(
                "Rejecting command submission containing a stale bindless update batch"
            );
            // Silently omitting the upload would allow later draw/dispatch
            // commands in this same CmdSubmit to consume descriptors that were
            // never published. Let the queue reject the complete transaction.
            throw StaleBindlessUpdateBatch{};
        }
        // auto                 texture_slots  = _cmd.StealTextureUpdates();
        // auto                 buffer_slots   = _cmd.StealBufferUpdates();

        // auto free_textures = _cmd.StealFreeTextures();
        // auto free_buffers  = _cmd.StealFreeBuffers();
        // auto free_slots    = _cmd.StealFreeSlots();

        auto          array_data          = _cmd.StealArrayData();
        auto          array_indices_dat   = _cmd.StealArrayIndicesData();
        auto          texture_data        = _cmd.StealTextureData();
        auto          texture_indices_dat = _cmd.StealTextureIndicesData();
        Array<byte>&& buffer_data         = _cmd.StealBufferData();
        auto          buffer_indices_dat  = _cmd.StealBufferIndicesData();

        //update bindless array
        // {
        //     bindless_array->Lock();
        //     for (const VulkanBindlessArray::TextureUpdateInfo& texture : texture_slots) {
        //         uint indirect_handle                       = (m_device->GetSamplerIdx(texture.sampler) & 0xff) | (texture.slot & 0xffffff) << 8;
        //         bindless_array->handles[texture.array_idx] = {texture.slot, 1, VulkanBindlessArray::Texture};
        //     }
        //     for (const auto& buffer : buffer_slots) {
        //         uint indirect_handle                      = buffer.slot;
        //         bindless_array->handles[buffer.array_idx] = {buffer.slot, 0, VulkanBindlessArray::Buffer};
        //     }

        //     bindless_array->OnFree(free_slots, free_textures, free_buffers);
        //     bindless_array->Unlock();
        // }
        // uint64 texture_handle_stride = m_device->GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize;
        // uint64 buffer_handle_stride  = m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize;
        // uint64 array_handle_stride   = sizeof(uint);

        // // //cpu side data
        // Array<std::pair<uint, uint>> array_indices_dat(buffer_slots.size() + texture_slots.size());
        // Array<ubyte>                 array_dat((texture_slots.size() + buffer_slots.size()) * array_handle_stride);

        // Array<std::pair<uint, uint>> texture_indices_dat(texture_slots.size());
        // Array<ubyte>                 texture_dat(texture_slots.size() * texture_handle_stride);

        // Array<std::pair<uint, uint>> buffer_indices_dat(buffer_slots.size());
        // Array<ubyte>                 buffer_dat(buffer_slots.size() * buffer_handle_stride);

        // VulkanDescriptorHeap& heap = m_device->GetGlobalDescriptorHeap();

        //shuffle copy shader
        auto& shuffle_sd = m_device->internal_shaders->sd_component_shuffle;

        // byte* mapped_image_descs  = nullptr;
        // byte* mapped_buffer_descs = nullptr;

        uint array_idx = 0;

        bool b_array   = !texture_data.empty() || !buffer_data.empty();
        bool b_texture = !texture_data.empty();
        bool b_buffer  = !buffer_data.empty();

        // if (b_texture) {
        //     //copy texture handles
        //     for (size_t i = 0; i < texture_slots.size(); ++i) {
        //         const auto&    texture    = texture_slots[i];
        //         VulkanTexture* vk_texture = ResourceCast(texture_slots[i].texture);
        //         TextureView    view(vk_texture, texture.format, texture.mip_level, texture.num_mips);
        //         uint           src_idx;
        //         if (uint(vk_texture->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) != 0) {
        //             // view.aspect_flags = ETextureAspectFlags::COLOR;
        //             src_idx = heap.GetImageDescIdx(&view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

        //         } else {
        //             src_idx = heap.GetImageDescIdx(&view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        //         }
        //         memcpy(texture_dat.data() + i * texture_handle_stride, &heap.image_desc_data[src_idx], texture_handle_stride);
        //         texture_indices_dat[i]       = {i, texture_slots[i].slot};
        //         array_indices_dat[array_idx] = {array_idx, texture_slots[i].array_idx};

        //         uint indirect_handle = (m_device->GetSamplerIdx(texture.sampler) & 0xff) | (texture.slot & 0xffffff) << 8;
        //         memcpy(array_dat.data() + array_idx * array_handle_stride, &indirect_handle, array_handle_stride);
        //         array_idx++;
        //     }
        // }

        // if (b_buffer) {
        //     //copy buffer handles

        //     //buffer desc buffer has array_buffer for offset 0 and allocate from slot 1(0 for invalid handle)
        //     //array buffer is in same set with other bindless buffer descs, so we need to offset the slot by array_buffer slot(size of storage buffer handle)
        //     //in gpu:
        //     // bindless_array_buffer: [binding 0, offset 0]
        //     // bindless_buffer_descs: [binding 1, offset $sizeofhandle)]
        //     uint buffer_dst_slot_offset = bindless_array->buffers_offset_in_set / buffer_handle_stride;
        //     for (size_t i = 0; i < buffer_slots.size(); ++i) {
        //         VulkanBuffer* vk_buffer = ResourceCast(buffer_slots[i].buffer);
        //         uint          src_idx   = heap.GetBufferDescIdx(vk_buffer->GetView(), vk_buffer->GetDescriptorType());
        //         memcpy(buffer_dat.data() + i * buffer_handle_stride, &heap.buffer_desc_data[src_idx], buffer_handle_stride);
        //         buffer_indices_dat[i] = {i, buffer_slots[i].slot + buffer_dst_slot_offset};

        //         array_indices_dat[array_idx] = {array_idx, buffer_slots[i].array_idx};
        //         memcpy(array_dat.data() + array_idx * sizeof(uint), &buffer_slots[i].slot, sizeof(uint));
        //         array_idx++;
        //     }
        // }

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

            staging_buffer = allocator.AllocateShaderBuffer(staging_size);

            //copy cpu data to staging buffer
            if (b_array) {
                array_staging     = staging_buffer.buffer->GetView(array_staging_offset, array_staging_size);
                array_indices_buf = staging_buffer.buffer->GetView(array_indices_offset, array_indices_size);
                cmd_list.CopyData(array_staging, array_data.data(), array_staging_size);
                cmd_list.CopyData(array_indices_buf, array_indices_dat.data(), array_indices_size);
            }

            if (b_texture) {
                texture_desc_staging =
                    staging_buffer.buffer->GetView(texture_staging_offset, texture_staging_size);
                texture_indices_buf =
                    staging_buffer.buffer->GetView(texture_indices_offset, texture_indices_size);
                cmd_list.CopyData(texture_desc_staging, texture_data.data(), texture_staging_size);
                cmd_list.CopyData(texture_indices_buf, texture_indices_dat.data(), texture_indices_size);
            }

            if (b_buffer) {
                buffer_desc_staging =
                    staging_buffer.buffer->GetView(buffer_staging_offset, buffer_staging_size);
                buffer_indices_buf =
                    staging_buffer.buffer->GetView(buffer_indices_offset, buffer_indices_size);
                cmd_list.CopyData(buffer_desc_staging, buffer_data.data(), buffer_staging_size);
                cmd_list.CopyData(buffer_indices_buf, buffer_indices_dat.data(), buffer_indices_size);
            }
        }

        //recording bindless array updates
        if (b_array) {
            cmd_list.BeginLabel("UpdateBindlessArray", {0.0f, 1.0f, 0.0f, 1.0f});
            cmd_list.SetPso(shuffle_sd.handle);
            ComponentShuffleShader::Arg arg;

            {
                //bindless array update
                arg.component_cnt = array_indices_dat.size();
                arg.stride        = sizeof(uint) >> 2;

                cmd_list.BindDescriptors(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg,
                        array_indices_buf,
                        array_staging,
                        bindless_array->bindless_array_buffer->GetView()
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

                cmd_list.BindDescriptors(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg,
                        texture_indices_buf,
                        texture_desc_staging,
                        bindless_array->bindless_texture_descs->GetView(
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

                cmd_list.BindDescriptors(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg,
                        buffer_indices_buf,
                        buffer_desc_staging,
                        bindless_array->bindless_buffer_descs->GetView(
                            bindless_array->buffers_offset_in_set
                        )
                    )
                );
                cmd_list.Dispatch((buffer_indices_dat.size() + 63) / 64, 1, 1);
            }

            cmd_list.EndLabel();
        }

        // allocator.AddOnComplete([bindless_array,
        //                          free_slots(_cmd.StealFreeSlots()),
        //                          free_buffers(_cmd.StealFreeBuffers()),
        //                          free_textures(std::move(_cmd.StealFreeTextures()))]() {
        //     bindless_array->OnFree(std::move(free_slots), std::move(free_textures), free_buffers);
        // });
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
        allocator.RecordTimestampQuery(cmd_list, _cmd);
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
            BufferView staging =
                allocator.AllocateShaderBuffer(to_update.size() * sizeof(VkAccelerationStructureInstanceKHR));
            BufferView indices = allocator.AllocateShaderBuffer(to_update.size() * sizeof(uint32) * 2);

            Array<std::pair<uint, uint>> to_update_indices(to_update.size());

            for (size_t i = 0; i < to_update.size(); ++i) {
                const auto& id       = to_update[i];
                to_update_indices[i] = {i, id};
            }

            cmd_list.CopyData(
                staging,
                _cmd.InstanceData().data(),
                to_update.size() * sizeof(VkAccelerationStructureInstanceKHR)
            );
            cmd_list.CopyData(indices, to_update_indices.data(), to_update.size() * sizeof(uint32) * 2);

            auto& shuffle_sd = m_device->internal_shaders->sd_component_shuffle;
            cmd_list.SetPso(shuffle_sd.handle);

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

            cmd_list.BindDescriptors(
                shuffle_sd.handle, shuffle_sd.SetArgs(arg, indices, staging, aligned_instance_buffer_view)
            );

            cmd_list.Dispatch((to_update.size() + 63) / 64, 1, 1);

            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcAccessMask       = VK_ACCESS_2_SHADER_WRITE_BIT;
            barrier.dstAccessMask       = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
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
            tracker.FlushSrcState(
                instance_buffer,
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR,
                VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
            );
            // tracker.RecordState(instance_buffer, {VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});
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
            m_device->GetInstance(),
            m_device->GetGpu(),
            m_device->GetDevice(),
            cmd_list.GetHandle(),
            &this->tracker
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

#pragma region[ Native Queue ]

VkNativeQueue::VkNativeQueue(EQueueType _type, VulkanDevice& _device) : device(_device), type(_type) {
    switch (_type) {
        case EQueueType::Graphics:
            queue = _device.GetGraphicsQueue();
            break;
        case EQueueType::Compute:
            queue = _device.GetComputeQueue();
            break;
        case EQueueType::Copy:
            queue = _device.GetTransferQueue();
            break;
        default:
            assert(false && "Invalid queue type");
    }
    assert(queue != VK_NULL_HANDLE && "Invalid queue type!");
}

VkNativeQueue::~VkNativeQueue() {}

VulkanOperationResult VkNativeQueue::SubmitEmpty(
    const VulkanOperationContext& _context,
    VkFence                       _fence
) {
    if (device.IsFaulted()) {
        wait_infos.clear();
        signal_infos.clear();
        device.RecordRejectedSubmit();
        return {EVulkanOperationStatus::Rejected, device.GetFirstFaultResult()};
    }

    VkSubmitInfo2 submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submit_info.pNext                    = nullptr;
    submit_info.waitSemaphoreInfoCount   = wait_infos.size();
    submit_info.pWaitSemaphoreInfos      = wait_infos.data();
    submit_info.signalSemaphoreInfoCount = signal_infos.size();
    submit_info.pSignalSemaphoreInfos    = signal_infos.data();
    submit_info.commandBufferInfoCount   = 0;
    submit_info.pCommandBufferInfos      = VK_NULL_HANDLE;
    VulkanOperationContext context = _context;
    context.operation  = EVulkanFaultOperation::QueueSubmitEmpty;
    context.queue_type = type;
    context.queue      = queue;
    const VulkanOperationResult outcome =
        device.SubmitOnQueue(queue, submit_info, _fence, context);
    NotifyNativeSubmission(type, queue, outcome, true);
    wait_infos.clear();
    signal_infos.clear();
    return outcome;
}

VulkanOperationResult VkNativeQueue::Submit(
    VulkanCmdList&                _cmdlist,
    const VulkanOperationContext& _context,
    VkFence                       _fence
) {
    VulkanCmdList* command_lists[] = {&_cmdlist};
    return Submit(std::span<VulkanCmdList* const>(command_lists), _context, _fence);
}

VulkanOperationResult VkNativeQueue::Submit(
    std::span<VulkanCmdList* const> _cmdlists,
    const VulkanOperationContext&   _context,
    VkFence                         _fence
) {
    assert(!_cmdlists.empty() && "ordered command-buffer submit must not be empty");
    if (device.IsFaulted()) {
        wait_infos.clear();
        signal_infos.clear();
        device.RecordRejectedSubmit();
        return {EVulkanOperationStatus::Rejected, device.GetFirstFaultResult()};
    }

    VulkanOperationContext context = _context;
    context.queue_type = type;
    context.queue      = queue;
    if (context.operation == EVulkanFaultOperation::PresentSubmit &&
        device.ShouldInjectPresentSubmit()) {
        LOG_INFO(
            "[VulkanFault][Injection] point=present-submit trigger={} mode=synthetic-device-lost",
            device.GetPresentSubmitFaultTrigger()
        );
        wait_infos.clear();
        signal_infos.clear();
        return device.InjectPresentSubmitFault(context);
    }

    VkSubmitInfo2 submit_info{};
    Array<VkCommandBufferSubmitInfo> cmd_infos;
    cmd_infos.reserve(_cmdlists.size());
    for (VulkanCmdList* cmd_list : _cmdlists) {
        assert(cmd_list != nullptr && "ordered command-buffer submit contains null entry");
        cmd_infos.push_back(
            VkCommandBufferSubmitInfo{
                .sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                .pNext         = nullptr,
                .commandBuffer = cmd_list->GetHandle()
            }
        );
    }
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submit_info.pNext                    = nullptr;
    submit_info.waitSemaphoreInfoCount   = wait_infos.size();
    submit_info.pWaitSemaphoreInfos      = wait_infos.data();
    submit_info.signalSemaphoreInfoCount = signal_infos.size();
    submit_info.pSignalSemaphoreInfos    = signal_infos.data();
    submit_info.commandBufferInfoCount   = static_cast<uint32_t>(cmd_infos.size());
    submit_info.pCommandBufferInfos      = cmd_infos.data();

    if (context.operation == EVulkanFaultOperation::None) {
        context.operation = EVulkanFaultOperation::QueueSubmit;
    }
    const VulkanOperationResult outcome =
        device.SubmitOnQueue(queue, submit_info, _fence, context);
    NotifyNativeSubmission(type, queue, outcome, false);
    wait_infos.clear();
    signal_infos.clear();
    return outcome;
}

void VkNativeQueue::Wait(VulkanFence* _fence, uint64 _fence_val, VkPipelineStageFlags2 _stage) {
    VkSemaphore sem = _fence->GetUnderlyingHandle();
    wait_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage
        }
    );
}

void VkNativeQueue::Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
    wait_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage
        }
    );
}
void VkNativeQueue::Signal(VulkanFence* _fence, uint64 _fence_val, VkPipelineStageFlags2 _stage) {
    VkSemaphore sem = _fence->GetUnderlyingHandle();
    signal_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage
        }
    );
}
void VkNativeQueue::Signal(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
    signal_infos.push_back(
        VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage
        }
    );
}
void VkCommandQueue::Wait(WaitEvent _evt) {
    auto* fence = reinterpret_cast<VulkanFence*>(_evt.timeline_handle);
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(_evt, _evt.value, false);

        queue_cv.notify_one();
    }
}

void VkNativeQueue::BeginLabel(std::string_view _label, float4 _color) {
    std::unique_lock<std::mutex> guard(*submit_mutex);
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = _label.data();
    label.color[0]   = _color.x;
    label.color[1]   = _color.y;
    label.color[2]   = _color.z;
    label.color[3]   = _color.w;
    vkQueueBeginDebugUtilsLabelEXT(queue, &label);
}

void VkNativeQueue::EndLabel() {
    std::unique_lock<std::mutex> guard(*submit_mutex);
    vkQueueEndDebugUtilsLabelEXT(queue);
}

void VkNativeQueue::InsertLabel(std::string_view _label, float4 _color) {
    std::unique_lock<std::mutex> guard(*submit_mutex);
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = _label.data();
    label.color[0]   = _color.x;
    label.color[1]   = _color.y;
    label.color[2]   = _color.z;
    label.color[3]   = _color.w;
    vkQueueInsertDebugUtilsLabelEXT(queue, &label);
}

#pragma endregion

#pragma region[ Profiler ]
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
    VulkanCmdList&          _cmd_list,
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
    VulkanCmdList&          _cmd_list,
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
#pragma endregion

#pragma region[ VkCommandQueue ]
static double RhiThreadProfileMilliseconds(
    std::chrono::steady_clock::time_point _begin,
    std::chrono::steady_clock::time_point _end
) {
    return std::chrono::duration<double, std::milli>(_end - _begin).count();
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

static bool WaitForSubmittedDependencies(
    VulkanDevice&           _device,
    const Array<WaitEvent>& _wait_events,
    EQueueType               _queue,
    const std::atomic_bool* _continue_waiting = nullptr
) {
    for (const WaitEvent& event : _wait_events) {
        auto* fence = reinterpret_cast<VulkanFence*>(event.timeline_handle);
        if (fence == nullptr ||
            !fence->WaitSubmitted(
                event.value,
                _continue_waiting,
                _queue,
                static_cast<uint32>(_wait_events.size())
            )) {
            return false;
        }
    }
    return !_device.IsFaulted();
}

static VkResult GetRejectedSubmitResult(VulkanDevice& _device) {
    return _device.IsFaulted() ? _device.GetFirstFaultResult() : VK_ERROR_UNKNOWN;
}

static VulkanOperationResult MakeUnsubmittedOutcome(
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

static void MarkSubmissionAccepted(
    VulkanFence* _queue_timeline, uint64 _timeline, const Array<SignalEvent>& _signal_events
) {
    _queue_timeline->MarkSubmitted(_timeline);
    for (const SignalEvent& event : _signal_events) {
        reinterpret_cast<VulkanFence*>(event.timeline_handle)->MarkSubmitted(event.value);
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

[[nodiscard]] static bool IsAcceptedPresentationSourcePublication(
    const TextureBarrier& _barrier,
    const VulkanTexture&  _texture
) noexcept {
    const ETextureUsageFlags required_usage =
        ETextureUsageFlags::PRESENTATION_SOURCE |
        ETextureUsageFlags::TRANSFER_SRC;
    return _barrier.publish_external_state &&
           _barrier.state == ETextureState::TRANSFER &&
           _barrier.pass_type == EPassType::Copy &&
           (_texture.GetUsage() & required_usage) == required_usage &&
           _texture.GetAspectFlags() == ETextureAspectFlags::COLOR &&
           _texture.GetNumSamples() == 1 &&
           !_texture.b_present &&
           _texture.GetPreferredLayout() ==
               VK_IMAGE_LAYOUT_GENERAL &&
           _barrier.mip_level == 0 &&
           _barrier.mip_cnt == _texture.GetNumMips() &&
           _barrier.array_layer == 0 &&
           _barrier.array_cnt == _texture.GetNumArray();
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

// Freeze source command order before CmdReorderer or parallel translation can
// change execution topology. Direct texture uses become concrete events;
// bindless uses remain symbolic until Submission can observe the last
// natively accepted descriptor membership.
static VulkanPresentationSourceStateProgram
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
            const TextureBarrier* legacy_publication = nullptr;
            const ExplicitTextureBarrier*
                explicit_publication = nullptr;
            for (const TextureBarrier& barrier :
                 _command.ReadTextures()) {
                if (barrier.handle == _handle) {
                    ++reference_count;
                    legacy_publication = &barrier;
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
                if (_submit.HasExplicitResourceStateOwnership()) {
                    ready =
                        explicit_publication != nullptr &&
                        IsAcceptedPresentationSourcePublication(
                            *explicit_publication, *texture
                        );
                } else {
                    ready =
                        legacy_publication != nullptr &&
                        IsAcceptedPresentationSourcePublication(
                            *legacy_publication, *texture
                        );
                }
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
                    static_cast<const CustomDispatchCmd&>(
                        custom
                    ).IterateArgs(
                        [&](const TArg& argument, ParamInfoFlags) {
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

struct VulkanPresentationSourceMembershipCommit {
    BindlessArrayRef bindless_array{};
    VulkanBindlessArray::PresentationSourceMembershipSnapshot
        membership{};
};

struct VulkanPresentationSourceStateTransaction {
    Array<VulkanPresentationSourceStateDelta>       delta{};
    Array<VulkanPresentationSourceMembershipCommit> membership_commits{};
    std::unique_lock<std::mutex>                     submission_lock{};
};

static std::mutex& PresentationSourceBindlessSubmissionMutex() {
    static std::mutex mutex;
    return mutex;
}

static VulkanPresentationSourceStateTransaction
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

static void CommitAcceptedPresentationSourceStateTransaction(
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
    return _outcome.status == EVulkanOperationStatus::Rejected &&
           !_device.IsFaulted() &&
           _outcome.result != VK_ERROR_DEVICE_LOST;
}

static void TerminalizeUnsubmittedSignal(
    VulkanDevice&                _device,
    VulkanFence*                 _fence,
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

static void TerminalizeUnsubmittedSignals(
    VulkanDevice&                _device,
    const Array<SignalEvent>&    _signal_events,
    const VulkanOperationResult& _outcome
) {
    for (const SignalEvent& event : _signal_events) {
        TerminalizeUnsubmittedSignal(
            _device,
            reinterpret_cast<VulkanFence*>(event.timeline_handle),
            event.value,
            _outcome
        );
    }
}

static QueryPublishBatch PublishUnsubmittedQueryErrors(
    CmdSubmit&             _submit,
    VulkanAllocatorBatch*  _allocators,
    std::string_view       _reason
) noexcept {
    const QueryPublishBatch query_batch =
        _submit.query_publish_batch.Valid() ?
            _submit.query_publish_batch :
            QueryBackendAccess::BeginPublishBatch();

    if (_allocators != nullptr) {
        for (auto& allocator : _allocators->submitted) {
            if (allocator) {
                allocator->PublishTimestampQueriesAfterGpuCompletion(
                    query_batch, false, _reason
                );
            }
        }
        for (auto& allocator : _allocators->abandoned) {
            if (allocator) {
                allocator->PublishTimestampQueriesAfterGpuCompletion(
                    query_batch, false, _reason
                );
            }
        }
    }
    // The submit-token pass is also the malformed/native-record fallback in
    // the opposite direction: a token without an allocator record still
    // becomes terminal in the same transaction.
    _submit.PublishPendingQueryErrors(_reason, query_batch);
    return query_batch;
}

static void TerminalizeRejectedSubmit(
    VulkanDevice&    _device,
    CmdSubmit&&      _submit,
    VkResult         _result,
    std::string_view _context
) noexcept {
    if (_result == VK_SUCCESS) {
        _result = VK_ERROR_UNKNOWN;
    }

    _device.RecordRejectedSubmit();
    TerminalizeUnsubmittedSignals(
        _device,
        _submit.signal_events,
        VulkanOperationResult{EVulkanOperationStatus::Rejected, _result}
    );
    FinalizeRejectedBindlessUpdates(_submit);
    _submit.RejectPendingQueries(_context);

    // Completion callbacks are the only user code retained on a rejected
    // packet. Success callbacks are intentionally destroyed without running.
    // Drop command/cached payload first, matching normal queue retirement and
    // giving unhanded bindless updates their destructor rollback opportunity.
    Array<std::function<void()>> callbacks = std::move(_submit.callbacks);
    _submit.success_callbacks.clear();
    _submit.cmds.clear();
    _submit.cached_args.clear();
    _submit.segments.clear();
    _submit.wait_events.clear();
    _submit.signal_events.clear();
    _submit.debug_label.clear();

    InvokeCallbacksNoexcept(callbacks, _context);
}

VkCommandQueue::VkCommandQueue(
    VulkanDevice& _device,
    EQueueType    _type,
    bool          _enable_rhi_thread,
    bool          _thread_profile_logging,
    bool          _parallel_recording,
    uint32_t      _parallel_record_workers,
    bool          _parallel_record_verify,
    bool          _parallel_record_profile,
    uint32_t      _parallel_record_min_work_units_per_job,
    uint64_t      _parallel_record_worker_throw_trigger
) :
    CommandQueue(),
    vk_device(_device),
    queue(_type, _device),
    timestamp_pool(
        _device,
        VK_QUERY_TYPE_TIMESTAMP,
        s_queue_max_frame_in_flight * s_query_max_storage * 4,
        _type
    ),
    profiler_storage(timestamp_pool),
    rhi_thread_enabled(_enable_rhi_thread),
    parallel_record_requested(_parallel_recording),
    parallel_record_verify(_parallel_record_verify),
    parallel_record_profile_enabled(_parallel_record_profile),
    parallel_record_min_work_units_per_job(
        std::max(1u, _parallel_record_min_work_units_per_job)
    ),
    parallel_record_worker_throw_trigger(_parallel_record_worker_throw_trigger) {
    if (_thread_profile_logging) {
        thread_profile = MakeUnique<RhiThreadProfileState>();
    }
    if (parallel_record_profile_enabled && _type == EQueueType::Graphics) {
        parallel_record_profile = MakeUnique<ParallelRecordProfileState>();
    }
    if (parallel_record_requested && rhi_thread_enabled && _type == EQueueType::Graphics) {
        const uint32 hardware_threads = std::max(2u, std::thread::hardware_concurrency());
        parallel_record_workers = _parallel_record_workers == 0
                                      ? std::min(4u, hardware_threads)
                                      : std::clamp(_parallel_record_workers, 2u, hardware_threads);
        parallel_record_pool = MakeUnique<ExternalCpuJoinPool>(parallel_record_workers);
    }
    timeline = MoerNew(VulkanFence(vk_device));

    try {
        completion_worker_running = true;
        completion_thread =
            std::jthread(&VkCommandQueue::CompletionThreadMain, this);

        if (rhi_thread_enabled) {
            rhi_worker_running = true;
            rhi_thread = std::jthread(&VkCommandQueue::RhiThreadMain, this);
        }

        LOG_INFO(
            "[Threading] Vulkan {} queue RHI mode: {}",
            _type == EQueueType::Graphics ? "graphics" : "compute",
            rhi_thread_enabled ? "threaded" : "synchronous"
        );
        if (parallel_record_requested) {
            LOG_INFO(
                "[ParallelRecord] queue={} requested=true effective={} workers={} "
                "min_work_units_per_job={} reason={}",
                _type == EQueueType::Graphics ? "Graphics" : "Compute",
                parallel_record_pool != nullptr,
                parallel_record_workers,
                parallel_record_min_work_units_per_job,
                parallel_record_pool ? "none" :
                _type != EQueueType::Graphics ? "unsupported-queue" :
                                                "rhi-thread-disabled-or-bypass"
            );
        }
        if (parallel_record_profile) {
            LOG_INFO(
                "[ParallelRecordProfile][Config] queue=Graphics enabled=true window_ms=1000"
            );
        }
    } catch (...) {
        if (rhi_thread_enabled) {
            {
                std::lock_guard lock(rhi_work_mutex);
                rhi_worker_running = false;
            }
            rhi_work_cv.notify_all();
            if (rhi_thread.joinable()) {
                rhi_thread.join();
            }
        }

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
    FlushParallelRecordProfile();

    if (rhi_thread_enabled) {
        {
            std::unique_lock<std::mutex> lock(rhi_work_mutex);
            rhi_worker_running = false;
        }
        rhi_work_cv.notify_all();
        rhi_thread.join();
    }

    if (parallel_record_pool) {
        parallel_record_pool->StopAndDrain();
        parallel_record_pool.reset();
    }

    {
        std::unique_lock<std::mutex> lock(event_mutex);
        completion_worker_running = false;
    }
    queue_cv.notify_all();
    completion_thread.join();

    Array<VulkanAllocator*> allocs;
    allocators.PopAll(allocs);
    for (auto* allocator : allocs) {
        MoerDelete(allocator);
    }

    Array<VulkanPresentor*> presents;
    presentors.PopAll(presents);
    for (auto* presentor : presents) {
        MoerDelete(presentor);
    }

    allocator_quarantine.clear();
    presentor_quarantine.clear();
    MoerDelete(timeline);
    vk_device.RecordQueueSyncComplete();
}

bool VkCommandQueue::ClaimRuntimeOwnership() {
    EExecutionOwnershipMode expected = EExecutionOwnershipMode::Unclaimed;
    if (execution_ownership_mode.compare_exchange_strong(
            expected,
            EExecutionOwnershipMode::Runtime,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        std::lock_guard lock(rhi_work_mutex);
        if (!rhi_work_queue.empty() || enqueued_rhi_work != completed_rhi_work) {
            execution_ownership_mode.store(
                EExecutionOwnershipMode::Unclaimed, std::memory_order_release
            );
            LOG_ERROR(
                "[RHIExecutor][Vulkan] cannot claim Runtime ownership for queue={} "
                "with legacy work in flight",
                static_cast<uint32>(queue.GetType())
            );
            return false;
        }
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
            "[RHIExecutor][Vulkan] queue={} Runtime ownership claim rejected mode={}",
            static_cast<uint32>(queue.GetType()),
            static_cast<uint32>(expected)
        );
    } catch (...) {
    }
    return false;
}

void VkCommandQueue::ReleaseRuntimeOwnership() noexcept {
    CancelRuntimeDependencyWaits();
    EExecutionOwnershipMode expected = EExecutionOwnershipMode::Runtime;
    const bool released = execution_ownership_mode.compare_exchange_strong(
        expected,
        EExecutionOwnershipMode::Unclaimed,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
    assert(released && "only the owning Vulkan runtime may release a queue claim");
}

void VkCommandQueue::CancelRuntimeDependencyWaits() noexcept {
    runtime_dependency_waits_enabled.store(false, std::memory_order_release);
}

bool VkCommandQueue::ClaimLegacyOwnership(std::string_view _operation) {
    EExecutionOwnershipMode expected = EExecutionOwnershipMode::Unclaimed;
    if (execution_ownership_mode.compare_exchange_strong(
            expected,
            EExecutionOwnershipMode::Legacy,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        LOG_INFO(
            "[Threading] Vulkan queue={} ownership=Legacy first_operation={}",
            static_cast<uint32>(queue.GetType()),
            _operation
        );
        return true;
    }
    if (expected == EExecutionOwnershipMode::Legacy) {
        return true;
    }
    LOG_ERROR(
        "[RHIExecutor][Vulkan] rejected legacy {} on Runtime-owned queue={}",
        _operation,
        static_cast<uint32>(queue.GetType())
    );
    return false;
}

WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
    if (!ClaimLegacyOwnership("Execute")) {
        TerminalizeRejectedSubmit(
            vk_device,
            std::move(_submit),
            VK_ERROR_UNKNOWN,
            "[VulkanQueue] rejected mixed Runtime/Legacy submit"
        );
        return {uint64(timeline), last_frame.load(std::memory_order_acquire)};
    }
    if (vk_device.IsFaulted()) {
        TerminalizeRejectedSubmit(
            vk_device,
            std::move(_submit),
            vk_device.GetFirstFaultResult(),
            "[VulkanQueue] rejected submit"
        );
        return {uint64(timeline), last_frame.load(std::memory_order_acquire)};
    }
    if (rhi_thread_enabled) {
        std::chrono::steady_clock::time_point caller_started{};
        if (thread_profile) {
            caller_started = std::chrono::steady_clock::now();
        }
        uint64 current_timeline;
        {
            std::unique_lock<std::mutex> lock(rhi_work_mutex);
            current_timeline = last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint64 serial = ++enqueued_rhi_work;
            const uint32 enqueue_depth = uint32(rhi_work_queue.size() + 1);
            const auto enqueued_at = thread_profile ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
            rhi_work_queue.emplace_back(
                std::in_place_type<RhiExecuteWork>,
                std::move(_submit),
                current_timeline,
                serial,
                enqueued_at,
                enqueue_depth
            );
            if (thread_profile) {
                std::get<RhiExecuteWork>(rhi_work_queue.back()).caller_ms =
                    RhiThreadProfileMilliseconds(caller_started, std::chrono::steady_clock::now());
            }
        }
        rhi_work_cv.notify_one();
        return {uint64(timeline), current_timeline};
    }

    std::unique_lock<std::mutex> lock(exec_mtx);
    const uint64 current_timeline = last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (thread_profile) {
        const auto work_started = std::chrono::steady_clock::now();
        ExecuteNow(std::move(_submit), current_timeline, current_timeline);
        const double work_ms =
            RhiThreadProfileMilliseconds(work_started, std::chrono::steady_clock::now());
        RecordThreadingProfile(ERhiWorkKind::Execute, work_ms, 0.0, work_ms, 0);
    } else {
        ExecuteNow(std::move(_submit), current_timeline, current_timeline);
    }
    return {uint64(timeline), current_timeline};
}

std::optional<VkCommandQueue::CurrentVulkanRecordedSubmit>
VkCommandQueue::TranslateForRuntime(CmdSubmit&& _submit) noexcept {
    assert(
        execution_ownership_mode.load(std::memory_order_acquire) ==
            EExecutionOwnershipMode::Runtime &&
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
        ExecuteNow(
            std::move(_submit),
            current_timeline,
            current_timeline,
            &recorded,
            true,
            nullptr
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
            const CurrentVulkanSubmitResult submit_result =
                SubmitRecorded(
                    _recorded,
                    &runtime_dependency_waits_enabled,
                    true
                );
            if (split_profiling_frame && !submit_result.outcome.WasSubmitted()) {
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
    if (_recorded.retirement_outcome.WasSubmitted()) {
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
    if (split_profiling_frame && !_recorded.retirement_outcome.WasSubmitted()) {
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
    ResetSplitProfilingCpuFrame();
    const VulkanOperationResult outcome =
        MakeUnsubmittedOutcome(_result, _recoverable);
    vk_device.RecordRejectedSubmit();

    try {
        TerminalizeUnsubmittedSignals(
            vk_device,
            _submit.signal_events,
            outcome
        );
        // Rejection is now externally observable. Completion must not retain
        // and dereference the raw fence pointers after a waiter releases its
        // last FenceRef.
        _submit.signal_events.clear();
        PublishUnsubmittedQueryErrors(
            _submit,
            nullptr,
            "Vulkan runtime rejected the submit before GPU completion"
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
            VulkanAllocatorBatch{},
            std::optional<VulkanDescriptorPushLease>{},
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
    } catch (...) {
        // Continuing would destroy callbacks/signals outside Completion and
        // violate the ownership contract. Allocation failure is therefore
        // treated as an unrecoverable runtime bookkeeping failure.
        std::terminate();
    }
    queue_cv.notify_one();
}

VulkanRuntimeSubmissionResult VkCommandQueue::PresentForRuntime(
    SwapchainRef _swapchain,
    TextureView _source,
    PresentReceiptRef _receipt
) noexcept {
    assert(
        execution_ownership_mode.load(std::memory_order_acquire) ==
            EExecutionOwnershipMode::Runtime &&
        "runtime Present requires an exclusively claimed queue"
    );
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "runtime Present must run on the Submission owner"
    );
    try {
        if (vk_device.IsFaulted()) {
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(_receipt, false, false);
            return {
                .outcome = {
                    EVulkanOperationStatus::Rejected,
                    vk_device.GetFirstFaultResult()
                }
            };
        }
        TextureRef source_texture{_source.texture};
        const VulkanScriptedPresentOverrideForTesting* scripted_override =
            g_scripted_present_override.load(std::memory_order_acquire);
        const VulkanPresentSourceContract source_contract =
            ValidatePresentSourceContract(
                _source, _swapchain.Get()
            );
        if (!source_contract.valid) {
            InvokePresentSourceRejectionCallbackForTesting(
                scripted_override
            );
            bool recoverable_rejection = false;
            const VulkanOperationResult rejection =
                ClassifyPresentSourceRejection(
                    vk_device, recoverable_rejection
                );
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(_receipt, false, false);
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] rejected Present source contract: {}",
                    source_contract.reason
                );
            } catch (...) {
            }
            return {
                .outcome               = rejection,
                .recoverable_rejection = recoverable_rejection,
            };
        }
        auto* vk_source_texture =
            static_cast<VulkanTexture*>(_source.texture);
        if (!vk_source_texture->IsPresentationSourceReady() &&
            (scripted_override == nullptr ||
             scripted_override->require_present_source_ready)) {
            InvokePresentSourceRejectionCallbackForTesting(
                scripted_override
            );
            bool recoverable_rejection = false;
            const VulkanOperationResult rejection =
                ClassifyPresentSourceRejection(
                    vk_device, recoverable_rejection
                );
            vk_device.RecordRejectedPresent();
            ResolvePresentReceiptNoexcept(_receipt, false, false);
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] rejected Present source before an "
                    "accepted producer export published GENERAL / TRANSFER_READ"
                );
            } catch (...) {
            }
            return {
                .outcome               = rejection,
                .recoverable_rejection = recoverable_rejection,
            };
        }

        auto execution_lease = runtime_execution_gate.Acquire();
        std::unique_lock<std::mutex> execution_lock(exec_mtx);
        const uint64 current_timeline =
            last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        if (scripted_override != nullptr) {
            VulkanScriptedPresentResult scripted =
                scripted_override->callback(
                    scripted_override->context,
                    current_timeline
                );
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
                    scripted.outcome.status =
                        EVulkanOperationStatus::Rejected;
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
            Array<RHIResource*> deferred_releases = TakeDeferredReleases();
            CommitPresentCompletion(
                scripted.outcome,
                context,
                false,
                EVulkanPresentorRetirementState::Unused,
                {},
                std::move(_swapchain),
                std::move(source_texture),
                std::move(deferred_releases),
                current_timeline
            );
            ResolvePresentReceiptNoexcept(
                _receipt,
                false,
                scripted.outcome.status ==
                    EVulkanOperationStatus::Recreate
            );
            return {
                .outcome               = scripted.outcome,
                .completion            = std::nullopt,
                .recoverable_rejection = false,
            };
        }
        return PresentNow(
            std::move(_swapchain),
            std::move(source_texture),
            _source,
            std::move(_receipt),
            current_timeline,
            current_timeline,
            true
        );
    } catch (...) {
        vk_device.RecordRejectedPresent();
        ResolvePresentReceiptNoexcept(_receipt, false, false);
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] Present owner setup threw before timeline reservation"
            );
        } catch (...) {
        }
        return {
            .outcome = {
                EVulkanOperationStatus::Rejected,
                vk_device.IsFaulted() ?
                    vk_device.GetFirstFaultResult() :
                    VK_ERROR_UNKNOWN
            }
        };
    }
}

VkCommandQueue::CurrentVulkanSubmitResult
VkCommandQueue::SubmitRecorded(
    CurrentVulkanRecordedSubmit& _recorded,
    const std::atomic_bool*       _continue_waiting,
    bool                          _defer_completion
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

    const auto profile_submit_started = _recorded.profile.enabled ?
                                            std::chrono::steady_clock::now() :
                                            std::chrono::steady_clock::time_point{};
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
                    reinterpret_cast<VulkanFence*>(
                        event.timeline_handle
                    ),
                    event.value,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                );
            }
            for (auto& event : _recorded.submit.signal_events) {
                queue.Signal(
                    reinterpret_cast<VulkanFence*>(
                        event.timeline_handle
                    ),
                    event.value,
                    end_tag
                );
            }
            submit_outcome = _recorded.has_commands ?
                                 queue.Submit(
                                     std::span<VulkanCmdList* const>(
                                         _recorded.ordered_cmd_lists.data(),
                                         _recorded.ordered_cmd_lists.size()
                                     ),
                                     _recorded.context
                                 ) :
                                 queue.SubmitEmpty(
                                     _recorded.context
                                 );
            _recorded.native_submit_resolved = true;
            // Persist the native result before any signal-fence bookkeeping
            // can throw. Once vkQueueSubmit2 accepts the packet, every later
            // exception must still retire it as GPU-submitted.
            _recorded.retirement_outcome = submit_outcome;
            if (submit_outcome.WasSubmitted()) {
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
    const double profile_submit_cpu_ms = _recorded.profile.enabled ?
                                             RhiThreadProfileMilliseconds(
                                                 profile_submit_started,
                                                 std::chrono::steady_clock::now()
                                             ) :
                                             0.0;
    const uint32 ordered_cmd_list_count =
        static_cast<uint32>(_recorded.ordered_cmd_lists.size());

    if (!_defer_completion) {
        CommitRuntimeSubmitCompletion(_recorded, submit_outcome);
    }

    if (_recorded.profile.enabled) {
        _recorded.profile.sample.submit_cpu_ms = profile_submit_cpu_ms;
        _recorded.profile.sample.execute_cpu_wall_ms = RhiThreadProfileMilliseconds(
            _recorded.profile.execute_started, std::chrono::steady_clock::now()
        );
        _recorded.profile.sample.ordered_cb = ordered_cmd_list_count;
        RecordParallelRecordProfile(_recorded.profile.sample);
    }

    return {
        .outcome                = submit_outcome,
        .ordered_cmd_list_count = ordered_cmd_list_count,
    };
}

void VkCommandQueue::CommitRuntimeSubmitCompletion(
    CurrentVulkanRecordedSubmit& _recorded,
    const VulkanOperationResult&  _outcome
) noexcept {
    assert(!_recorded.completion_committed);
    try {
        if (!_outcome.WasSubmitted()) {
            TerminalizeUnsubmittedSignal(
                vk_device,
                timeline,
                _recorded.timeline,
                _outcome
            );
            TerminalizeUnsubmittedSignals(
                vk_device, _recorded.submit.signal_events, _outcome
            );
            // Exact signal rejection is published by Submission. Do not
            // retain raw external fence pointers in the Completion packet.
            _recorded.submit.signal_events.clear();
            PublishUnsubmittedQueryErrors(
                _recorded.submit,
                &_recorded.allocators,
                "Vulkan submission was rejected before GPU completion"
            );
            _recorded.allocators.abandoned.reserve(
                _recorded.allocators.abandoned.size() +
                _recorded.allocators.submitted.size()
            );
            for (auto& allocator : _recorded.allocators.submitted) {
                _recorded.allocators.abandoned.emplace_back(
                    std::move(allocator)
                );
            }
            _recorded.allocators.submitted.clear();
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
            _outcome.WasSubmitted(),
            _outcome.WasSubmitted(),
            std::move(_recorded.submit),
            std::move(_recorded.allocators),
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

bool VkCommandQueue::TryExecuteParallel(
    CmdSubmit&                    _submit,
    uint64                        _timeline,
    uint64                        _serial,
    const VulkanOperationContext& _context,
    const FunctionTable&           _function_table,
    const CmdReorderer&            _reorderer,
    double                         _reorder_time,
    std::chrono::steady_clock::time_point _profile_started,
    VulkanPresentationSourceStateProgram&
                                  _presentation_source_program,
    std::optional<CurrentVulkanRecordedSubmit>* _out_recorded
) {
    const ERHIProfilingPhase profiling_phase = _submit.ProfilingPhase();
    const bool emits_profiling_queries       = _submit.EmitsProfilingQueries();
    const bool begins_profiling_frame        = _submit.BeginsProfilingFrame();
    const bool ends_profiling_frame          = _submit.EndsProfilingFrame();
    const bool complete_profiling_frame =
        profiling_phase == ERHIProfilingPhase::Complete;
    auto fallback_name = [](ParallelRecordFallbackReason _reason) -> std::string_view {
        switch (_reason) {
            case ParallelRecordFallbackReason::None:
                return "none";
            case ParallelRecordFallbackReason::WorkerCountTooSmall:
                return "worker-count-too-small";
            case ParallelRecordFallbackReason::NoEligibleLayer:
                return "no-eligible-layer";
            case ParallelRecordFallbackReason::InsufficientWork:
                return "insufficient-work";
            case ParallelRecordFallbackReason::InsufficientConcurrency:
                return "insufficient-concurrency";
            case ParallelRecordFallbackReason::InvalidWorkDescription:
                return "invalid-work-description";
            case ParallelRecordFallbackReason::InvalidCommandType:
                return "invalid-command-type";
            case ParallelRecordFallbackReason::LayerTooLarge:
                return "layer-too-large";
        }
        return "unknown";
    };
    uint64 batch_serial = 0;
    auto verify_fallback = [&](std::string_view _reason) {
        if (parallel_record_verify) {
            LOG_INFO(
                "[ParallelRecord] batch={} requested=true effective=false reason={} coordinator={}",
                batch_serial,
                _reason,
                Platform::GetCurrentThreadID()
            );
        }
    };

    if (!parallel_record_pool || queue.GetType() != EQueueType::Graphics || !rhi_thread_enabled) {
        return false;
    }
    batch_serial = ++parallel_record_batch_serial;
    if (thread_profile) {
        verify_fallback("record-diagnostics-enabled");
        return false;
    }
    if (std::any_of(
            _submit.cmds.begin(),
            _submit.cmds.end(),
            [](const UniquePtr<Command>& _command) {
                return _command &&
                       _command->Type() == Command::EType::Query;
            }
        )) {
        // A query pair must stay in one native command buffer owned by one
        // allocator. Other source packets can still translate concurrently;
        // only this source falls back to its serial recorder.
        verify_fallback("timestamp-query-serial-island");
        return false;
    }

    const auto& cmd_lists = _reorderer.m_cmd_lists;
    Array<Array<const Command*>> command_layers(cmd_lists.size());
    Array<Array<Command::EType>> type_layers(cmd_lists.size());
    Array<Array<uint32_t>>       work_unit_layers(cmd_lists.size());
    for (size_t layer_index = 0; layer_index < cmd_lists.size(); ++layer_index) {
        for (const auto* node = cmd_lists[layer_index].head; node != nullptr; node = node->next) {
            command_layers[layer_index].push_back(node->cmd);
            type_layers[layer_index].push_back(node->cmd->Type());
            work_unit_layers[layer_index].push_back(
                VulkanParallelRecordDetail::EstimateWorkUnits(
                    *node->cmd, _submit.cached_args
                )
            );
        }
    }

    // Scope commands own a reorderer layer. Validate that invariant before
    // any command payload or tracker state is prepared, then virtualize the
    // scope across independently recorded primaries below.
    Array<const ScopeCmd*> scope_validation_stack;
    for (const auto& commands : command_layers) {
        uint32 scope_count = 0;
        for (const Command* command : commands) {
            scope_count += command->Type() == Command::EType::Scope ? 1u : 0u;
        }
        if (scope_count != 0 && (scope_count != 1 || commands.size() != 1)) {
            verify_fallback("mixed-scope-layer");
            return false;
        }
        if (scope_count == 0) {
            continue;
        }
        const auto* scope = static_cast<const ScopeCmd*>(commands.front());
        if (scope->IsPush()) {
            scope_validation_stack.push_back(scope);
        } else {
            if (scope_validation_stack.empty() ||
                scope_validation_stack.back()->ScopeName() != scope->ScopeName() ||
                scope_validation_stack.back()->QueryTimestamp() != scope->QueryTimestamp()) {
                verify_fallback("unbalanced-scope");
                return false;
            }
            scope_validation_stack.pop_back();
        }
    }
    if (!scope_validation_stack.empty()) {
        verify_fallback("unbalanced-scope");
        return false;
    }

    Array<ParallelRecordLayerDescription> layer_descriptions;
    layer_descriptions.reserve(type_layers.size());
    for (size_t layer_index = 0; layer_index < type_layers.size(); ++layer_index) {
        bool force_serial = false;
        if (emits_profiling_queries) {
            for (const Command* command : command_layers[layer_index]) {
                if (command->Type() == Command::EType::ShaderDispatch &&
                    static_cast<const DispatchCmd*>(command)->ProfileSectionName() != "Other") {
                    force_serial = true;
                    break;
                }
            }
        }
        layer_descriptions.push_back({
            .command_types = std::span<const Command::EType>(
                type_layers[layer_index].data(), type_layers[layer_index].size()
            ),
            .command_work_units = std::span<const uint32_t>(
                work_unit_layers[layer_index].data(), work_unit_layers[layer_index].size()
            ),
            .force_serial = force_serial,
        });
    }
    const ParallelRecordPlan plan = BuildParallelRecordPlan(
        std::span<const ParallelRecordLayerDescription>(
            layer_descriptions.data(), layer_descriptions.size()
        ),
        parallel_record_workers,
        parallel_record_min_work_units_per_job
    );
    if (!plan.CanRecordInParallel()) {
        if (parallel_record_verify && batch_serial <= 16) {
            for (size_t layer_index = 0; layer_index < layer_descriptions.size(); ++layer_index) {
                const ParallelRecordLayerDescription& layer = layer_descriptions[layer_index];
                if (layer.command_types.empty()) {
                    continue;
                }
                std::string type_names;
                uint64      layer_work_units = 0;
                for (const Command::EType type : layer.command_types) {
                    if (!type_names.empty()) {
                        type_names += ',';
                    }
                    type_names += GetCommandRecordTraits(type).stable_name;
                }
                for (const uint32_t work_units : layer.command_work_units) {
                    layer_work_units += work_units;
                }
                LOG_INFO(
                    "[ParallelRecord] batch={} candidate-layer={} commands={} work_units={} "
                    "force_serial={} types={}",
                    batch_serial,
                    layer_index,
                    layer.command_types.size(),
                    layer_work_units,
                    layer.force_serial,
                    type_names
                );
            }
        }
        verify_fallback(fallback_name(plan.fallback_reason));
        return false;
    }
    uint64 parallel_work_units = 0;
    for (const ParallelRecordLayerPlan& layer_plan : plan.layers) {
        if (!layer_plan.parallel) {
            continue;
        }
        for (uint32 job_index = 0; job_index < layer_plan.job_count; ++job_index) {
            parallel_work_units += plan.jobs[layer_plan.first_job + job_index].work_units;
        }
    }
    for (const ParallelRecordLayerPlan& layer_plan : plan.layers) {
        if (!layer_plan.parallel) {
            continue;
        }
        for (const Command* command : command_layers[layer_plan.layer_index]) {
            if (!IsParallelRecordReplaySafe(command->Type())) {
                verify_fallback("replay-unsafe-command");
                return false;
            }
        }
    }

    struct ParallelChunkRuntime {
        ParallelRecordJobRange      range;
        UniquePtr<VulkanAllocator> allocator;
        uint32                      worker_id{0};
        VkResult                    native_failure{VK_SUCCESS};
        bool                        recorded{false};
    };
    struct ParallelLayerRuntime {
        Array<const Command*>          commands;
        Array<const ScopeCmd*>         scopes;
        Array<const ScopeCmd*>         scopes_after;
        UniquePtr<VulkanAllocator>     primary_allocator;
        Array<ParallelChunkRuntime>    chunks;
        UniquePtr<VulkanAllocator>     fallback_allocator;
        bool                           parallel{false};
        bool                           scope_only{false};
        bool                           preprocessed{false};
        bool                           worker_recorded{false};
    };

    const std::string queue_label = !_submit.debug_label.empty() ?
                                        _submit.debug_label :
                                        "Graphics Exec";
    const float4 queue_label_color = !_submit.debug_label.empty() ?
                                         _submit.debug_label_color :
                                         float4{1.0f, 0.0f, 0.0f, 1.0f};
    Timer execute_timer;
    bool  profiling_started = false;
    auto begin_primary = [&] (
                             VulkanCmdList& _cmd_list,
                             const Array<const ScopeCmd*>& _scopes,
                             bool _coordinator
                         ) -> VkResult {
        const VkResult begin_result = _cmd_list.Begin();
        if (begin_result != VK_SUCCESS) {
            return begin_result;
        }
        if (_coordinator && begins_profiling_frame && !profiling_started) {
            profiler_storage.CollectProfiling(_cmd_list.GetHandle());
            {
                std::unique_lock<std::mutex> profiler_lock(profiler_mutex);
                cached_profiler_entry = profiler_storage.GetProfilerEntry();
            }
            profiler_storage.BeginProfilerSession(_cmd_list, "Graphics Exec");
            if (complete_profiling_frame) {
                ResetSplitProfilingCpuFrame();
                execute_timer.Start();
            } else {
                BeginSplitProfilingCpuFrame();
            }
            profiling_started = true;
        }
        _cmd_list.BeginLabel(queue_label, queue_label_color);
        for (const ScopeCmd* scope : _scopes) {
            _cmd_list.BeginLabel(scope->ScopeName(), scope->Color());
        }
        return VK_SUCCESS;
    };
    auto end_primary = [&](VulkanCmdList& _cmd_list, const Array<const ScopeCmd*>& _scopes) -> VkResult {
        for (auto scope = _scopes.rbegin(); scope != _scopes.rend(); ++scope) {
            _cmd_list.EndLabel();
        }
        _cmd_list.EndLabel();
        return _cmd_list.End();
    };

    VulkanDescriptorHeap& descriptor_heap = vk_device.GetGlobalDescriptorHeap();
    VulkanDescriptorPushLease descriptor_lease =
        descriptor_heap.BeginPushDescriptors(EQueueType::Graphics);

    auto tracker_owner = GetAllocator();
    auto& tracker       = tracker_owner->GetTracker();
    VkCmdPreprocessor preprocessor(
        vk_device,
        tracker,
        *tracker_owner,
        _function_table,
        _submit.cached_args,
        queue.GetType()
    );

    Array<ParallelLayerRuntime> runtime_layers(cmd_lists.size());
    Array<const ScopeCmd*>      active_scopes;
    double                      preprocess_time = 0.0;
    auto reject_prepared_recording = [&](std::string_view _reason, VkResult _result) {
        if (_result == VK_SUCCESS) {
            _result = VK_ERROR_UNKNOWN;
        }
        try {
            LOG_ERROR(
                "[ParallelRecord] prepared batch {} rejected: reason={} VkResult={}",
                batch_serial,
                _reason,
                static_cast<int32_t>(_result)
            );
        } catch (...) {
        }
        ResetSplitProfilingCpuFrame();
        // Once preprocessing/recording has begun, falling back to replaying
        // the whole packet is unsafe: serial commands may already have moved
        // payloads or allocated transient resources, and RestoreState may have
        // published preferred layouts. Treat native record failure as terminal
        // so no later frame can consume partially prepared CPU-side state.
        vk_device.TryLatchFirstFault(_context, _result, false, false, true);
        vk_device.RecordRejectedSubmit();

        Array<UniquePtr<VulkanAllocator>> abandoned_allocators;
        auto collect_allocator = [&](UniquePtr<VulkanAllocator>& _allocator) {
            if (_allocator) {
                abandoned_allocators.push_back(std::move(_allocator));
            }
        };
        for (ParallelLayerRuntime& runtime : runtime_layers) {
            collect_allocator(runtime.primary_allocator);
            collect_allocator(runtime.fallback_allocator);
            for (ParallelChunkRuntime& chunk : runtime.chunks) {
                collect_allocator(chunk.allocator);
            }
        }
        collect_allocator(tracker_owner);

        Array<RHIResource*> deferred_releases = TakeDeferredReleases();
        CurrentVulkanRecordedSubmit rejected{
            std::move(_submit), _context, _timeline
        };
        rejected.allocators.abandoned = std::move(abandoned_allocators);
        rejected.descriptor_lease.emplace(std::move(descriptor_lease));
        rejected.deferred_releases   = std::move(deferred_releases);
        rejected.retirement_outcome = {
            EVulkanOperationStatus::Rejected, _result
        };
        rejected.native_submit_resolved = true;
        if (_out_recorded != nullptr) {
            // Runtime Translate must hand the terminal packet to Submission.
            // Its pre-Completion hook publishes the entire rejected suffix
            // before this source can release any Completion callback.
            _out_recorded->emplace(std::move(rejected));
        } else {
            CommitRuntimeSubmitCompletion(
                rejected, rejected.retirement_outcome
            );
        }
        if (parallel_record_verify) {
            try {
                LOG_INFO(
                    "[ParallelRecord] batch={} requested=true effective=false "
                    "outcome=record-rejected reason={} coordinator={} VkResult={}",
                    batch_serial,
                    _reason,
                    Platform::GetCurrentThreadID(),
                    static_cast<int32_t>(_result)
                );
            } catch (...) {
            }
        }
    };

    // Freeze scope state and command ownership before any recorder starts.
    // Synthetic label reopen/close at command-buffer boundaries never emits
    // timestamp queries; the real Scope command remains in a serial island.
    for (size_t layer_index = 0; layer_index < runtime_layers.size(); ++layer_index) {
        ParallelLayerRuntime& runtime = runtime_layers[layer_index];
        runtime.commands = std::move(command_layers[layer_index]);
        runtime.scopes = active_scopes;
        runtime.scope_only = runtime.commands.size() == 1 &&
                             runtime.commands.front()->Type() == Command::EType::Scope;
        runtime.parallel = plan.layers[layer_index].parallel;
        if (runtime.scope_only) {
            const auto* scope = static_cast<const ScopeCmd*>(runtime.commands.front());
            if (scope->IsPush()) {
                active_scopes.push_back(scope);
            } else {
                active_scopes.pop_back();
            }
        }
        runtime.scopes_after = active_scopes;
    }
    assert(active_scopes.empty() && "validated scope stack changed during parallel prepare");

    auto record_layer_prefix = [&](size_t _layer_index, VulkanCmdList& _cmd_list) {
        ParallelLayerRuntime& runtime = runtime_layers[_layer_index];
        assert(!runtime.preprocessed && "parallel layer prefix was recorded twice");
        const auto preprocess_begin = std::chrono::steady_clock::now();
        for (const Command* command : runtime.commands) {
            preprocessor.VisitCmd(command);
        }
        tracker.ResolveBarriers();
        tracker.DispatchBarriers(_cmd_list);
        runtime.preprocessed = true;
        preprocess_time += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - preprocess_begin
                           ).count();
        _cmd_list.InsertLabel(
            std::format("[RHI Parallel] Layer {}", _layer_index),
            {0.15f, 0.25f, 0.55f, 1.0f}
        );
    };

    std::atomic<uint32_t> active_jobs{0};
    std::atomic<uint32_t> max_active_jobs{0};
    UnorderedSet<uint32>  distinct_workers;
    uint64                total_job_count = 0;
    uint32                parallel_wave_count = 0;
    uint32                serial_island_count = 0;
    bool                  parallel_recorded = true;
    double                worker_join_time = 0.0;

    // Preserve translate-time side-effect ordering by draining each contiguous
    // safe wave before the coordinator visits the next serial island. The GPU
    // submission remains layer ordered, while commands inside the wave may be
    // translated concurrently regardless of their eventual execution layer.
    auto run_parallel_wave = [&](size_t _first_layer, size_t _layer_end) -> bool {
        Array<ExternalCpuJoinPool::Job> wave_jobs;
        for (size_t layer_index = _first_layer; layer_index < _layer_end; ++layer_index) {
            ParallelLayerRuntime& runtime = runtime_layers[layer_index];
            if (!runtime.parallel) {
                continue;
            }
            for (size_t chunk_index = 0; chunk_index < runtime.chunks.size(); ++chunk_index) {
                wave_jobs.emplace_back([&, layer_index, chunk_index] {
                    RHIThreadRoleScope owner_scope(ERHIThreadRole::RecordWorker);
                    ParallelLayerRuntime& job_layer = runtime_layers[layer_index];
                    ParallelChunkRuntime& chunk     = job_layer.chunks[chunk_index];
                    const uint32 now_active =
                        active_jobs.fetch_add(1, std::memory_order_acq_rel) + 1;
                    uint32 observed = max_active_jobs.load(std::memory_order_relaxed);
                    while (observed < now_active &&
                           !max_active_jobs.compare_exchange_weak(
                               observed, now_active, std::memory_order_relaxed
                           )) {
                    }
                    auto finish_active = [&] {
                        active_jobs.fetch_sub(1, std::memory_order_acq_rel);
                    };

                    try {
                        chunk.worker_id = Platform::GetCurrentThreadID();
                        VulkanCmdList& cmd_list = chunk.allocator->GetCmdList();
                        cmd_list.SetDescriptorPushLease(descriptor_lease.state);
                        const VkResult worker_begin =
                            begin_primary(cmd_list, job_layer.scopes, false);
                        if (worker_begin != VK_SUCCESS) {
                            chunk.native_failure = worker_begin;
                            throw std::runtime_error("parallel command buffer begin failed");
                        }
                        bool inject_after_first_command = false;
                        if (parallel_record_worker_throw_trigger != 0) {
                            const uint64 attempt = parallel_record_worker_attempts.fetch_add(
                                1, std::memory_order_acq_rel
                            ) + 1;
                            inject_after_first_command =
                                attempt == parallel_record_worker_throw_trigger;
                        }
                        cmd_list.InsertLabel(
                            std::format(
                                "[RHI Parallel] Layer {} Commands {}..{}",
                                layer_index,
                                chunk.range.command_begin,
                                chunk.range.CommandEnd()
                            ),
                            {0.1f, 0.55f, 0.8f, 1.0f}
                        );
                        VkCmdVisitor visitor(
                            vk_device,
                            *chunk.allocator,
                            chunk.allocator->GetTracker(),
                            cmd_list,
                            _submit.cached_args,
                            nullptr,
                            false
                        );
                        for (uint32 command_index = chunk.range.command_begin;
                             command_index < chunk.range.CommandEnd(); ++command_index) {
                            visitor.VisitCmd(job_layer.commands[command_index]);
                            if (inject_after_first_command) {
                                LOG_INFO(
                                    "[ParallelRecord][Injection] point=worker-throw "
                                    "phase=after-first-command trigger={} batch={} layer={} "
                                    "command={}",
                                    parallel_record_worker_throw_trigger,
                                    batch_serial,
                                    layer_index,
                                    command_index
                                );
                                throw std::runtime_error(
                                    "injected parallel command recorder worker failure"
                                );
                            }
                        }
                        const VkResult worker_end =
                            end_primary(cmd_list, job_layer.scopes_after);
                        if (worker_end != VK_SUCCESS) {
                            chunk.native_failure = worker_end;
                            throw std::runtime_error("parallel command buffer end failed");
                        }
                        chunk.recorded = true;
                    } catch (...) {
                        finish_active();
                        throw;
                    }
                    finish_active();
                });
            }
        }

        if (wave_jobs.empty()) {
            return true;
        }
        const uint32 wave_index = parallel_wave_count++;
        total_job_count += wave_jobs.size();
        const uint64 worker_descriptor_begin =
            descriptor_heap.CurrentPushDescriptorOffset(descriptor_lease.state);
        ExternalJoinResult join_result = ExternalJoinResult::Failed;
        const auto worker_join_started = std::chrono::steady_clock::now();
        try {
            join_result = parallel_record_pool->RunAndWait(
                std::span<const ExternalCpuJoinPool::Job>(wave_jobs.data(), wave_jobs.size())
            );
        } catch (...) {
            join_result = ExternalJoinResult::Failed;
        }
        worker_join_time += RhiThreadProfileMilliseconds(
            worker_join_started, std::chrono::steady_clock::now()
        );

        bool     wave_recorded = join_result == ExternalJoinResult::Completed;
        VkResult native_failure = VK_SUCCESS;
        for (size_t layer_index = _first_layer; layer_index < _layer_end; ++layer_index) {
            ParallelLayerRuntime& runtime = runtime_layers[layer_index];
            if (!runtime.parallel) {
                continue;
            }
            for (const ParallelChunkRuntime& chunk : runtime.chunks) {
                wave_recorded &= chunk.recorded;
                if (native_failure == VK_SUCCESS && chunk.native_failure != VK_SUCCESS) {
                    native_failure = chunk.native_failure;
                }
                if (chunk.worker_id != 0) {
                    distinct_workers.insert(chunk.worker_id);
                }
            }
        }
        if (parallel_record_verify) {
            try {
                LOG_INFO(
                    "[ParallelRecord][Wave] batch={} wave={} layers={}..{} jobs={} "
                    "join_completed={} worker_recorded={}",
                    batch_serial,
                    wave_index,
                    _first_layer,
                    _layer_end,
                    wave_jobs.size(),
                    join_result == ExternalJoinResult::Completed,
                    wave_recorded
                );
            } catch (...) {
            }
        }
        // Native command-buffer failures are device/driver failures, not a
        // replayable translation exception. Match coordinator Begin/End by
        // latching the original VkResult and rejecting the prepared packet.
        if (native_failure != VK_SUCCESS) {
            reject_prepared_recording("worker-native-record-failed", native_failure);
            return false;
        }
        for (size_t layer_index = _first_layer; layer_index < _layer_end; ++layer_index) {
            if (runtime_layers[layer_index].parallel) {
                runtime_layers[layer_index].worker_recorded = wave_recorded;
            }
        }
        if (wave_recorded) {
            return true;
        }

        parallel_recorded = false;
        // The join contract drains all accepted jobs. No worker can still be
        // writing when the submit-local descriptor cursor is rewound.
        descriptor_heap.RewindPushDescriptors(
            descriptor_lease.state, worker_descriptor_begin
        );
        for (size_t layer_index = _first_layer; layer_index < _layer_end; ++layer_index) {
            ParallelLayerRuntime& runtime = runtime_layers[layer_index];
            if (!runtime.parallel) {
                continue;
            }
            runtime.worker_recorded = false;
            runtime.fallback_allocator = GetAllocator();
            VulkanCmdList& fallback_cmd = runtime.fallback_allocator->GetCmdList();
            fallback_cmd.SetDescriptorPushLease(descriptor_lease.state);
            const VkResult fallback_begin =
                begin_primary(fallback_cmd, runtime.scopes, true);
            if (fallback_begin != VK_SUCCESS) {
                reject_prepared_recording("fallback-begin-failed", fallback_begin);
                return false;
            }
            fallback_cmd.InsertLabel(
                "[RHI Parallel] Coordinator Fallback", {0.75f, 0.25f, 0.1f, 1.0f}
            );
            VkCmdVisitor visitor(
                vk_device,
                *runtime.fallback_allocator,
                runtime.fallback_allocator->GetTracker(),
                fallback_cmd,
                _submit.cached_args,
                nullptr,
                false
            );
            try {
                for (const Command* command : runtime.commands) {
                    visitor.VisitCmd(command);
                }
            } catch (const StaleBindlessUpdateBatch& error) {
                try {
                    LOG_WARNING(
                        "[ParallelRecord] fallback rejected stale bindless batch: "
                        "layer={} error={}",
                        layer_index,
                        error.what()
                    );
                } catch (...) {
                }
                reject_prepared_recording(
                    "stale-bindless-update", VK_ERROR_VALIDATION_FAILED_EXT
                );
                return false;
            } catch (const std::exception& error) {
                try {
                    LOG_ERROR(
                        "[ParallelRecord] fallback record failed: layer={} error={}",
                        layer_index,
                        error.what()
                    );
                } catch (...) {
                }
                reject_prepared_recording(
                    "fallback-record-failed", VK_ERROR_OUT_OF_POOL_MEMORY
                );
                return false;
            } catch (...) {
                reject_prepared_recording(
                    "fallback-record-failed", VK_ERROR_OUT_OF_POOL_MEMORY
                );
                return false;
            }
            const VkResult fallback_end = end_primary(fallback_cmd, runtime.scopes_after);
            if (fallback_end != VK_SUCCESS) {
                reject_prepared_recording("fallback-end-failed", fallback_end);
                return false;
            }
        }
        return true;
    };

    // Consecutive coordinator-only layers share one primary. When a serial
    // island is immediately followed by a parallel layer, its final commands
    // also carry that layer's barrier prefix. This preserves GPU ordering while
    // avoiding one primary allocator/CB for every reorderer layer.
    constexpr size_t no_pending_wave = std::numeric_limits<size_t>::max();
    size_t pending_wave_begin = no_pending_wave;
    size_t pending_wave_end   = no_pending_wave;
    auto flush_parallel_wave = [&]() -> bool {
        if (pending_wave_begin == no_pending_wave) {
            return true;
        }
        const size_t wave_begin = pending_wave_begin;
        const size_t wave_end   = pending_wave_end;
        pending_wave_begin = no_pending_wave;
        pending_wave_end   = no_pending_wave;
        return run_parallel_wave(wave_begin, wave_end);
    };

    for (size_t layer_index = 0; layer_index < runtime_layers.size();) {
        while (layer_index < runtime_layers.size() &&
               runtime_layers[layer_index].commands.empty()) {
            ++layer_index;
        }
        if (layer_index == runtime_layers.size()) {
            break;
        }

        ParallelLayerRuntime& first_runtime = runtime_layers[layer_index];
        if (first_runtime.parallel) {
            if (!first_runtime.preprocessed) {
                first_runtime.primary_allocator = GetAllocator();
                VulkanCmdList& primary_cmd = first_runtime.primary_allocator->GetCmdList();
                primary_cmd.SetDescriptorPushLease(descriptor_lease.state);
                const VkResult primary_begin =
                    begin_primary(primary_cmd, first_runtime.scopes, true);
                if (primary_begin != VK_SUCCESS) {
                    reject_prepared_recording("coordinator-begin-failed", primary_begin);
                    return true;
                }
                try {
                    record_layer_prefix(layer_index, primary_cmd);
                } catch (const std::exception& error) {
                    try {
                        LOG_ERROR(
                            "[ParallelRecord] coordinator prefix failed: layer={} "
                            "error={}",
                            layer_index,
                            error.what()
                        );
                    } catch (...) {
                    }
                    reject_prepared_recording(
                        "coordinator-prefix-failed", VK_ERROR_OUT_OF_POOL_MEMORY
                    );
                    return true;
                } catch (...) {
                    reject_prepared_recording(
                        "coordinator-prefix-failed", VK_ERROR_OUT_OF_POOL_MEMORY
                    );
                    return true;
                }
                const VkResult primary_end = end_primary(primary_cmd, first_runtime.scopes_after);
                if (primary_end != VK_SUCCESS) {
                    reject_prepared_recording("coordinator-end-failed", primary_end);
                    return true;
                }
            }

            const ParallelRecordLayerPlan& layer_plan = plan.layers[layer_index];
            first_runtime.chunks.reserve(layer_plan.job_count);
            for (uint32 job_index = 0; job_index < layer_plan.job_count; ++job_index) {
                first_runtime.chunks.push_back({
                    .range     = plan.jobs[layer_plan.first_job + job_index],
                    .allocator = GetAllocator(),
                });
            }
            if (pending_wave_begin == no_pending_wave) {
                pending_wave_begin = layer_index;
            }
            pending_wave_end = layer_index + 1;
            ++layer_index;
            continue;
        }

        // No serial visitor may overtake the body translation of an earlier
        // safe layer. This is the CPU-side counterpart of ordered GPU submit.
        if (!flush_parallel_wave()) {
            return true;
        }

        const uint32 island_index = serial_island_count++;
        if (parallel_record_verify) {
            try {
                LOG_INFO(
                    "[ParallelRecord][Island] batch={} island={} first_layer={} "
                    "joined_waves={}",
                    batch_serial,
                    island_index,
                    layer_index,
                    parallel_wave_count
                );
            } catch (...) {
            }
        }

        first_runtime.primary_allocator = GetAllocator();
        VulkanCmdList& island_cmd = first_runtime.primary_allocator->GetCmdList();
        island_cmd.SetDescriptorPushLease(descriptor_lease.state);
        const VkResult island_begin = begin_primary(island_cmd, first_runtime.scopes, true);
        if (island_begin != VK_SUCCESS) {
            reject_prepared_recording("coordinator-begin-failed", island_begin);
            return true;
        }

        const Array<const ScopeCmd*>* island_exit_scopes = &first_runtime.scopes;
        try {
            while (layer_index < runtime_layers.size() &&
                   !runtime_layers[layer_index].parallel) {
                ParallelLayerRuntime& runtime = runtime_layers[layer_index];
                if (runtime.commands.empty()) {
                    ++layer_index;
                    continue;
                }
                record_layer_prefix(layer_index, island_cmd);
                VkCmdVisitor visitor(
                    vk_device,
                    *first_runtime.primary_allocator,
                    tracker,
                    island_cmd,
                    _submit.cached_args,
                    emits_profiling_queries ? &profiler_storage : nullptr,
                    emits_profiling_queries
                );
                for (const Command* command : runtime.commands) {
                    visitor.VisitCmd(command);
                }
                island_exit_scopes = &runtime.scopes_after;
                ++layer_index;
            }

            // The serial island executes immediately before the next parallel
            // body, so its primary can own that body's state-transition prefix.
            if (layer_index < runtime_layers.size() &&
                runtime_layers[layer_index].parallel &&
                !runtime_layers[layer_index].commands.empty()) {
                record_layer_prefix(layer_index, island_cmd);
                island_exit_scopes = &runtime_layers[layer_index].scopes;
            }
        } catch (const StaleBindlessUpdateBatch& error) {
            try {
                LOG_WARNING(
                    "[ParallelRecord] coordinator rejected stale bindless batch: "
                    "layer={} error={}",
                    layer_index,
                    error.what()
                );
            } catch (...) {
            }
            reject_prepared_recording(
                "stale-bindless-update", VK_ERROR_VALIDATION_FAILED_EXT
            );
            return true;
        } catch (const std::exception& error) {
            try {
                LOG_ERROR(
                    "[ParallelRecord] coordinator island record failed: layer={} "
                    "error={}",
                    layer_index,
                    error.what()
                );
            } catch (...) {
            }
            reject_prepared_recording(
                "coordinator-record-failed", VK_ERROR_OUT_OF_POOL_MEMORY
            );
            return true;
        } catch (...) {
            reject_prepared_recording(
                "coordinator-record-failed", VK_ERROR_OUT_OF_POOL_MEMORY
            );
            return true;
        }

        const VkResult island_end = end_primary(island_cmd, *island_exit_scopes);
        if (island_end != VK_SUCCESS) {
            reject_prepared_recording("coordinator-end-failed", island_end);
            return true;
        }
    }

    if (!flush_parallel_wave()) {
        return true;
    }

    if (!_submit.HasExplicitResourceStateOwnership()) {
        tracker.RestoreState();
    }
    VulkanCmdList& epilogue_cmd = tracker_owner->GetCmdList();
    epilogue_cmd.SetDescriptorPushLease(descriptor_lease.state);
    const VkResult epilogue_begin = begin_primary(epilogue_cmd, active_scopes, true);
    if (epilogue_begin != VK_SUCCESS) {
        reject_prepared_recording("epilogue-begin-failed", epilogue_begin);
        return true;
    }
    epilogue_cmd.InsertLabel("[RHI Parallel] Restore State", {0.15f, 0.25f, 0.55f, 1.0f});
    tracker.DispatchBarriers(epilogue_cmd);
    if (ends_profiling_frame) {
        profiler_storage.EndProfilerSession(epilogue_cmd, "Graphics Exec");
    }
    const VkResult epilogue_end = end_primary(epilogue_cmd, active_scopes);
    if (epilogue_end != VK_SUCCESS) {
        reject_prepared_recording("epilogue-end-failed", epilogue_end);
        return true;
    }
    tracker.Reset();
    descriptor_heap.EndPushDescriptors(descriptor_lease);
    const uint64 descriptor_bytes =
        descriptor_heap.CurrentPushDescriptorOffset(descriptor_lease.state) -
        descriptor_lease.state->begin;

    Array<VulkanCmdList*> ordered_cmd_lists;
    Array<UniquePtr<VulkanAllocator>> submitted_allocators;
    Array<UniquePtr<VulkanAllocator>> abandoned_allocators;
    ordered_cmd_lists.reserve(plan.layers.size() + total_job_count + 1);
    submitted_allocators.reserve(plan.layers.size() + total_job_count + 1);
    for (ParallelLayerRuntime& runtime : runtime_layers) {
        if (runtime.primary_allocator) {
            ordered_cmd_lists.push_back(&runtime.primary_allocator->GetCmdList());
            submitted_allocators.push_back(std::move(runtime.primary_allocator));
        }
        if (!runtime.parallel) {
            continue;
        }
        if (runtime.worker_recorded) {
            for (ParallelChunkRuntime& chunk : runtime.chunks) {
                ordered_cmd_lists.push_back(&chunk.allocator->GetCmdList());
                submitted_allocators.push_back(std::move(chunk.allocator));
            }
        } else {
            ordered_cmd_lists.push_back(&runtime.fallback_allocator->GetCmdList());
            submitted_allocators.push_back(std::move(runtime.fallback_allocator));
            // Keep every abandoned recorder pool alive and retire/reset it as
            // part of the same timeline transaction, but never submit its CB.
            for (ParallelChunkRuntime& chunk : runtime.chunks) {
                abandoned_allocators.push_back(std::move(chunk.allocator));
            }
        }
    }
    ordered_cmd_lists.push_back(&tracker_owner->GetCmdList());
    submitted_allocators.push_back(std::move(tracker_owner));

    Array<RHIResource*> deferred_releases = TakeDeferredReleases();
    const double profile_record_wall_ms = parallel_record_profile ?
                                              RhiThreadProfileMilliseconds(
                                                  _profile_started,
                                                  std::chrono::steady_clock::now()
                                              ) :
                                              0.0;
    CurrentVulkanRecordedSubmit recorded_submit{
        std::move(_submit), _context, _timeline
    };
    recorded_submit.presentation_source_program =
        std::move(_presentation_source_program);
    recorded_submit.has_commands      = true;
    recorded_submit.ordered_cmd_lists = std::move(ordered_cmd_lists);
    recorded_submit.allocators = VulkanAllocatorBatch{
        std::move(submitted_allocators), std::move(abandoned_allocators)
    };
    recorded_submit.descriptor_lease.emplace(std::move(descriptor_lease));
    recorded_submit.deferred_releases = std::move(deferred_releases);
    recorded_submit.profile.sample = {
        .requested       = true,
        .planned         = true,
        .effective       = parallel_recorded,
        .worker_fallback = !parallel_recorded,
        .record_wall_ms  = profile_record_wall_ms,
        .reorder_ms      = _reorder_time,
        .preprocess_ms   = preprocess_time,
        .worker_join_ms  = worker_join_time,
        .layers          = static_cast<uint32>(runtime_layers.size()),
        .jobs            = static_cast<uint32>(total_job_count),
        .work_units      = parallel_work_units,
        .max_active      = max_active_jobs.load(std::memory_order_relaxed),
    };
    if (parallel_record_profile) {
        recorded_submit.profile.enabled         = true;
        recorded_submit.profile.execute_started = _profile_started;
    }
    const uint32 recorded_cmd_list_count =
        static_cast<uint32>(recorded_submit.ordered_cmd_lists.size());
    CurrentVulkanSubmitResult submit_result{};
    if (_out_recorded != nullptr) {
        _out_recorded->emplace(std::move(recorded_submit));
    } else {
        submit_result = SubmitRecorded(recorded_submit);
    }

    QueryFrameDiagnostics query_diagnostics{};
    if (complete_profiling_frame) {
        execute_timer.Stop();
        query_diagnostics = profiler_storage.GetCurrentFrameQueryDiagnostics();
        const double execute_time = execute_timer.ElapsedMilliseconds();
        profiler_storage.RegisterCpuTimestamp("Queue Execution", execute_time);
        profiler_storage.RegisterCpuTimestamp("Command Reorder", _reorder_time);
        profiler_storage.RegisterCpuTimestamp("Command Preprocess", preprocess_time);
        if (execute_time > 0.0) {
            profiler_storage.RegisterCpuTimestamp("Reorder Percentage", _reorder_time / execute_time);
            profiler_storage.RegisterCpuTimestamp(
                "Preprocess Percentage", preprocess_time / execute_time
            );
        }
        profiler_storage.AdvanceFrame();
    } else if (emits_profiling_queries) {
        const bool submission_accepted =
            _out_recorded != nullptr || submit_result.outcome.WasSubmitted();
        if (!submission_accepted) {
            ResetSplitProfilingCpuFrame();
        } else {
            AccumulateSplitProfilingCpuFrame(_reorder_time, preprocess_time);
            if (ends_profiling_frame) {
                query_diagnostics = profiler_storage.GetCurrentFrameQueryDiagnostics();
                EndSplitProfilingCpuFrame();
            }
        }
    }

    if (parallel_record_verify) {
        try {
            LOG_INFO(
            "[ParallelRecord] batch={} requested=true effective={} outcome={} layers={} jobs={} "
            "work_units={} ordered_cb={} waves={} islands={} workers={} distinct_workers={} max_active={} "
            "coordinator={} submit_status={} "
            "descriptor_bytes={} query_digest={:016x} queries={}",
            batch_serial,
            parallel_recorded,
            parallel_recorded ? "parallel" : "serial-fallback-worker-failure",
            plan.parallel_layer_count,
            total_job_count,
            parallel_work_units,
            _out_recorded != nullptr ? recorded_cmd_list_count :
                                       submit_result.ordered_cmd_list_count,
            parallel_wave_count,
            serial_island_count,
            parallel_record_workers,
            distinct_workers.size(),
            max_active_jobs.load(std::memory_order_relaxed),
            Platform::GetCurrentThreadID(),
            _out_recorded != nullptr ? std::numeric_limits<uint32>::max() :
                                       static_cast<uint32>(submit_result.outcome.status),
            descriptor_bytes,
            query_diagnostics.digest,
            query_diagnostics.used_query_count
            );
        } catch (...) {
        }
    }
    return true;
}

void VkCommandQueue::ExecuteNow(
    CmdSubmit&& _submit,
    uint64 _timeline,
    uint64 _serial,
    std::optional<CurrentVulkanRecordedSubmit>* _out_recorded,
    bool _runtime_owner,
    bool* _out_terminalized
) {
    assert(
        _runtime_owner || !rhi_thread_enabled ||
        rhi_thread_id.load(std::memory_order_acquire) == Platform::GetCurrentThreadID()
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
        if (_out_recorded != nullptr) {
            _out_recorded->emplace(std::move(rejected));
        } else {
            CommitRuntimeSubmitCompletion(
                rejected, rejected.retirement_outcome
            );
            if (_out_terminalized != nullptr) {
                *_out_terminalized = true;
            }
        }
        return;
    }

    const size_t timestamp_query_slot_count = static_cast<size_t>(
        std::count_if(
            _submit.cmds.begin(),
            _submit.cmds.end(),
            [](const UniquePtr<Command>& _command) {
                return _command &&
                       _command->Type() == Command::EType::Query;
            }
        )
    );
    auto reject_before_native_record =
        [&](const VulkanOperationResult& _outcome,
            std::string_view             _reason,
            UniquePtr<VulkanAllocator>*  _abandoned_allocator = nullptr) {
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
            Array<UniquePtr<VulkanAllocator>> abandoned_allocators;
            if (_abandoned_allocator != nullptr &&
                *_abandoned_allocator) {
                // Complete every allocation before CmdSubmit moves into the
                // terminal packet. If reserve/emplace fails, the submit is
                // still owned by ExecuteNow and cannot notify on Translate.
                abandoned_allocators.reserve(1);
                abandoned_allocators.emplace_back(
                    std::move(*_abandoned_allocator)
                );
            }
            CurrentVulkanRecordedSubmit rejected{
                std::move(_submit), context, _timeline
            };
            rejected.allocators.abandoned =
                std::move(abandoned_allocators);
            rejected.retirement_outcome     = _outcome;
            rejected.recoverable_rejection =
                _outcome.status == EVulkanOperationStatus::Rejected &&
                !vk_device.IsFaulted() &&
                _outcome.result != VK_ERROR_DEVICE_LOST;
            rejected.native_submit_resolved = true;
            vk_device.RecordRejectedSubmit();
            if (_out_recorded != nullptr) {
                assert(
                    !_out_recorded->has_value() &&
                    "pre-record rejection cannot overwrite a recorded packet"
                );
                _out_recorded->emplace(std::move(rejected));
                return;
            }
            CommitRuntimeSubmitCompletion(
                rejected, rejected.retirement_outcome
            );
            if (_out_terminalized != nullptr) {
                *_out_terminalized = true;
            }
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

    const auto parallel_profile_started = parallel_record_profile ?
                                               std::chrono::steady_clock::now() :
                                               std::chrono::steady_clock::time_point{};
    Timer reorder_timer{};
    FunctionTable function_table{
        .is_resource_write       = &IsBufferTextureWrite,
        .is_resource_read        = &IsBufferTextureRead,
        .is_texture_sampled      = &IsTextureSampled,
        .is_resource_in_bindless = &IsResourceInBindlessArray,
        .lock_bdls_array         = &LockBindlessArray,
        .unlock_bdls_array       = &UnlockBindlessArray
    };
    CmdReorderer reorderer{function_table, _submit.cached_args};
    // Prepare the dependency layers exactly once. A profitability fallback
    // reuses this immutable reorder result on the serial recorder.
    reorder_timer.Start();
    for (const auto& cmd : _submit.cmds) {
        reorderer.AcceptCmd(cmd.get());
    }
    reorder_timer.Stop();
    const double reorder_time = reorder_timer.ElapsedMilliseconds();

    if (TryExecuteParallel(
            _submit,
            _timeline,
            _serial,
            context,
            function_table,
            reorderer,
            reorder_time,
            parallel_profile_started,
            presentation_source_program,
            _out_recorded
        )) {
        if (_out_terminalized != nullptr && _out_recorded != nullptr &&
            !_out_recorded->has_value()) {
            *_out_terminalized = true;
        }
        return;
    }

    UniquePtr<RhiRecordExecuteSample> record_sample;
    UniquePtr<VulkanSerialGoldenTrace> serial_golden;
    UnorderedMap<const Command*, uint32_t> original_ordinals;
    if (thread_profile && queue.GetType() == EQueueType::Graphics) {
        EnsureRecordCalibration();
        record_sample = MakeUnique<RhiRecordExecuteSample>();
        serial_golden = MakeUnique<VulkanSerialGoldenTrace>();
        original_ordinals.reserve(_submit.cmds.size());
        for (uint32_t ordinal = 0; ordinal < _submit.cmds.size(); ++ordinal) {
            const Command* command = _submit.cmds[ordinal].get();
            original_ordinals.emplace(command, ordinal);
            serial_golden->PrimeCommandResources(command, ordinal, _submit.cached_args);
        }
    }

    Timer timer{};

    //Get Allocators for buffer, texture and commandlist
    auto  allocator_ptr = std::move(GetAllocator());
    auto& vk_allocator  = *allocator_ptr;
    if (timestamp_query_slot_count != 0) {
        const VulkanScriptedQueryPreparationOverrideForTesting*
            scripted_override =
                g_scripted_query_preparation_override.load(
                    std::memory_order_acquire
                );
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
                    VulkanOperationResult{
                        EVulkanOperationStatus::Rejected,
                        scripted_result
                    },
                    "scripted timestamp query preparation rejection",
                    &allocator_ptr
                );
                return;
            }
        }
    }
    try {
        // Explicit Query commands force this source onto the serial recorder.
        // Size (and lazily create) its allocator-local pool before Begin() so
        // a legal large query packet cannot fail after native recording has
        // already produced side effects.
        vk_allocator.EnsureTimestampQueryCapacity(
            timestamp_query_slot_count
        );
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] timestamp query pool preparation "
                "failed: {}",
                error.what()
            );
        } catch (...) {
        }
        const bool device_faulted = vk_device.IsFaulted();
        reject_before_native_record(
            VulkanOperationResult{
                device_faulted ?
                    EVulkanOperationStatus::Faulted :
                    EVulkanOperationStatus::Rejected,
                device_faulted ?
                    vk_device.GetFirstFaultResult() :
                    VK_ERROR_OUT_OF_POOL_MEMORY
            },
            "timestamp query pool preparation failed",
            &allocator_ptr
        );
        return;
    } catch (...) {
        const bool device_faulted = vk_device.IsFaulted();
        reject_before_native_record(
            VulkanOperationResult{
                device_faulted ?
                    EVulkanOperationStatus::Faulted :
                    EVulkanOperationStatus::Rejected,
                device_faulted ?
                    vk_device.GetFirstFaultResult() :
                    VK_ERROR_OUT_OF_POOL_MEMORY
            },
            "timestamp query pool preparation failed",
            &allocator_ptr
        );
        return;
    }

    //Get Resource State Tracker
    auto& tracker = vk_allocator.GetTracker();

    //Visitor for actual command recording
    VkCmdVisitor visitor(
        vk_device,
        vk_allocator,
        tracker,
        vk_allocator.GetCmdList(),
        _submit.cached_args,
        &profiler_storage,
        emits_profiling_queries
    );

    //Visitor for barrier generation
    VkCmdPreprocessor preprocessor(
        vk_device,
        tracker,
        vk_allocator,
        function_table,
        _submit.cached_args,
        queue.GetType()
    );

    // LOG_INFO("Reorderer time {}", timer.ElapsedMilliseconds());
    const auto& cmd_lists = reorderer.m_cmd_lists;
    bool        has_cmd   = !reorderer.m_cmd_lists.empty();
    std::optional<VulkanDescriptorPushLease> descriptor_lease;
    uint64      descriptor_begin_offset = 0;

    if (record_sample) {
        RecordTopologyBuilder topology_builder;
        uint32                raw_layer_index = 0;
        record_sample->layer_timings.resize(cmd_lists.size());
        for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
            topology_builder.BeginLayer(raw_layer_index++);
            serial_golden->BeginLayer(raw_layer_index - 1);
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                topology_builder.AddCommand(cmdnode->cmd->Type());
                const auto ordinal = original_ordinals.find(cmdnode->cmd);
                if (ordinal == original_ordinals.end()) {
                    serial_golden->MarkUnresolved();
                    serial_golden->RecordCommand(
                        cmdnode->cmd,
                        std::numeric_limits<uint32_t>::max(),
                        _submit.cached_args
                    );
                } else {
                    serial_golden->RecordCommand(
                        cmdnode->cmd, ordinal->second, _submit.cached_args
                    );
                }
            }
            topology_builder.EndLayer();
            serial_golden->EndLayer();
        }
        record_sample->topology = topology_builder.Finish();
    }

    auto record_pending_barriers = [&](uint64 _group_index) {
        const QueueFamilyIndices queue_families = vk_device.GetQueueFamilyIndices();
        const SerialQueueFamilyMap serial_queue_family_map{
            .graphics_family = queue_families.graphics.value_or(VK_QUEUE_FAMILY_IGNORED),
            .compute_family  = queue_families.compute.value_or(VK_QUEUE_FAMILY_IGNORED),
            .copy_family     = queue_families.transfer.value_or(VK_QUEUE_FAMILY_IGNORED),
            .ignored_family  = VK_QUEUE_FAMILY_IGNORED,
        };
        const BarrierSemanticDiagnostics barriers =
            tracker.GetPendingBarrierDiagnostics(
                serial_golden.get(), _group_index, serial_queue_family_map
            );
        record_sample->barrier_hash.Add(_group_index);
        record_sample->barrier_hash.Add(barriers.digest);
        record_sample->barrier_hash.Add(barriers.buffer_count);
        record_sample->barrier_hash.Add(barriers.texture_count);
        record_sample->barrier_hash.Add(barriers.memory_count);
        record_sample->buffer_barriers += barriers.buffer_count;
        record_sample->texture_barriers += barriers.texture_count;
        record_sample->memory_barriers += barriers.memory_count;
    };

    //Set Descriptor buffer ringbuffer offset and start debug region
    double preprocess_time = 0.0;
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

        Array<UniquePtr<VulkanAllocator>> abandoned_allocators;
        if (allocator_ptr) {
            abandoned_allocators.push_back(std::move(allocator_ptr));
        }
        Array<RHIResource*> deferred_releases = TakeDeferredReleases();
        CurrentVulkanRecordedSubmit rejected{
            std::move(_submit), context, _timeline
        };
        rejected.allocators.abandoned = std::move(abandoned_allocators);
        rejected.descriptor_lease     = std::move(descriptor_lease);
        rejected.deferred_releases    = std::move(deferred_releases);
        rejected.retirement_outcome = {
            EVulkanOperationStatus::Rejected, _result
        };
        rejected.native_submit_resolved = true;
        if (_out_recorded != nullptr) {
            _out_recorded->emplace(std::move(rejected));
        } else {
            CommitRuntimeSubmitCompletion(
                rejected, rejected.retirement_outcome
            );
            if (_out_terminalized != nullptr) {
                *_out_terminalized = true;
            }
        }
    };

    auto record_serial_commands = [&] {
    if (has_cmd) {
        const VkResult begin_result = vk_allocator.GetCmdList().Begin();
        if (begin_result != VK_SUCCESS) {
            throw SerialNativeRecordFailure{"command-buffer-begin-failed", begin_result};
        }
        if (begins_profiling_frame) {
            if (serial_golden) {
                serial_golden->SetCurrentCommand(std::numeric_limits<uint32_t>::max());
            }
            profiler_storage.CollectProfiling(vk_allocator.GetCmdList().GetHandle());
            if (serial_golden) {
                serial_golden->RecordQueryEvent(
                    SerialQueryEvent::Reset,
                    "Graphics Exec",
                    ProfilerStorage::kResetQueryStage
                );
            }
            {
                std::unique_lock<std::mutex> profiler_lock(profiler_mutex);
                cached_profiler_entry = profiler_storage.GetProfilerEntry();
            }
            profiler_storage.BeginProfilerSession(vk_allocator.GetCmdList(), "Graphics Exec");
            if (serial_golden) {
                serial_golden->RecordQueryEvent(
                    SerialQueryEvent::Begin,
                    "Graphics Exec",
                    ProfilerStorage::kBeginTimestampStage
                );
            }
            if (complete_profiling_frame) {
                ResetSplitProfilingCpuFrame();
                timer.Start();
            } else {
                BeginSplitProfilingCpuFrame();
            }
        }

        if (queue.GetType() != EQueueType::Copy) {
            // Present work advances the queue timeline without consuming descriptor storage.
            // Keep ring reuse aligned with the execute-only in-flight limit.
            descriptor_lease.emplace(
                vk_device.GetGlobalDescriptorHeap().BeginPushDescriptors(queue.GetType())
            );
            vk_allocator.GetCmdList().SetDescriptorPushLease(descriptor_lease->state);
            if (record_sample) {
                descriptor_begin_offset = vk_device.GetGlobalDescriptorHeap()
                                              .CurrentPushDescriptorOffset(descriptor_lease->state);
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
        vk_allocator.GetCmdList().BeginLabel(queue_label, queue_label_color);
    }

    uint layer = 0;
    if (record_sample) {
        uint32 raw_layer_index = 0;
        for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
            const uint32 current_raw_layer = raw_layer_index++;
            if (cmd_list.head == nullptr) {
                continue;
            }
            if (layer == 0) {
                vk_allocator.GetCmdList().BeginLabel(
                    "[RHI Diagnostics] Command Layers", {0.15f, 0.25f, 0.55f, 1.0f}
                );
            }
            reorder_timer.Start();
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                preprocessor.VisitCmd(cmdnode->cmd);
                serial_golden->RegisterDerivedResources(cmdnode->cmd);
            }
            tracker.ResolveBarriers();
            record_pending_barriers(current_raw_layer);
            tracker.DispatchBarriers(vk_allocator.GetCmdList());
            reorder_timer.Stop();
            preprocess_time += reorder_timer.ElapsedMilliseconds();
            vk_allocator.GetCmdList().InsertLabel(
                std::format("[RHI] Layer {}", layer++), {0.15f, 0.25f, 0.55f, 1.0f}
            );
            RecordLayerTiming& layer_timing = record_sample->layer_timings[current_raw_layer];
            struct DeferredCommandDiagnostics {
                const Command* cmd{nullptr};
                uint64         descriptor_begin{0};
                uint64         descriptor_bytes{0};
                double         command_ms{0.0};
            };
            Array<DeferredCommandDiagnostics> deferred_diagnostics(
                record_sample->topology.layer_command_counts[current_raw_layer]
            );
            size_t     deferred_index = 0;
            const auto layer_started  = std::chrono::steady_clock::now();
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                const auto* cmd = cmdnode->cmd;
                const uint64 descriptor_before =
                    vk_device.GetGlobalDescriptorHeap().CurrentPushDescriptorOffset(
                        descriptor_lease->state
                    );
                const auto  command_started = std::chrono::steady_clock::now();
                visitor.VisitCmd(cmd);
                const auto command_finished = std::chrono::steady_clock::now();
                const uint64 descriptor_after =
                    vk_device.GetGlobalDescriptorHeap().CurrentPushDescriptorOffset(
                        descriptor_lease->state
                    );
                assert(deferred_index < deferred_diagnostics.size());
                deferred_diagnostics[deferred_index++] = {
                    .cmd              = cmd,
                    .descriptor_begin = descriptor_before - descriptor_begin_offset,
                    .descriptor_bytes = descriptor_after - descriptor_before,
                    .command_ms       = RhiThreadProfileMilliseconds(
                        command_started, command_finished
                    ),
                };
            }
            const auto layer_finished = std::chrono::steady_clock::now();
            assert(deferred_index == deferred_diagnostics.size());
            layer_timing.wall_ms = RhiThreadProfileMilliseconds(layer_started, layer_finished);

            // Descriptor/query golden reconstruction is deliberately outside
            // the timed visitor loop so diagnostic hashing cannot inflate the
            // hypothetical parallel-recording opportunity.
            for (const DeferredCommandDiagnostics& diagnostics : deferred_diagnostics) {
                const Command* cmd     = diagnostics.cmd;
                const auto     ordinal = original_ordinals.find(cmd);
                if (ordinal == original_ordinals.end()) {
                    serial_golden->MarkUnresolved();
                    serial_golden->SetCurrentCommand(std::numeric_limits<uint32_t>::max());
                } else {
                    serial_golden->SetCurrentCommand(ordinal->second);
                }
                serial_golden->RecordDescriptorsForCommand(
                    cmd,
                    _submit.cached_args,
                    diagnostics.descriptor_begin,
                    diagnostics.descriptor_bytes
                );
                if (cmd->Type() == Command::EType::Query) {
                    const auto& query = *static_cast<const QueryCmd*>(cmd);
                    serial_golden->RecordQueryEvent(
                        query.IsBegin() ?
                            SerialQueryEvent::Begin :
                            SerialQueryEvent::End,
                        query.Token().Name(),
                        query.IsBegin() ?
                            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT :
                            VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT
                    );
                } else if (emits_profiling_queries && cmd->Type() == Command::EType::Scope) {
                    const auto& scope = *static_cast<const ScopeCmd*>(cmd);
                    if (scope.QueryTimestamp()) {
                        serial_golden->RecordQueryEvent(
                            scope.IsPush() ? SerialQueryEvent::Begin : SerialQueryEvent::End,
                            scope.ScopeName(),
                            scope.IsPush() ? ProfilerStorage::kBeginTimestampStage
                                           : ProfilerStorage::kEndTimestampStage
                        );
                    }
                } else if (emits_profiling_queries && cmd->Type() == Command::EType::ShaderDispatch) {
                    const auto& dispatch = *static_cast<const DispatchCmd*>(cmd);
                    if (dispatch.ProfileSectionName() != "Other") {
                        serial_golden->RecordQueryEvent(
                            SerialQueryEvent::Begin,
                            dispatch.ProfileSectionName(),
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                        );
                        serial_golden->RecordQueryEvent(
                            SerialQueryEvent::End,
                            dispatch.ProfileSectionName(),
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                        );
                    }
                }
                record_sample->serial_command_sum_ms += diagnostics.command_ms;
                if (IsParallelRecordCandidate(cmd->Type())) {
                    layer_timing.candidate_units_ms.push_back(diagnostics.command_ms);
                }
            }
        }
    } else {
        // Keep the profile-off path on the serial baseline: no diagnostic
        // counters, per-command clocks, hashes, allocations, formatting, or output.
        // The pre-existing preprocess timer and debug-label formatting remain unchanged.
        for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
            if (cmd_list.head == nullptr) {
                continue;
            }
            reorder_timer.Start();
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                preprocessor.VisitCmd(cmdnode->cmd);
            }
            tracker.ResolveBarriers();
            tracker.DispatchBarriers(vk_allocator.GetCmdList());
            reorder_timer.Stop();
            preprocess_time += reorder_timer.ElapsedMilliseconds();
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                visitor.VisitCmd(cmdnode->cmd);
            }
        }
    }
    if (record_sample && layer > 0) {
        vk_allocator.GetCmdList().EndLabel();
    }

    if (has_cmd) {
        if (!_submit.HasExplicitResourceStateOwnership()) {
            tracker.RestoreState();
        }
        if (record_sample) {
            record_pending_barriers(std::numeric_limits<uint64>::max());
        }
        tracker.DispatchBarriers(vk_allocator.GetCmdList());
        if (ends_profiling_frame) {
            profiler_storage.EndProfilerSession(vk_allocator.GetCmdList(), "Graphics Exec");
            if (serial_golden) {
                serial_golden->SetCurrentCommand(std::numeric_limits<uint32_t>::max());
                serial_golden->RecordQueryEvent(
                    SerialQueryEvent::End,
                    "Graphics Exec",
                    ProfilerStorage::kEndTimestampStage
                );
            }
        }
        vk_allocator.GetCmdList().EndLabel();
        const VkResult end_result = vk_allocator.GetCmdList().End();
        if (end_result != VK_SUCCESS) {
            throw SerialNativeRecordFailure{"command-buffer-end-failed", end_result};
        }
        if (queue.GetType() != EQueueType::Copy) {
            if (record_sample) {
                record_sample->descriptor_bytes =
                    vk_device.GetGlobalDescriptorHeap().CurrentPushDescriptorOffset(
                        descriptor_lease->state
                    ) - descriptor_begin_offset;
            }
            vk_device.GetGlobalDescriptorHeap().EndPushDescriptors(*descriptor_lease);
        }
        if (record_sample && ends_profiling_frame) {
            const QueryFrameDiagnostics query_diagnostics =
                profiler_storage.GetCurrentFrameQueryDiagnostics();
            record_sample->query_digest = query_diagnostics.digest;
            record_sample->used_queries = query_diagnostics.used_query_count;
        }
        tracker.Reset();
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

    if (record_sample) {
        StableRecordHash descriptor_hash;
        descriptor_hash.Add(record_sample->descriptor_bytes);
        record_sample->descriptor_digest = descriptor_hash.Value();
        if (!emits_profiling_queries) {
            StableRecordHash query_hash;
            query_hash.Add(0);
            record_sample->query_digest = query_hash.Value();
        }
        record_sample->serial_golden     = serial_golden->Finish();
        record_sample->golden_unresolved = serial_golden->UnresolvedCount();
        record_sample->golden_opaque     = serial_golden->OpaqueCount();
        record_sample->golden_unresolved_command_mask =
            serial_golden->UnresolvedCommandMask();
        record_sample->golden_opaque_command_mask = serial_golden->OpaqueCommandMask();
        record_sample->golden_unresolved_native_buffers =
            serial_golden->UnresolvedNativeBufferCount();
        record_sample->golden_unresolved_native_images =
            serial_golden->UnresolvedNativeImageCount();
        record_sample->golden_has_unresolved_buffer_barrier =
            serial_golden->HasUnresolvedBufferBarrier();
        if (record_sample->golden_has_unresolved_buffer_barrier) {
            record_sample->golden_first_unresolved_buffer_barrier =
                serial_golden->FirstUnresolvedBufferBarrier();
        }
        RecordRhiRecordProfile(*record_sample);
    }

    const uint32 serial_layer_count = static_cast<uint32>(cmd_lists.size());
    Array<RHIResource*> deferred_releases = TakeDeferredReleases();
    const double profile_record_wall_ms = parallel_record_profile ?
                                              RhiThreadProfileMilliseconds(
                                                  parallel_profile_started,
                                                  std::chrono::steady_clock::now()
                                              ) :
                                              0.0;
    Array<VulkanCmdList*> serial_cmd_lists;
    VulkanAllocatorBatch serial_allocators;
    if (has_cmd) {
        serial_cmd_lists.reserve(1);
        serial_cmd_lists.emplace_back(
            &vk_allocator.GetCmdList()
        );
    }
    serial_allocators.submitted.reserve(1);
    serial_allocators.submitted.emplace_back(
        std::move(allocator_ptr)
    );

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
    recorded_submit.allocators = std::move(serial_allocators);
    recorded_submit.descriptor_lease  = std::move(descriptor_lease);
    recorded_submit.deferred_releases = std::move(deferred_releases);
    recorded_submit.profile.sample = {
        .requested       = parallel_record_requested,
        .planned         = false,
        .effective       = false,
        .worker_fallback = false,
        .record_wall_ms  = profile_record_wall_ms,
        .reorder_ms      = reorder_time,
        .preprocess_ms   = preprocess_time,
        .worker_join_ms  = 0.0,
        .layers          = serial_layer_count,
        .jobs            = 0,
        .max_active      = 0,
    };
    if (parallel_record_profile) {
        recorded_submit.profile.enabled         = true;
        recorded_submit.profile.execute_started = parallel_profile_started;
    }
    CurrentVulkanSubmitResult submit_result{};
    if (_out_recorded != nullptr) {
        _out_recorded->emplace(std::move(recorded_submit));
    } else {
        submit_result = SubmitRecorded(recorded_submit);
    }

    if (has_cmd && complete_profiling_frame) {
        timer.Stop();
        profiler_storage.RegisterCpuTimestamp("Queue Execution", timer.ElapsedMilliseconds());
        profiler_storage.RegisterCpuTimestamp("Command Reorder", reorder_time);
        profiler_storage.RegisterCpuTimestamp("Command Preprocess", preprocess_time);
        profiler_storage.RegisterCpuTimestamp(
            "Reorder Percentage", reorder_time / timer.ElapsedMilliseconds()
        );
        profiler_storage.RegisterCpuTimestamp(
            "Preprocess Percentage", preprocess_time / timer.ElapsedMilliseconds()
        );
        profiler_storage.AdvanceFrame();
    } else if (has_cmd && emits_profiling_queries) {
        const bool submission_accepted =
            _out_recorded != nullptr || submit_result.outcome.WasSubmitted();
        if (!submission_accepted) {
            ResetSplitProfilingCpuFrame();
        } else {
            AccumulateSplitProfilingCpuFrame(reorder_time, preprocess_time);
            if (ends_profiling_frame) {
                EndSplitProfilingCpuFrame();
            }
        }
    }
}

void VkCommandQueue::Present(
    SwapchainRef      _sc,
    TextureView       _view,
    PresentReceiptRef _receipt
) {
    TextureRef source_texture{_view.texture};

    if (!ClaimLegacyOwnership("Present")) {
        if (_receipt) {
            _receipt->Resolve(false);
        }
        return;
    }
    if (vk_device.IsFaulted()) {
        vk_device.RecordRejectedPresent();
        if (_receipt) {
            _receipt->Resolve(false);
        }
        return;
    }

    if (rhi_thread_enabled) {
        std::chrono::steady_clock::time_point caller_started{};
        if (thread_profile) {
            caller_started = std::chrono::steady_clock::now();
        }
        {
            std::unique_lock<std::mutex> lock(rhi_work_mutex);
            const uint64 current_timeline = last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
            const uint64 serial           = ++enqueued_rhi_work;
            const uint32 enqueue_depth    = uint32(rhi_work_queue.size() + 1);
            const auto enqueued_at = thread_profile ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
            rhi_work_queue.emplace_back(
                std::in_place_type<RhiPresentWork>,
                std::move(_sc),
                std::move(source_texture),
                _view,
                std::move(_receipt),
                current_timeline,
                serial,
                enqueued_at,
                enqueue_depth
            );
            if (thread_profile) {
                std::get<RhiPresentWork>(rhi_work_queue.back()).caller_ms =
                    RhiThreadProfileMilliseconds(caller_started, std::chrono::steady_clock::now());
            }
        }
        rhi_work_cv.notify_one();
        return;
    }

    std::unique_lock<std::mutex> lock(exec_mtx);
    const uint64 current_timeline = last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (thread_profile) {
        const auto work_started = std::chrono::steady_clock::now();
        PresentNow(
            std::move(_sc),
            std::move(source_texture),
            _view,
            std::move(_receipt),
            current_timeline,
            current_timeline
        );
        const double work_ms =
            RhiThreadProfileMilliseconds(work_started, std::chrono::steady_clock::now());
        RecordThreadingProfile(ERhiWorkKind::Present, work_ms, 0.0, work_ms, 0);
    } else {
        PresentNow(
            std::move(_sc),
            std::move(source_texture),
            _view,
            std::move(_receipt),
            current_timeline,
            current_timeline
        );
    }
}

VulkanRuntimeSubmissionResult VkCommandQueue::PresentNow(
    SwapchainRef&& _sc,
    TextureRef&&   _source_texture,
    TextureView    _view,
    PresentReceiptRef _receipt,
    uint64         _timeline,
    uint64         _serial,
    bool           _runtime_owner
) noexcept {
    assert(
        _runtime_owner || !rhi_thread_enabled ||
        rhi_thread_id.load(std::memory_order_acquire) == Platform::GetCurrentThreadID()
    );

    const VulkanOperationContext context{
        .operation   = EVulkanFaultOperation::PresentSubmit,
        .queue_type  = queue.GetType(),
        .queue       = queue.GetHandle(),
        .timeline    = _timeline,
        .work_serial = _serial,
    };

    VulkanOperationResult final_outcome{
        EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
    };
    bool                            gpu_submitted = false;
    bool                            present_accepted = false;
    bool                            recreate_swapchain = false;
    bool                            recoverable_rejection = false;
    EVulkanPresentorRetirementState presentor_state =
        EVulkanPresentorRetirementState::Unused;
    UniquePtr<VulkanPresentor> presentor{};
    Array<RHIResource*>        deferred_releases{};

    try {
        const VulkanScriptedPresentOverrideForTesting*
            scripted_override = g_scripted_present_override.load(
                std::memory_order_acquire
            );
        if (vk_device.IsFaulted()) {
            vk_device.RecordRejectedPresent();
            final_outcome = {
                EVulkanOperationStatus::Rejected,
                vk_device.GetFirstFaultResult()
            };
        } else if (const VulkanPresentSourceContract source_contract =
                       ValidatePresentSourceContract(_view, _sc.Get());
                   !source_contract.valid) {
            InvokePresentSourceRejectionCallbackForTesting(
                scripted_override
            );
            final_outcome = ClassifyPresentSourceRejection(
                vk_device, recoverable_rejection
            );
            vk_device.RecordRejectedPresent();
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] rejected Present source contract: {}",
                    source_contract.reason
                );
            } catch (...) {
            }
        } else if (!static_cast<VulkanTexture*>(_view.texture)
                        ->IsPresentationSourceReady()) {
            InvokePresentSourceRejectionCallbackForTesting(
                scripted_override
            );
            final_outcome = ClassifyPresentSourceRejection(
                vk_device, recoverable_rejection
            );
            vk_device.RecordRejectedPresent();
        } else {
            auto* vk_source_texture =
                static_cast<VulkanTexture*>(_view.texture);
            VkSwapchain* sc = ResourceCast(_sc.Get());
            presentor       = GetPresentor();
            if (!sc->WaitFrameInFlight()) {
                vk_device.RecordRejectedPresent();
                final_outcome = {
                    EVulkanOperationStatus::Rejected,
                    GetRejectedSubmitResult(vk_device)
                };
            } else {
                const VkSwapchain::AcquireResult acquire =
                    sc->AquireNextImage(
                        presentor->GetImageReadySemaphore(),
                        rhi_thread_enabled ? 0 : 50'000'000
                    );
                final_outcome      = acquire.outcome;
                recreate_swapchain =
                    acquire.outcome.status == EVulkanOperationStatus::Recreate;

                if (acquire.HasImage()) {
                    presentor_state =
                        EVulkanPresentorRetirementState::RecordedNotSubmitted;
                    auto& vk_allocator = *presentor;
                    auto& vk_cmd_list  = vk_allocator.GetCmdList();
                    auto& vk_tracker   = vk_allocator.GetTracker();
                    auto* vk_src_tex   = vk_source_texture;
                    const uint idx     = acquire.image_index;
                    auto* swapchain_tex =
                        ResourceCast(sc->GetSwapchainImage(idx).texture);

                    VK_CHECK_RESULT(vk_cmd_list.Begin());
                    const std::string present_label =
                        std::format("Present: {}", vk_src_tex->GetName());
                    vk_cmd_list.BeginLabel(
                        present_label, {0.0f, 0.85f, 0.95f, 1.0f}
                    );
                    vk_tracker.SetPassType(EPassType::Graphics);
                    // PRESENTATION_SOURCE is a producer-side terminal
                    // contract. The active graph exports GENERAL with
                    // TRANSFER_READ visibility, while legacy recording
                    // restores these textures to their GENERAL preferred
                    // layout. Present consumes that state directly instead
                    // of guessing and changing the source layout here. A
                    // GENERAL->GENERAL memory barrier still connects the
                    // accepted producer writes to this copy.
                    vk_tracker.EmitExplicitBarrier(
                        vk_src_tex,
                        VulkanEnumTranslator::METoVKImageAspectFlags(
                            vk_src_tex->GetAspectFlags()
                        ),
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
                    vk_tracker.RecordState(
                        swapchain_tex,
                        vk_tracker.WriteTexture(
                            swapchain_tex, ETextureState::TRANSFER
                        )
                    );
                    vk_tracker.ResolveBarriers();
                    vk_tracker.DispatchBarriers(vk_cmd_list);
                    vk_cmd_list.InsertLabel(
                        "Copy Present Image", GpuMarkerPalette::Transfer()
                    );
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
                    vk_tracker.RecordState(
                        swapchain_tex,
                        VK_ACCESS_2_NONE,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                        VK_PIPELINE_STAGE_2_COPY_BIT
                    );
                    vk_tracker.ResolveBarriers();
                    vk_tracker.DispatchBarriers(vk_cmd_list);
                    vk_cmd_list.EndLabel();
                    VK_CHECK_RESULT(vk_cmd_list.End());
                    vk_tracker.Reset();

                    deferred_releases = TakeDeferredReleases();
                    queue.Signal(
                        timeline, _timeline, VK_PIPELINE_STAGE_2_COPY_BIT
                    );
                    queue.Wait(
                        acquire.ready_semaphore,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                    );
                    queue.Signal(
                        sc->GetRenderFinishedFence(idx),
                        VK_PIPELINE_STAGE_2_COPY_BIT
                    );
                    const VulkanOperationResult submit_outcome =
                        queue.Submit(vk_allocator.GetCmdList(), context);
                    final_outcome = submit_outcome;
                    gpu_submitted = submit_outcome.WasSubmitted();
                    if (gpu_submitted) {
                        presentor_state =
                            EVulkanPresentorRetirementState::Submitted;
                        timeline->MarkSubmitted(_timeline);

                        const VulkanOperationResult present_outcome =
                            sc->Present(
                                queue.GetHandle(), idx, _timeline, _serial
                            );
                        present_accepted =
                            present_outcome.result == VK_SUCCESS ||
                            present_outcome.result == VK_SUBOPTIMAL_KHR;
                        recreate_swapchain =
                            recreate_swapchain ||
                            present_outcome.status ==
                                EVulkanOperationStatus::Recreate;
                        if (!present_outcome.Succeeded()) {
                            final_outcome = present_outcome;
                        }
                    }
                } else {
                    deferred_releases = TakeDeferredReleases();
                }
            }
        }
    } catch (const std::exception& error) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedPresent();
        final_outcome = {
            EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
        };
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] Present threw before retirement: {}",
                error.what()
            );
        } catch (...) {
        }
    } catch (...) {
        queue.DiscardPendingSubmitState();
        vk_device.RecordRejectedPresent();
        final_outcome = {
            EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN
        };
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Present threw before retirement");
        } catch (...) {
        }
    }

    CommitPresentCompletion(
        final_outcome,
        context,
        gpu_submitted,
        presentor_state,
        std::move(presentor),
        std::move(_sc),
        std::move(_source_texture),
        std::move(deferred_releases),
        _timeline
    );

    ResolvePresentReceiptNoexcept(
        _receipt, present_accepted, recreate_swapchain
    );

    VulkanRuntimeSubmissionResult runtime_result{
        .outcome = final_outcome,
        .recoverable_rejection = recoverable_rejection,
    };
    if (gpu_submitted) {
        runtime_result.completion = WaitEvent{uint64(timeline), _timeline};
    }
    return runtime_result;
}

void VkCommandQueue::CommitPresentCompletion(
    const VulkanOperationResult&    _outcome,
    const VulkanOperationContext&   _context,
    bool                            _gpu_submitted,
    EVulkanPresentorRetirementState _presentor_state,
    UniquePtr<VulkanPresentor>&&    _presentor,
    SwapchainRef&&                  _swapchain,
    TextureRef&&                    _source_texture,
    Array<RHIResource*>&&           _deferred_releases,
    uint64                          _timeline
) noexcept {
    try {
        if (!_gpu_submitted &&
            (_outcome.status == EVulkanOperationStatus::Rejected ||
             _outcome.status == EVulkanOperationStatus::Faulted)) {
            TerminalizeUnsubmittedSignal(
                vk_device, timeline, _timeline, _outcome
            );
        }
        std::unique_lock<std::mutex> lock(event_mutex);
        const uint64 retirement_serial = retirement_enqueued_serial + 1;
        event_queue.emplace_back(
            std::in_place_type<VulkanPresentCompletionBatch>,
            _timeline,
            true,
            retirement_serial,
            _outcome,
            _context,
            _gpu_submitted,
            true,
            _presentor_state,
            std::move(_presentor),
            std::move(_swapchain),
            std::move(_source_texture),
            std::move(_deferred_releases)
        );
        retirement_enqueued_serial = retirement_serial;
    } catch (...) {
        // A submitted presentor must never unwind on the Submission owner.
        std::terminate();
    }
    queue_cv.notify_one();
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
    assert(
        !rhi_thread_enabled ||
        rhi_thread_id.load(std::memory_order_acquire) != Platform::GetCurrentThreadID()
    );

    uint64 target_timeline = 0;
    uint64 target_work     = 0;
    if (rhi_thread_enabled) {
        std::unique_lock<std::mutex> lock(rhi_work_mutex);
        target_work     = enqueued_rhi_work;
        target_timeline = last_frame.load(std::memory_order_relaxed);
        rhi_work_done_cv.wait(lock, [this, target_work]() {
            return completed_rhi_work >= target_work;
        });
    } else {
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
    if (rhi_thread_enabled) {
        std::unique_lock<std::mutex> lock(rhi_work_mutex);
        if (enqueued_rhi_work == target_work && completed_rhi_work >= target_work) {
            vk_device.deferred_release_queue.PopAll(deleted_resources);
        }
    } else {
        std::unique_lock<std::mutex> lock(exec_mtx);
        if (last_frame.load(std::memory_order_relaxed) == target_timeline) {
            vk_device.deferred_release_queue.PopAll(deleted_resources);
        }
    }
    for (auto* resource : deleted_resources) {
        MoerDelete(resource);
    }
}

ProfileData VkCommandQueue::GetProfilerEntry() {
    std::unique_lock<std::mutex> lock(profiler_mutex);
    return cached_profiler_entry;
}

UniquePtr<VulkanAllocator> VkCommandQueue::GetAllocator() {
    // Completion publishes only successfully completed and reset allocators
    // into this pool. Pool membership is therefore the reuse gate; if
    // Completion is delayed by unrelated CPU callbacks, allocate overflow
    // storage instead of stalling the Translate owner.
    auto allocator = std::move(UniquePtr<VulkanAllocator>(allocators.Pop()));
    if (allocator) {
        return std::move(allocator);
    }
    return MakeUnique<VulkanAllocator>(&vk_device, queue.GetType());
}

UniquePtr<VulkanPresentor> VkCommandQueue::GetPresentor() {
    // Presentors follow the same completion-owned pool contract as command
    // allocators. A temporary pool miss is capacity pressure, not a reason for
    // Submission to wait on arbitrary completion callbacks.
    auto presentor = std::move(UniquePtr<VulkanPresentor>(presentors.Pop()));
    if (presentor) {
        return std::move(presentor);
    }
    return MakeUnique<VulkanPresentor>(&vk_device, queue.GetType());
}

Array<RHIResource*> VkCommandQueue::TakeDeferredReleases() {
    if (queue.GetType() != EQueueType::Graphics) {
        return {};
    }

    Array<RHIResource*> deleted_resources;
    if (rhi_thread_enabled) {
        std::unique_lock<std::mutex> lock(rhi_work_mutex);
        if (!rhi_work_queue.empty()) {
            return {};
        }
        vk_device.deferred_release_queue.PopAll(deleted_resources);
    } else {
        vk_device.deferred_release_queue.PopAll(deleted_resources);
    }

    return deleted_resources;
}

void VkCommandQueue::EnqueueCompletionMarker(uint64 _timeline) {
    const uint64 retirement_serial = retirement_enqueued_serial + 1;
    event_queue.emplace_back(
        VulkanCompletionMarker{}, _timeline, true, retirement_serial
    );
    retirement_enqueued_serial = retirement_serial;
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
    if (_sample.serial_golden.complete) {
        ++totals.golden_complete;
    } else {
        ++totals.golden_incomplete;
    }
    totals.golden_unresolved_total += _sample.golden_unresolved;
    totals.golden_opaque_total += _sample.golden_opaque;
    totals.golden_command_digests.insert(_sample.serial_golden.command_digest);
    totals.golden_layer_digests.insert(_sample.serial_golden.layer_digest);
    totals.golden_barrier_digests.insert(_sample.serial_golden.barrier_digest);
    totals.golden_descriptor_digests.insert(_sample.serial_golden.descriptor_digest);
    totals.golden_query_digests.insert(_sample.serial_golden.query_digest);
    totals.golden_combined_digests.insert(_sample.serial_golden.combined_digest);
    StableRecordHash manifest_entry_hash;
    manifest_entry_hash.Add(_sample.topology.topology_digest);
    manifest_entry_hash.Add(_sample.serial_golden.combined_digest);
    manifest_entry_hash.Add(_sample.serial_golden.complete ? 1u : 0u);
    totals.golden_manifest_entries.insert(manifest_entry_hash.Value());
    const bool is_new_manifest_entry =
        profile.observed_golden_manifests.insert(manifest_entry_hash.Value()).second;

    if (is_new_manifest_entry) {
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
            _sample.serial_golden.complete ? 1 : 0,
            _sample.topology.topology_digest,
            _sample.serial_golden.combined_digest,
            _sample.serial_golden.command_digest,
            _sample.serial_golden.layer_digest,
            _sample.serial_golden.barrier_digest,
            _sample.serial_golden.descriptor_digest,
            _sample.serial_golden.query_digest,
            _sample.serial_golden.command_count,
            _sample.serial_golden.layer_count,
            _sample.serial_golden.barrier_count,
            _sample.serial_golden.descriptor_count,
            _sample.serial_golden.query_count,
            _sample.golden_unresolved,
            _sample.golden_opaque,
            _sample.golden_unresolved_command_mask,
            _sample.golden_opaque_command_mask,
            _sample.golden_unresolved_native_buffers,
            _sample.golden_unresolved_native_images,
            _sample.golden_has_unresolved_buffer_barrier
                ? _sample.golden_first_unresolved_buffer_barrier.group_ordinal
                : 0,
            _sample.golden_has_unresolved_buffer_barrier
                ? _sample.golden_first_unresolved_buffer_barrier.src_stage_mask
                : 0,
            _sample.golden_has_unresolved_buffer_barrier
                ? _sample.golden_first_unresolved_buffer_barrier.dst_stage_mask
                : 0,
            _sample.golden_has_unresolved_buffer_barrier
                ? _sample.golden_first_unresolved_buffer_barrier.src_access_mask
                : 0,
            _sample.golden_has_unresolved_buffer_barrier
                ? _sample.golden_first_unresolved_buffer_barrier.dst_access_mask
                : 0,
            _sample.golden_has_unresolved_buffer_barrier
                ? _sample.golden_first_unresolved_buffer_barrier.range_offset
                : 0,
            _sample.golden_has_unresolved_buffer_barrier
                ? _sample.golden_first_unresolved_buffer_barrier.range_size
                : 0
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
        "work_units_avg={:.3f} ordered_cb_avg={:.3f} max_active={}",
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

void VkCommandQueue::RhiThreadMain() {
    Platform::SetCurrentThreadName(
        queue.GetType() == EQueueType::Graphics ? "Moer RHI Thread" : "Moer Compute RHI"
    );
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Submission);
    rhi_thread_id.store(Platform::GetCurrentThreadID(), std::memory_order_release);
    LOG_INFO(
        "[Threading] RHIThread id = {}, queue = {}",
        rhi_thread_id.load(std::memory_order_relaxed),
        queue.GetType() == EQueueType::Graphics ? "Graphics" : "Compute"
    );

    while (true) {
        std::optional<RhiWork> work;
        {
            std::unique_lock<std::mutex> lock(rhi_work_mutex);
            rhi_work_cv.wait(lock, [this]() {
                return !rhi_worker_running || !rhi_work_queue.empty();
            });
            if (!rhi_worker_running && rhi_work_queue.empty()) {
                break;
            }

            work.emplace(std::move(rhi_work_queue.front()));
            rhi_work_queue.pop_front();
        }

        const uint64 serial = std::visit([](const auto& _work) { return _work.serial; }, *work);
        {
            std::unique_lock<std::mutex> lock(exec_mtx);
            auto execute_work = [&] {
                std::visit(
                    Overload{
                        [this](RhiExecuteWork& _work) {
                            ExecuteNow(std::move(_work.submit), _work.timeline, _work.serial);
                        },
                        [this](RhiPresentWork& _work) {
                            PresentNow(
                                std::move(_work.swapchain),
                                std::move(_work.source_texture),
                                _work.source_view,
                                std::move(_work.receipt),
                                _work.timeline,
                                _work.serial
                            );
                        }
                    },
                    *work
                );
            };
            if (thread_profile) {
                const auto enqueued_at =
                    std::visit([](const auto& _work) { return _work.enqueued_at; }, *work);
                const uint32 enqueue_depth =
                    std::visit([](const auto& _work) { return _work.enqueue_depth; }, *work);
                const double caller_ms =
                    std::visit([](const auto& _work) { return _work.caller_ms; }, *work);
                const ERhiWorkKind work_kind = std::holds_alternative<RhiExecuteWork>(*work)
                                                   ? ERhiWorkKind::Execute
                                                   : ERhiWorkKind::Present;
                const auto work_started = std::chrono::steady_clock::now();
                execute_work();
                const auto work_finished = std::chrono::steady_clock::now();
                RecordThreadingProfile(
                    work_kind,
                    caller_ms,
                    RhiThreadProfileMilliseconds(enqueued_at, work_started),
                    RhiThreadProfileMilliseconds(work_started, work_finished),
                    enqueue_depth
                );
            } else {
                execute_work();
            }
        }

        work.reset();
        {
            std::unique_lock<std::mutex> lock(rhi_work_mutex);
            completed_rhi_work = serial;
        }
        rhi_work_done_cv.notify_all();
    }

    LOG_INFO("[Threading] RHIThread id = {} stopped", Platform::GetCurrentThreadID());
}

void VkCommandQueue::CompletionThreadMain() {
    Platform::SetCurrentThreadName(
        queue.GetType() == EQueueType::Graphics ? "Vulkan Gfx Completion" : "Vulkan Compute Completion"
    );
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Completion);

    bool batch_gpu_success = false;
    bool batch_release_safe = false;
    while (true) {
        std::optional<QueueEvent> evt;
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            queue_cv.wait(lock, [this]() {
                return !completion_worker_running || !event_queue.empty();
            });
            if (!completion_worker_running && event_queue.empty()) {
                break;
            }

            evt.emplace(std::move(event_queue.front()));
            event_queue.pop_front();
        }

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

                    VulkanOperationContext wait_context = _submission.context;
                    wait_context.operation = EVulkanFaultOperation::TimelineHostWait;
                    const VkResult wait_result = timeline->HostWait(event_timeline, wait_context);
                    if (wait_result == VK_SUCCESS && !vk_device.IsDeviceLost()) {
                        timeline->Notify(event_timeline);
                        batch_gpu_success = true;
                        batch_release_safe = true;
                    } else {
                        timeline->Fail(wait_result);
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
                        VulkanOperationContext wait_context = _batch.context;
                        wait_context.operation = EVulkanFaultOperation::TimelineHostWait;
                        const VkResult wait_result =
                            timeline->HostWait(event_timeline, wait_context);
                        if (wait_result == VK_SUCCESS && !vk_device.IsFaulted()) {
                            batch_gpu_success = true;
                            batch_release_safe = true;
                            completion_result = VK_SUCCESS;

                            for (auto& allocator : _batch.allocators.submitted) {
                                const VkResult query_result =
                                    allocator->PrepareTimestampQueriesAfterGpuCompletion(
                                        _batch.context
                                    );
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

                            if (batch_gpu_success) {
                                timeline->Notify(event_timeline);
                            } else if (_batch.owns_queue_timeline) {
                                timeline->Fail(completion_result);
                            }
                        } else {
                            completion_result =
                                wait_result != VK_SUCCESS ?
                                    wait_result :
                                vk_device.IsFaulted() ?
                                    vk_device.GetFirstFaultResult() :
                                    VK_ERROR_DEVICE_LOST;
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
                            reinterpret_cast<VulkanFence*>(event.timeline_handle);
                        if (fence == nullptr) {
                            continue;
                        }
                        if (batch_gpu_success) {
                            fence->Notify(event.value);
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
                            "Vulkan timestamp query did not resolve after GPU completion" :
                            "Vulkan submission did not reach successful GPU completion";
                    for (auto& allocator : _batch.allocators.submitted) {
                        allocator->PublishTimestampQueriesAfterGpuCompletion(
                            query_batch,
                            batch_gpu_success,
                            query_failure_reason
                        );
                    }
                    for (auto& allocator : _batch.allocators.abandoned) {
                        allocator->PublishTimestampQueriesAfterGpuCompletion(
                            query_batch,
                            false,
                            "Vulkan timestamp query recorder was abandoned before native submission"
                        );
                    }
                    // Every CmdSubmit token must become terminal even if a
                    // backend bug failed to associate it with a native
                    // allocator record. Publish the fallback in the same
                    // transaction, but defer all notifications.
                    _batch.submit.PublishPendingQueryErrors(
                        query_failure_reason, query_batch
                    );

                    bool recycle_batch =
                        batch_gpu_success && !vk_device.IsFaulted();
                    if (recycle_batch) {
                        for (auto& allocator : _batch.allocators.submitted) {
                            recycle_batch =
                                allocator->CompleteSuccessCallbacks() &&
                                recycle_batch;
                        }
                    }

                    if (_batch.descriptor_lease.has_value()) {
                        vk_device.GetGlobalDescriptorHeap().RecyclePushDescriptors(
                            std::move(*_batch.descriptor_lease)
                        );
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
                        _batch.submit.NotifyPendingQueries(query_batch);
                        for (auto& allocator : _batch.allocators.submitted) {
                            allocator->NotifyTimestampQueriesAfterGpuCompletion(
                                query_batch
                            );
                        }
                        for (auto& allocator : _batch.allocators.abandoned) {
                            allocator->NotifyTimestampQueriesAfterGpuCompletion(
                                query_batch
                            );
                        }
                        _batch.submit.query_tokens.clear();
                    }

                    if (recycle_batch) {
                        for (auto& allocator : _batch.allocators.submitted) {
                            recycle_batch = allocator->Reset() && recycle_batch;
                        }
                    }
                    if (recycle_batch && !vk_device.IsFaulted()) {
                        for (auto& allocator : _batch.allocators.submitted) {
                            allocators.Push(allocator.release());
                        }
                    } else {
                        for (auto& allocator : _batch.allocators.submitted) {
                            if (!allocator) {
                                continue;
                            }
                            vk_device.RecordAllocatorQuarantine();
                            vk_device.RecordSkippedCommandPoolReset();
                            allocator_quarantine.emplace_back(std::move(allocator));
                        }
                    }
                    for (auto& allocator : _batch.allocators.abandoned) {
                        if (!vk_device.IsFaulted() && allocator->ResetAbandoned()) {
                            allocators.Push(allocator.release());
                        } else if (allocator) {
                            vk_device.RecordAllocatorQuarantine();
                            vk_device.RecordSkippedCommandPoolReset();
                            allocator_quarantine.emplace_back(std::move(allocator));
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
                },
                [this, event_timeline, &batch_gpu_success, &batch_release_safe](
                    VulkanPresentCompletionBatch& _batch
                ) {
                    batch_gpu_success = false;
                    batch_release_safe =
                        !_batch.gpu_submitted &&
                        _batch.presentor_state ==
                            EVulkanPresentorRetirementState::Unused &&
                        (_batch.outcome.status == EVulkanOperationStatus::Retry ||
                         _batch.outcome.status ==
                             EVulkanOperationStatus::Recreate);

                    VkResult completion_result = _batch.outcome.result;
                    if (_batch.gpu_submitted) {
                        VulkanOperationContext wait_context = _batch.context;
                        wait_context.operation =
                            EVulkanFaultOperation::TimelineHostWait;
                        const VkResult wait_result =
                            timeline->HostWait(event_timeline, wait_context);
                        if (wait_result == VK_SUCCESS &&
                            !vk_device.IsDeviceLost()) {
                            timeline->Notify(event_timeline);
                            batch_gpu_success = true;
                            batch_release_safe = true;
                            completion_result = VK_SUCCESS;
                        } else {
                            completion_result =
                                wait_result != VK_SUCCESS ?
                                    wait_result :
                                vk_device.IsFaulted() ?
                                    vk_device.GetFirstFaultResult() :
                                    VK_ERROR_DEVICE_LOST;
                            if (_batch.owns_queue_timeline) {
                                timeline->Fail(completion_result);
                            }
                        }
                    } else if (
                        _batch.owns_queue_timeline &&
                        _batch.outcome.status ==
                            EVulkanOperationStatus::Faulted
                    ) {
                        if (completion_result == VK_SUCCESS) {
                            completion_result = VK_ERROR_UNKNOWN;
                        }
                        timeline->Fail(completion_result);
                    } else if (
                        _batch.owns_queue_timeline &&
                        _batch.outcome.status ==
                            EVulkanOperationStatus::Rejected
                    ) {
                        timeline->Reject(event_timeline);
                    }

                    if (_batch.presentor) {
                        bool recycle_presentor = false;
                        if (_batch.presentor_state ==
                                EVulkanPresentorRetirementState::Submitted &&
                            batch_gpu_success && !vk_device.IsFaulted()) {
                            recycle_presentor =
                                _batch.presentor->CompleteSuccess() &&
                                _batch.presentor->Reset();
                        } else if (
                            _batch.presentor_state ==
                                EVulkanPresentorRetirementState::Unused &&
                            batch_release_safe && !vk_device.IsFaulted()
                        ) {
                            // No command buffer was begun, matching the
                            // previous acquire-retry fast recycle path.
                            recycle_presentor = true;
                        }

                        if (recycle_presentor && !vk_device.IsFaulted()) {
                            presentors.Push(_batch.presentor.release());
                        } else {
                            vk_device.RecordAllocatorQuarantine();
                            vk_device.RecordSkippedCommandPoolReset();
                            presentor_quarantine.emplace_back(
                                std::move(_batch.presentor)
                            );
                        }
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
                    _batch.deferred_releases.clear();
                },
                [this, &batch_gpu_success](UniquePtr<VulkanAllocator>& _allocator) {
                    if (batch_gpu_success && !vk_device.IsFaulted()) {
                        if (_allocator->CompleteSuccess() && _allocator->Reset()) {
                            allocators.Push(_allocator.release());
                            return;
                        }
                    }
                    vk_device.RecordAllocatorQuarantine();
                    vk_device.RecordSkippedCommandPoolReset();
                    allocator_quarantine.emplace_back(std::move(_allocator));
                },
                [this, &batch_gpu_success](VulkanAllocatorBatch& _batch) {
                    bool recycle_batch = batch_gpu_success && !vk_device.IsFaulted();
                    if (recycle_batch) {
                        for (auto& allocator : _batch.submitted) {
                            recycle_batch =
                                allocator->CompleteSuccess() && recycle_batch;
                        }
                    }
                    if (recycle_batch) {
                        for (auto& allocator : _batch.submitted) {
                            recycle_batch = allocator->Reset() && recycle_batch;
                        }
                    }

                    if (recycle_batch && !vk_device.IsFaulted()) {
                        for (auto& allocator : _batch.submitted) {
                            allocators.Push(allocator.release());
                        }
                    } else {
                        for (auto& allocator : _batch.submitted) {
                            if (allocator) {
                                vk_device.RecordAllocatorQuarantine();
                                vk_device.RecordSkippedCommandPoolReset();
                                allocator_quarantine.emplace_back(std::move(allocator));
                            }
                        }
                    }

                    for (auto& allocator : _batch.abandoned) {
                        if (!vk_device.IsFaulted() && allocator->ResetAbandoned()) {
                            allocators.Push(allocator.release());
                        } else if (allocator) {
                            vk_device.RecordAllocatorQuarantine();
                            vk_device.RecordSkippedCommandPoolReset();
                            allocator_quarantine.emplace_back(std::move(allocator));
                        }
                    }
                },
                [this](VulkanDescriptorPushLease& _lease) {
                    vk_device.GetGlobalDescriptorHeap().RecyclePushDescriptors(std::move(_lease));
                },
                [this, &batch_gpu_success](UniquePtr<VulkanPresentor>& _presentor) {
                    if (batch_gpu_success && !vk_device.IsFaulted()) {
                        if (_presentor->CompleteSuccess() && _presentor->Reset()) {
                            presentors.Push(_presentor.release());
                            return;
                        }
                    }
                    vk_device.RecordAllocatorQuarantine();
                    vk_device.RecordSkippedCommandPoolReset();
                    presentor_quarantine.emplace_back(std::move(_presentor));
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
                    auto* fence = reinterpret_cast<VulkanFence*>(_evt.timeline_handle);
                    if (batch_gpu_success) {
                        fence->Notify(_evt.value);
                    } else if (vk_device.IsFaulted()) {
                        fence->Fail(vk_device.GetFirstFaultResult());
                    }
                },
                [](WaitEvent&) {},
                [](VulkanFence*) {
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
        NotifyQueueLocalSyncWait(
            queue.GetType(),
            target_retirement_serial,
            completion_groups.size()
        );
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

#pragma region[ copy queue ]

VkCopyQueue::VkCopyQueue(VulkanDevice& _device) :
    CopyQueue(),
    device(_device),
    queue(EQueueType::Copy, _device) {
    timeline = MoerNew(VulkanFence)(_device);
    enabled  = true;
    try {
        thread = std::jthread([this]() {
            ExecuteThread();
        });
    } catch (...) {
        enabled.store(false, std::memory_order_release);
        queue_cv.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
        timeline = nullptr;
        throw;
    }
}

VkCopyQueue::~VkCopyQueue() {
    CancelRuntimeDependencyWaits();
    CompleteAll(last_frame.load(std::memory_order_acquire));
    enabled.store(false, std::memory_order_release);
    queue_cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
    //clear allocators
    Array<VulkanAllocator*> allocs;
    allocators.PopAll(allocs);
    for (auto& allocator : allocs) {
        MoerDelete(allocator);
    }
    allocator_quarantine.clear();
    timeline = nullptr;
    device.RecordQueueSyncComplete();
}

void VkCopyQueue::EnableRuntimeDependencyWaits() noexcept {
    dependency_waits_enabled.store(true, std::memory_order_release);
}

void VkCopyQueue::CancelRuntimeDependencyWaits() noexcept {
    dependency_waits_enabled.store(false, std::memory_order_release);
}

bool VkCopyQueue::ClaimRuntimeOwnership() {
    EExecutionOwnershipMode expected = EExecutionOwnershipMode::Unclaimed;
    if (execution_ownership_mode.compare_exchange_strong(
            expected,
            EExecutionOwnershipMode::Runtime,
            std::memory_order_acq_rel,
            std::memory_order_acquire
        )) {
        EnableRuntimeDependencyWaits();
        return true;
    }
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] Copy Runtime ownership claim rejected mode={}",
            static_cast<uint32>(expected)
        );
    } catch (...) {
    }
    return false;
}

void VkCopyQueue::ReleaseRuntimeOwnership() noexcept {
    CancelRuntimeDependencyWaits();
    EExecutionOwnershipMode expected = EExecutionOwnershipMode::Runtime;
    const bool released = execution_ownership_mode.compare_exchange_strong(
        expected,
        EExecutionOwnershipMode::Unclaimed,
        std::memory_order_acq_rel,
        std::memory_order_acquire
    );
    assert(released && "only the owning Vulkan runtime may release Copy");
}
static constexpr uint64 fread_segment_size = 1024 * 64; // 64KB
IOWaitEvt               VkCopyQueue::Execute(IOQueueSubmission&& _submission) {
    IOQueueCommandList&& cmd_list = std::move(_submission.cmds);

    //prepare sizes
    uint64 temp_size = 0;
    for (auto& cmd : cmd_list.file_to_buffer) {
        const auto& src = std::get<FileDesc>(cmd.src);
        temp_size += cmd.SizeByte();
    }

    for (auto& cmd : cmd_list.file_to_texture) {
        const auto& src = std::get<FileDesc>(cmd.src);
        temp_size += cmd.SizeByte();
    }

    Array<ubyte> temp_buffer(temp_size);
    temp_size = 0;

    auto copy_file_to_mem = [&](const FileDesc& _src, size_t _file_offset, std::span<ubyte> _dst) {
        FILE* result_handle = nullptr;
        fopen_s(&result_handle, (const char*)_src.handle.file, "r");
        if (!result_handle) {
            SPDLOG_ERROR("Failed to open file {}", (const char*)_src.handle.file);
            assert(false && "Failed to open file");
        }
        std::fseek(result_handle, _file_offset, SEEK_SET);
        std::fread(_dst.data(), sizeof(ubyte), _dst.size_bytes(), result_handle);
        std::fclose(result_handle);
    };

    //direct copy to mem
    for (auto& cmd : cmd_list.file_to_mem) {
        const auto& src = std::get<FileDesc>(cmd.src);
        auto        dst = std::get<RawDataDesc>(cmd.dst);
        copy_file_to_mem(src, cmd.file_offset, dst.data);
    }

    //copy file to buffer
    for (auto& cmd : cmd_list.file_to_buffer) {
        const auto& src = std::get<FileDesc>(cmd.src);
        copy_file_to_mem(
            src, cmd.file_offset, std::span<ubyte>(temp_buffer.data() + temp_size, cmd.SizeByte())
        );
        temp_size += cmd.SizeByte();
    }

    //copy file to texture
    for (auto& cmd : cmd_list.file_to_texture) {
        const auto& src = std::get<FileDesc>(cmd.src);
        copy_file_to_mem(
            src, cmd.file_offset, std::span<ubyte>(temp_buffer.data() + temp_size, cmd.SizeByte())
        );
        temp_size += cmd.SizeByte();
    }

    return {};
}

//TODO:看看barrier
IOWaitEvt VkCopyQueue::Execute(CmdSubmit&& _evt) {
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] direct Copy Execute is unsupported; "
            "publish work through RHIExecutor"
        );
    } catch (...) {
    }
    RejectForRuntime(std::move(_evt), VK_ERROR_UNKNOWN, true);
    return {0, 0};
}

std::optional<VkCopyQueue::CurrentVulkanCopyRecordedSubmit>
VkCopyQueue::TranslateForRuntime(CmdSubmit&& _evt) noexcept {
    assert(
        execution_ownership_mode.load(std::memory_order_acquire) ==
            EExecutionOwnershipMode::Runtime &&
        "runtime Copy translate requires an exclusively claimed queue"
    );
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Translate &&
        "runtime Copy translation must run on the Translate owner"
    );

    auto execution_lease = runtime_execution_gate.Acquire();
    std::optional<CurrentVulkanCopyRecordedSubmit> recorded{};
    try {
        recorded.emplace(std::move(_evt));
        recorded->execution_lease = std::move(execution_lease);
        recorded->presentation_source_program =
            BuildPresentationSourceStateProgram(
                recorded->submit, EQueueType::Copy
            );

        const bool has_queue_work =
            !recorded->submit.cmds.empty() ||
            !recorded->submit.wait_events.empty() ||
            !recorded->submit.signal_events.empty() ||
            !recorded->submit.callbacks.empty() ||
            !recorded->submit.success_callbacks.empty() ||
            !recorded->submit.query_tokens.empty() ||
            recorded->submit.b_sync ||
            recorded->submit.b_tick_profiling ||
            recorded->submit.b_delete_resources;
        if (!has_queue_work) {
            recorded->retirement_outcome     = {};
            recorded->native_submit_resolved = true;
            recorded->completion_committed   = true;
            return recorded;
        }

        std::unique_lock<std::mutex> execution_lock(exec_mutex);
        recorded->logical_timeline =
            last_frame.fetch_add(1, std::memory_order_relaxed) + 1;
        recorded->context = VulkanOperationContext{
            .operation   = EVulkanFaultOperation::QueueSubmit,
            .queue_type  = EQueueType::Copy,
            .queue       = queue.GetHandle(),
            .timeline    = recorded->logical_timeline,
            .work_serial = recorded->logical_timeline,
        };

        if (device.IsFaulted()) {
            device.RecordRejectedSubmit();
            recorded->retirement_outcome = {
                EVulkanOperationStatus::Rejected,
                device.GetFirstFaultResult()
            };
            recorded->native_submit_resolved = true;
            return recorded;
        }

        FunctionTable function_table{
            .is_resource_write       = &IsBufferTextureWrite,
            .is_resource_read        = &IsBufferTextureRead,
            .is_texture_sampled      = &IsTextureSampled,
            .is_resource_in_bindless = &IsResourceInBindlessArray,
            .lock_bdls_array         = &LockBindlessArray,
            .unlock_bdls_array       = &UnlockBindlessArray
        };
        CmdReorderer reorderer{
            function_table, recorded->submit.cached_args
        };

        recorded->allocators.submitted.reserve(1);
        recorded->allocators.submitted.emplace_back(GetAllocator());
        auto& vk_allocator =
            *recorded->allocators.submitted.front();
        auto& vk_cmd_list = vk_allocator.GetCmdList();
        auto& vk_tracker  = vk_allocator.GetTracker();

        VkCmdPreprocessor preprocessor(
            device,
            vk_tracker,
            vk_allocator,
            {},
            recorded->submit.cached_args,
            EQueueType::Copy
        );
        VkCmdVisitor visitor(
            device,
            vk_allocator,
            vk_tracker,
            vk_cmd_list,
            recorded->submit.cached_args
        );

        for (const auto& cmd : recorded->submit.cmds) {
            reorderer.AcceptCmd(cmd.get());
        }

        VK_CHECK_RESULT(vk_cmd_list.Begin());
        vk_cmd_list.BeginLabel("Copy", {0.0f, 1.0f, 1.0f, 1.0f});
        for (const CmdReorderer::LinkedCommandList& cmd_list :
             reorderer.m_cmd_lists) {
            if (cmd_list.head == nullptr) {
                continue;
            }
            for (const auto* node = cmd_list.head; node != nullptr;
                 node = node->next) {
                preprocessor.VisitCmd(node->cmd);
            }
            vk_tracker.ResolveBarriers();
            vk_tracker.DispatchBarriers(vk_cmd_list);
            for (const auto* node = cmd_list.head; node != nullptr;
                 node = node->next) {
                visitor.VisitCmd(node->cmd);
            }
        }

        if (!recorded->submit.HasExplicitResourceStateOwnership()) {
            vk_tracker.RestoreState();
        }
        vk_tracker.DispatchBarriers(vk_cmd_list);
        vk_cmd_list.EndLabel();
        VK_CHECK_RESULT(vk_cmd_list.End());
        vk_tracker.Reset();
        return recorded;
    } catch (const std::exception& error) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] Copy translation threw: {}",
                error.what()
            );
        } catch (...) {
        }
    } catch (...) {
        try {
            LOG_ERROR("[RHIExecutor][Vulkan] Copy translation threw");
        } catch (...) {
        }
    }

    if (!recorded) {
        recorded.emplace(std::move(_evt));
        recorded->execution_lease = std::move(execution_lease);
    }
    if (!recorded->native_submit_resolved) {
        device.RecordRejectedSubmit();
        recorded->retirement_outcome = {
            EVulkanOperationStatus::Faulted, VK_ERROR_UNKNOWN
        };
        recorded->native_submit_resolved = true;
    }
    return recorded;
}

VulkanRuntimeSubmissionResult VkCopyQueue::SubmitRecordedForRuntime(
    CurrentVulkanCopyRecordedSubmit       _recorded,
    const VulkanRuntimePreCompletionHook* _pre_completion
) noexcept {
    assert(
        execution_ownership_mode.load(std::memory_order_acquire) ==
            EExecutionOwnershipMode::Runtime &&
        "runtime Copy submit requires an exclusively claimed queue"
    );
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "runtime Copy submit must run on the Submission owner"
    );
    assert(
        _recorded.execution_lease.OwnsGate() &&
        "runtime Copy packet must retain queue execution ownership"
    );

    if (_recorded.logical_timeline == 0 &&
        _recorded.completion_committed) {
        return {
            .outcome = _recorded.retirement_outcome,
        };
    }

    if (!_recorded.native_submit_resolved) {
        try {
            std::unique_lock<std::mutex> execution_lock(exec_mutex);
            if (!WaitForSubmittedDependencies(
                    device,
                    _recorded.submit.wait_events,
                    EQueueType::Copy,
                    &dependency_waits_enabled
                )) {
                device.RecordRejectedSubmit();
                _recorded.retirement_outcome = {
                    EVulkanOperationStatus::Rejected,
                    GetRejectedSubmitResult(device)
                };
                _recorded.recoverable_rejection =
                    !device.IsFaulted();
                _recorded.native_submit_resolved = true;
            } else {
                std::optional<
                    VulkanPresentationSourceStateTransaction>
                    presentation_transaction{};
                const auto reject_presentation_transaction =
                    [&](const VulkanOperationResult& _outcome,
                        std::string_view             _reason) {
                        try {
                            LOG_ERROR(
                                "[RHIExecutor][Vulkan] Copy rejected "
                                "before native submit: timeline={} "
                                "reason={} VkResult={}",
                                _recorded.logical_timeline,
                                _reason,
                                static_cast<int32>(_outcome.result)
                            );
                        } catch (...) {
                        }
                        device.RecordRejectedSubmit();
                        _recorded.retirement_outcome = _outcome;
                        _recorded.recoverable_rejection =
                            _outcome.status ==
                                EVulkanOperationStatus::Rejected &&
                            !device.IsFaulted() &&
                            _outcome.result !=
                                VK_ERROR_DEVICE_LOST;
                        _recorded.native_submit_resolved = true;
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
                        "presentation-state transaction evaluation "
                        "failed"
                    );
                }

                if (!_recorded.native_submit_resolved) {
                    queue.Signal(
                        timeline,
                        _recorded.logical_timeline,
                        VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                    );
                    for (auto& event :
                         _recorded.submit.wait_events) {
                        queue.Wait(
                            reinterpret_cast<VulkanFence*>(
                                event.timeline_handle
                            ),
                            event.value
                        );
                    }
                    for (auto& event :
                         _recorded.submit.signal_events) {
                        queue.Signal(
                            reinterpret_cast<VulkanFence*>(
                                event.timeline_handle
                            ),
                            event.value,
                            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                        );
                    }

                    assert(
                        _recorded.allocators.submitted.size() ==
                                1 &&
                        "Copy Translate must publish exactly one "
                        "command allocator"
                    );
                    auto& vk_allocator =
                        *_recorded.allocators.submitted.front();
                    _recorded.retirement_outcome =
                        queue.Submit(
                            vk_allocator.GetCmdList(),
                            _recorded.context
                        );
                    _recorded.native_submit_resolved = true;
                    if (_recorded.retirement_outcome.WasSubmitted()) {
                        CommitAcceptedPresentationSourceStateTransaction(
                            *presentation_transaction
                        );
                        MarkSubmissionAccepted(
                            timeline.Get(),
                            _recorded.logical_timeline,
                            _recorded.submit.signal_events
                        );
                    }
                }
            }
        } catch (const std::exception& error) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] Copy recorded submit threw before retirement: {}",
                    error.what()
                );
            } catch (...) {
            }
        } catch (...) {
            try {
                LOG_ERROR(
                    "[RHIExecutor][Vulkan] Copy recorded submit threw before retirement"
                );
            } catch (...) {
            }
        }
    }

    if (!_recorded.native_submit_resolved) {
        queue.DiscardPendingSubmitState();
        device.RecordRejectedSubmit();
        _recorded.retirement_outcome =
            MakeUnsubmittedOutcome(VK_ERROR_UNKNOWN, false);
        _recorded.recoverable_rejection = false;
        _recorded.native_submit_resolved = true;
    }
    VulkanRuntimeSubmissionResult runtime_result{
        .outcome               = _recorded.retirement_outcome,
        .recoverable_rejection = _recorded.recoverable_rejection,
    };
    if (_recorded.retirement_outcome.WasSubmitted()) {
        runtime_result.completion =
            WaitEvent{
                uint64(timeline.Get()),
                _recorded.logical_timeline
            };
    }
    if (_pre_completion != nullptr &&
        _pre_completion->callback != nullptr) {
        _pre_completion->callback(
            _pre_completion->context, runtime_result
        );
    }
    if (!_recorded.completion_committed) {
        CommitRuntimeSubmitCompletion(
            _recorded, _recorded.retirement_outcome
        );
    }
    return runtime_result;
}

void VkCopyQueue::RejectRecordedForRuntime(
    CurrentVulkanCopyRecordedSubmit&& _recorded,
    VkResult                           _result,
    bool                               _recoverable
) noexcept {
    assert(
        _recorded.execution_lease.OwnsGate() &&
        "a failed Copy Translate -> Submission handoff must retain ownership"
    );
    if (!_recorded.native_submit_resolved) {
        queue.DiscardPendingSubmitState();
        device.RecordRejectedSubmit();
        _recorded.retirement_outcome =
            MakeUnsubmittedOutcome(_result, _recoverable);
        _recorded.recoverable_rejection = _recoverable;
        _recorded.native_submit_resolved = true;
    }
    if (!_recorded.completion_committed) {
        CommitRuntimeSubmitCompletion(
            _recorded, _recorded.retirement_outcome
        );
    }
}
void VkCopyQueue::RejectForRuntime(
    CmdSubmit&& _submit,
    VkResult    _result,
    bool        _recoverable,
    VulkanBatchCompletionTicket _batch_completion
) noexcept {
    device.RecordRejectedSubmit();
    CurrentVulkanCopyRecordedSubmit rejected{std::move(_submit)};
    rejected.batch_completion = std::move(_batch_completion);
    rejected.context = VulkanOperationContext{
        .operation  = EVulkanFaultOperation::QueueSubmit,
        .queue_type = EQueueType::Copy,
        .queue      = queue.GetHandle(),
    };
    rejected.retirement_outcome =
        MakeUnsubmittedOutcome(_result, _recoverable);
    rejected.recoverable_rejection = _recoverable;
    rejected.native_submit_resolved = true;
    CommitRuntimeSubmitCompletion(
        rejected, rejected.retirement_outcome
    );
}

void VkCopyQueue::CommitRuntimeSubmitCompletion(
    CurrentVulkanCopyRecordedSubmit& _recorded,
    const VulkanOperationResult&      _outcome
) noexcept {
    assert(!_recorded.completion_committed);
    try {
        if (!_outcome.WasSubmitted()) {
            if (_recorded.logical_timeline != 0) {
                TerminalizeUnsubmittedSignal(
                    device,
                    timeline.Get(),
                    _recorded.logical_timeline,
                    _outcome
                );
            }
            TerminalizeUnsubmittedSignals(
                device, _recorded.submit.signal_events, _outcome
            );
            // Exact signal rejection is published by Submission. Do not
            // retain raw external fence pointers in the Completion packet.
            _recorded.submit.signal_events.clear();
            PublishUnsubmittedQueryErrors(
                _recorded.submit,
                &_recorded.allocators,
                "Vulkan Copy submission was rejected before GPU completion"
            );
            _recorded.allocators.abandoned.reserve(
                _recorded.allocators.abandoned.size() +
                _recorded.allocators.submitted.size()
            );
            for (auto& allocator : _recorded.allocators.submitted) {
                _recorded.allocators.abandoned.emplace_back(
                    std::move(allocator)
                );
            }
            _recorded.allocators.submitted.clear();
        }
        std::unique_lock<std::mutex> lock(event_mutex);
        const uint64 retirement_serial = retirement_enqueued_serial + 1;
        const std::shared_ptr<VulkanBatchCompletionGroup>
            completion_group = _recorded.batch_completion.Valid() ?
                _recorded.batch_completion.group :
                nullptr;
        event_queue.emplace_back(
            std::in_place_type<VulkanSubmitCompletionBatch>,
            _recorded.logical_timeline,
            true,
            retirement_serial,
            _outcome,
            _recorded.context,
            _outcome.WasSubmitted(),
            _recorded.logical_timeline != 0,
            std::move(_recorded.submit),
            std::move(_recorded.allocators),
            std::optional<VulkanDescriptorPushLease>{},
            Array<RHIResource*>{},
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
        retirement_enqueued_serial = retirement_serial;
        _recorded.completion_committed = true;
    } catch (...) {
        std::terminate();
    }
    queue_cv.notify_one();
}

void VkCopyQueue::Sync(uint64 _timeline) {
    if (GetCurrentRHIThreadRole() == ERHIThreadRole::Completion) {
        try {
            LOG_ERROR(
                "[RHIExecutor][Vulkan] Copy Completion owner cannot block on "
                "a retirement queue"
            );
        } catch (...) {
        }
        return;
    }
    CompleteAll(_timeline);
}

FenceRef VkCopyQueue::GetFenceHandle() {
    return FenceRef(timeline);
}

void VkCopyQueue::ExecuteThread() {
    Platform::SetCurrentThreadName("Vulkan Copy Completion");
    RHIThreadRoleScope owner_scope(ERHIThreadRole::Completion);
    bool batch_gpu_success = false;
    while (true) {
        std::optional<IOEvent> evt;
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            queue_cv.wait(lock, [this]() {
                return !enabled.load(std::memory_order_acquire) || !event_queue.empty();
            });
            if (!enabled.load(std::memory_order_acquire) && event_queue.empty()) {
                break;
            }
            evt.emplace(std::move(event_queue.front()));
            event_queue.pop_front();
        }

        const uint64 event_timeline = evt->timeline;
        std::visit(
            Overload{
                [this, event_timeline, &batch_gpu_success](VulkanSubmissionEvent& _submission) {
                    batch_gpu_success = false;
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
                    VulkanOperationContext wait_context = _submission.context;
                    wait_context.operation = EVulkanFaultOperation::TimelineHostWait;
                    const VkResult result = timeline->HostWait(event_timeline, wait_context);
                    if (result == VK_SUCCESS && !device.IsDeviceLost()) {
                        timeline->Notify(event_timeline);
                        batch_gpu_success = true;
                    } else {
                        timeline->Fail(result);
                    }
                },
                [this, event_timeline, &batch_gpu_success](
                    VulkanSubmitCompletionBatch& _batch
                ) {
                    batch_gpu_success = false;
                    VkResult completion_result = _batch.outcome.result;
                    if (completion_result == VK_SUCCESS && !_batch.gpu_submitted) {
                        completion_result = VK_ERROR_UNKNOWN;
                    }
                    const bool recoverable_rejection =
                        !_batch.gpu_submitted &&
                        _batch.outcome.status ==
                            EVulkanOperationStatus::Rejected &&
                        !device.IsFaulted() &&
                        _batch.outcome.result != VK_ERROR_DEVICE_LOST;

                    if (_batch.gpu_submitted) {
                        VulkanOperationContext wait_context = _batch.context;
                        wait_context.operation = EVulkanFaultOperation::TimelineHostWait;
                        const VkResult wait_result =
                            timeline->HostWait(event_timeline, wait_context);
                        if (wait_result == VK_SUCCESS && !device.IsFaulted()) {
                            batch_gpu_success = true;
                            completion_result = VK_SUCCESS;

                            for (auto& allocator : _batch.allocators.submitted) {
                                const VkResult query_result =
                                    allocator->PrepareTimestampQueriesAfterGpuCompletion(
                                        _batch.context
                                    );
                                if (query_result != VK_SUCCESS) {
                                    completion_result = query_result;
                                    batch_gpu_success = false;
                                    break;
                                }
                            }
                            if (batch_gpu_success && device.IsFaulted()) {
                                completion_result =
                                    device.GetFirstFaultResult();
                                batch_gpu_success = false;
                            }

                            if (batch_gpu_success) {
                                timeline->Notify(event_timeline);
                            } else if (_batch.owns_queue_timeline) {
                                timeline->Fail(completion_result);
                            }
                        } else {
                            completion_result =
                                wait_result != VK_SUCCESS ?
                                    wait_result :
                                device.IsFaulted() ?
                                    device.GetFirstFaultResult() :
                                    VK_ERROR_DEVICE_LOST;
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
                            reinterpret_cast<VulkanFence*>(event.timeline_handle);
                        if (fence == nullptr) {
                            continue;
                        }
                        if (batch_gpu_success) {
                            fence->Notify(event.value);
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
                            "Vulkan timestamp query did not resolve after GPU completion" :
                            "Vulkan submission did not reach successful GPU completion";
                    for (auto& allocator : _batch.allocators.submitted) {
                        allocator->PublishTimestampQueriesAfterGpuCompletion(
                            query_batch,
                            batch_gpu_success,
                            query_failure_reason
                        );
                    }
                    for (auto& allocator : _batch.allocators.abandoned) {
                        allocator->PublishTimestampQueriesAfterGpuCompletion(
                            query_batch,
                            false,
                            "Vulkan Copy timestamp query recorder was abandoned before native submission"
                        );
                    }
                    _batch.submit.PublishPendingQueryErrors(
                        query_failure_reason, query_batch
                    );

                    bool recycle_batch =
                        batch_gpu_success && !device.IsFaulted();
                    if (recycle_batch) {
                        for (auto& allocator : _batch.allocators.submitted) {
                            recycle_batch =
                                allocator->CompleteSuccessCallbacks() &&
                                recycle_batch;
                        }
                    }

                    if (_batch.descriptor_lease.has_value()) {
                        device.GetGlobalDescriptorHeap().RecyclePushDescriptors(
                            std::move(*_batch.descriptor_lease)
                        );
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
                        grouped_completion->query_tokens =
                            std::move(_batch.submit.query_tokens);
                        grouped_completion->query_batch = query_batch;
                        grouped_completion->callbacks =
                            std::move(_batch.submit.callbacks);
                        grouped_completion->success_callbacks =
                            std::move(_batch.submit.success_callbacks);
                        grouped_completion->deferred_releases =
                            std::move(_batch.deferred_releases);
                        grouped_completion->device       = &device;
                        grouped_completion->gpu_success  =
                            batch_gpu_success;
                        grouped_completion->release_safe = false;
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
                        _batch.submit.NotifyPendingQueries(query_batch);
                        for (auto& allocator : _batch.allocators.submitted) {
                            allocator->NotifyTimestampQueriesAfterGpuCompletion(
                                query_batch
                            );
                        }
                        for (auto& allocator : _batch.allocators.abandoned) {
                            allocator->NotifyTimestampQueriesAfterGpuCompletion(
                                query_batch
                            );
                        }
                        _batch.submit.query_tokens.clear();
                    }

                    if (recycle_batch) {
                        for (auto& allocator : _batch.allocators.submitted) {
                            recycle_batch = allocator->Reset() && recycle_batch;
                        }
                    }
                    if (recycle_batch && !device.IsFaulted()) {
                        for (auto& allocator : _batch.allocators.submitted) {
                            allocators.Push(allocator.release());
                        }
                    } else {
                        for (auto& allocator : _batch.allocators.submitted) {
                            if (!allocator) {
                                continue;
                            }
                            device.RecordAllocatorQuarantine();
                            device.RecordSkippedCommandPoolReset();
                            allocator_quarantine.emplace_back(std::move(allocator));
                        }
                    }
                    for (auto& allocator : _batch.allocators.abandoned) {
                        if (!device.IsFaulted() && allocator->ResetAbandoned()) {
                            allocators.Push(allocator.release());
                        } else if (allocator) {
                            device.RecordAllocatorQuarantine();
                            device.RecordSkippedCommandPoolReset();
                            allocator_quarantine.emplace_back(std::move(allocator));
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
                            callbacks, "[VulkanCopyQueue] runtime completion"
                        );
                        if (batch_gpu_success) {
                            InvokeCallbacksNoexcept(
                                success_callbacks,
                                "[VulkanCopyQueue] runtime success completion"
                            );
                        }

                        for (auto* resource : _batch.deferred_releases) {
                            if (batch_gpu_success) {
                                MoerDelete(resource);
                            } else {
                                device.EnqueueDeferredRelease(resource);
                            }
                        }
                    }
                    _batch.deferred_releases.clear();
                },
                [this, &batch_gpu_success](UniquePtr<VulkanAllocator>& _allocator) {
                    if (batch_gpu_success && !device.IsFaulted()) {
                        if (_allocator->CompleteSuccess() && _allocator->Reset()) {
                            allocators.Push(_allocator.release());
                            return;
                        }
                    }
                    device.RecordAllocatorQuarantine();
                    device.RecordSkippedCommandPoolReset();
                    allocator_quarantine.emplace_back(std::move(_allocator));
                },
                [this, &batch_gpu_success](VulkanCallbackBatch& _batch) {
                    if (!_batch.success_only || batch_gpu_success) {
                        InvokeCallbacksNoexcept(
                            _batch.callbacks, "[VulkanCopyQueue] completion"
                        );
                    }
                },
                [](VulkanCompletionMarker&) {},
                [this, &batch_gpu_success](IOSignalEvt& _evt) {
                    auto* fence = reinterpret_cast<VulkanFence*>(_evt.handle);
                    if (batch_gpu_success) {
                        fence->Notify(_evt.timeline);
                    } else if (device.IsFaulted()) {
                        fence->Fail(device.GetFirstFaultResult());
                    }
                },
                [](IOWaitEvt&) {},
                [](UniquePtr<VulkanPresentor>&) {
                    assert(false && "Presentor event is invalid on the copy queue");
                }
            },
            evt->event
        );

        if (evt->wake_thread) {
            uint64 settled = cpu_settled_frame.load(std::memory_order_relaxed);
            while (settled < event_timeline &&
                   !cpu_settled_frame.compare_exchange_weak(
                       settled,
                       event_timeline,
                       std::memory_order_release,
                       std::memory_order_relaxed
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
            settled_cv.notify_all();
        }
    }
}

void VkCopyQueue::ExecuteIOThread(IOQueueCommandList&& _cmd_list, uint64_t _timeline) {

    IOQueueCommandList&& cmd_list = std::move(_cmd_list);

    CommandList rhi_cmd_list{};
    //prepare sizes
    uint64 temp_size = 0;
    for (auto& cmd : cmd_list.file_to_buffer) {
        const auto& src = std::get<FileDesc>(cmd.src);
        temp_size += cmd.SizeByte();
    }

    for (auto& cmd : cmd_list.file_to_texture) {
        const auto& src = std::get<FileDesc>(cmd.src);
        temp_size += cmd.SizeByte();
    }

    Array<ubyte> temp_buffer(temp_size);
    temp_size = 0;

    auto copy_file_to_mem = [&](const FileDesc& _src, size_t _file_offset, std::span<ubyte> _dst) {
        FILE* result_handle = nullptr;
        fopen_s(&result_handle, (const char*)_src.handle.file, "r");
        if (!result_handle) {
            SPDLOG_ERROR("Failed to open file {}", (const char*)_src.handle.file);
            assert(false && "Failed to open file");
        }
        std::fseek(result_handle, _file_offset, SEEK_SET);
        std::fread(_dst.data(), sizeof(ubyte), _dst.size_bytes(), result_handle);
        std::fclose(result_handle);
    };

    //direct copy to mem
    for (auto& cmd : cmd_list.file_to_mem) {
        const auto& src = std::get<FileDesc>(cmd.src);
        auto        dst = std::get<RawDataDesc>(cmd.dst);
        copy_file_to_mem(src, cmd.file_offset, dst.data);
    }

    //copy file to buffer
    for (auto& cmd : cmd_list.file_to_buffer) {
        const auto& src = std::get<FileDesc>(cmd.src);
        copy_file_to_mem(
            src, cmd.file_offset, std::span<ubyte>(temp_buffer.data() + temp_size, cmd.SizeByte())
        );
        const auto&   dst    = std::get<BufferViewDesc>(cmd.dst);
        VulkanBuffer* vk_dst = reinterpret_cast<VulkanBuffer*>(dst.handle);
        rhi_cmd_list.CopyFrom(
            std::span<byte>((byte*)temp_buffer.data() + temp_size, cmd.SizeByte()),
            vk_dst->GetView(dst.offset, dst.size)
        );
        temp_size += cmd.SizeByte();
    }

    //copy file to texture
    for (auto& cmd : cmd_list.file_to_texture) {
        const auto& src = std::get<FileDesc>(cmd.src);
        copy_file_to_mem(
            src, cmd.file_offset, std::span<ubyte>(temp_buffer.data() + temp_size, cmd.SizeByte())
        );
        const auto&    dst    = std::get<TextureViewDesc>(cmd.dst);
        VulkanTexture* vk_dst = reinterpret_cast<VulkanTexture*>(dst.handle);
        TextureView    view(vk_dst, dst.pixel_fmt, dst.mip_offset, dst.mip_cnt);
        view.offset = dst.offset;
        view.extent = dst.size;
        rhi_cmd_list.CopyFrom(std::span<byte>((byte*)temp_buffer.data() + temp_size, cmd.SizeByte()), view);
        temp_size += cmd.SizeByte();
    }
    rhi_cmd_list.AddCallback([temp_data(std::move(temp_buffer))]() {});

    std::unique_lock<std::mutex> rhi_lock(rhi_mutex);
    io_rhi_cmdlists.emplace(std::move(rhi_cmd_list), _timeline);
}

void VkCopyQueue::IOThreadLoop() {
    while (enabled) {
        IOQueueCommandList cmdlist;
        uint64             timeline;
        {
            std::unique_lock<std::mutex> io_lock(io_mutex);
            if (io_thread_cmds.empty()) {
                std::this_thread::yield();
                continue;
            }
            auto pair = std::move(io_thread_cmds.front());
            io_thread_cmds.pop();
            cmdlist  = std::move(pair.first);
            timeline = pair.second;
        }

        ExecuteIOThread(std::move(cmdlist), timeline);
    }
}

void VkCopyQueue::RHIThreadLoop() {
    try {
        LOG_ERROR(
            "[RHIExecutor][Vulkan] Legacy Copy RHIThreadLoop is disabled; "
            "native Copy submission belongs to Moer Vulkan Submission"
        );
    } catch (...) {
    }
    assert(false && "legacy Copy RHIThreadLoop bypasses the Submission owner");
    return;

    while (enabled) {
        CommandList cmdlist;
        uint64      timeline;
        {
            std::unique_lock<std::mutex> io_lock(rhi_mutex);
            if (io_rhi_cmdlists.empty()) {
                std::this_thread::yield();
                continue;
            }
            auto pair = std::move(io_rhi_cmdlists.front());

            cmdlist  = std::move(pair.first);
            timeline = pair.second;
        }

        auto  allocator    = std::move(GetAllocator());
        auto& vk_allocator = *allocator;
        auto& vk_cmd_list  = vk_allocator.GetCmdList();
        auto& vk_tracker   = vk_allocator.GetTracker();
        VK_CHECK_RESULT(vk_cmd_list.Begin());
        vk_cmd_list.BeginLabel("IO Copy", {0.0f, 1.0f, 1.0f, 1.0f});

        VkCmdPreprocessor preprocessor(device, vk_tracker, vk_allocator, {}, {}, EQueueType::Copy);
        VkCmdVisitor      visitor(device, vk_allocator, vk_tracker, vk_cmd_list, {});

        auto&& submission = cmdlist.Submit();
        for (const auto& cmd : submission.cmds) {
            preprocessor.VisitCmd(cmd.get());
        }
        vk_tracker.ResolveBarriers();
        vk_tracker.DispatchBarriers(vk_cmd_list);
        for (const auto& cmd : submission.cmds) {
            visitor.VisitCmd(cmd.get());
        }
        if (!submission.HasExplicitResourceStateOwnership()) {
            vk_tracker.RestoreState();
        }
        vk_tracker.DispatchBarriers(vk_cmd_list);
        vk_cmd_list.EndLabel();
        VK_CHECK_RESULT(vk_cmd_list.End());
        vk_tracker.Reset();
        //event queue
        auto current_timeline = ++last_frame;
        const VulkanOperationContext context{
            .operation   = EVulkanFaultOperation::QueueSubmit,
            .queue_type  = EQueueType::Copy,
            .queue       = queue.GetHandle(),
            .timeline    = current_timeline,
            .work_serial = current_timeline,
        };
        queue.Signal(this->timeline, current_timeline, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        queue.Wait(this->timeline, current_timeline - 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        const VulkanOperationResult submit_outcome =
            queue.Submit(vk_allocator.GetCmdList(), context);
        if (submit_outcome.WasSubmitted()) {
            this->timeline->MarkSubmitted(current_timeline);
        } else {
            this->timeline->Fail(submit_outcome.result);
        }
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(
                VulkanSubmissionEvent{submit_outcome, context, submit_outcome.WasSubmitted()},
                current_timeline,
                false
            );
            event_queue.emplace_back(std::move(allocator), current_timeline, false);
            if (!submission.callbacks.empty()) {
                event_queue.emplace_back(
                    VulkanCallbackBatch{std::move(submission.callbacks), false},
                    current_timeline,
                    false
                );
            }
            if (!submission.success_callbacks.empty()) {
                event_queue.emplace_back(
                    VulkanCallbackBatch{std::move(submission.success_callbacks), true},
                    current_timeline,
                    false
                );
            }
            const uint64 retirement_serial = retirement_enqueued_serial + 1;
            event_queue.emplace_back(
                VulkanCompletionMarker{},
                current_timeline,
                true,
                retirement_serial
            );
            retirement_enqueued_serial = retirement_serial;
            queue_cv.notify_one();
        }
        {
            std::unique_lock<std::mutex> io_lock(rhi_mutex);
            io_rhi_cmdlists.pop();
        }
    }
}

UniquePtr<VulkanAllocator> VkCopyQueue::GetAllocator() {
    auto allocator = std::move(UniquePtr<VulkanAllocator>(allocators.Pop()));
    if (allocator) {
        // allocator->ResetCmdList();
        return std::move(allocator);
    }
    return MakeUnique<VulkanAllocator>(&device, EQueueType::Copy);
}

void VkCopyQueue::CompleteAll(uint64 _timeline) {
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
        NotifyQueueLocalSyncWait(
            EQueueType::Copy,
            target_retirement_serial,
            completion_groups.size()
        );
        settled_cv.wait(
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
