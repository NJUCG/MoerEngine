#include "VulkanBarrier.h"
#include "VulkanCustomCommand.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "VulkanQueueInternal.h"
#include "VulkanSwapChain.h"
#include "log/LogSystem.h"
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
    const uint     source_index                = static_cast<uint>(_source);
    const uint     destination_index           = static_cast<uint>(_destination);
    constexpr uint first_single_plane_color    = static_cast<uint>(PF_R4G4_UNORM_PACK8);
    constexpr uint last_single_plane_color     = static_cast<uint>(PF_E5B9G9R9_UFLOAT_PACK32);
    const auto     is_known_single_plane_color = [](uint _format_index) noexcept {
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
    if ((texture->GetUsage() & ETextureUsageFlags::PRESENTATION_SOURCE) != ETextureUsageFlags::PRESENTATION_SOURCE) {
        return {false, "Vulkan present source must declare PRESENTATION_SOURCE usage"};
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
        LogQueueErrorNoexcept("[RHIExecutor][Vulkan] failed to resolve Present receipt");
    }
}

} // namespace

[[nodiscard]] static bool IsPresentationSourceTexture(const Texture* _texture) noexcept {
    return _texture != nullptr &&
           (_texture->GetUsage() & ETextureUsageFlags::PRESENTATION_SOURCE) == ETextureUsageFlags::PRESENTATION_SOURCE;
}

[[nodiscard]] static bool IsAcceptedPresentationSourcePublication(
    const ExplicitTextureBarrier& _barrier,
    const VulkanTexture&          _texture
) noexcept {
    return _barrier.publish_external_state && _barrier.queue_transfer.phase == EBarrierQueueTransferPhase::None &&
           _barrier.dst_state.stage == ERHIPipelineStageFlags::PS_TRANSFER &&
           _barrier.dst_state.access == ERHIAccessFlags::TRANSFER_READ &&
           _barrier.dst_state.texture_layout == ETextureLayout::TEXTURE_LAYOUT_COMMON &&
           _barrier.texture_aspects == ETextureAspectFlags::COLOR && _barrier.mip_level == 0 &&
           _barrier.mip_count == _texture.GetNumMips() && _barrier.array_layer == 0 &&
           _barrier.array_count == _texture.GetNumArray();
}

template<typename TTextureVisitor, typename TBindlessVisitor>
static void VisitPresentationTextureArgument(
    const TArg&       _argument,
    TTextureVisitor&  _texture_visitor,
    TBindlessVisitor& _bindless_visitor
) {
    std::visit(
        [&](const auto& _resource) {
            using TResource = std::decay_t<decltype(_resource)>;
            if constexpr (std::is_same_v<TResource, TextureView>) {
                _texture_visitor(_resource.GetTexture());
            } else if constexpr (std::is_same_v<TResource, TextureViewArray>) {
                for (const TextureView& view : _resource) {
                    _texture_visitor(view.GetTexture());
                }
            } else if constexpr (std::is_same_v<TResource, BindlessArrayRef>) {
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
        VisitPresentationTextureArgument(argument, _texture_visitor, _bindless_visitor);
    }
}

template<typename TTextureVisitor, typename TBindlessVisitor>
static void VisitPresentationTextureArguments(
    const ArrayArguments& _arguments,
    const TCachedArgArray&,
    TTextureVisitor&  _texture_visitor,
    TBindlessVisitor& _bindless_visitor
) {
    VisitPresentationTextureArguments(_arguments, _texture_visitor, _bindless_visitor);
}

template<typename TTextureVisitor, typename TBindlessVisitor>
static void VisitPresentationTextureArguments(
    const TShaderArgArray& _arguments,
    const TCachedArgArray& _cached_arguments,
    TTextureVisitor&       _texture_visitor,
    TBindlessVisitor&      _bindless_visitor
) {
    if (std::holds_alternative<ArrayArguments>(_arguments)) {
        VisitPresentationTextureArguments(std::get<ArrayArguments>(_arguments), _texture_visitor, _bindless_visitor);
        return;
    }
    if (std::holds_alternative<ArrayArgReference>(_arguments)) {
        const uint index = std::get<ArrayArgReference>(_arguments).handle;
        if (index < _cached_arguments.size()) {
            VisitPresentationTextureArguments(_cached_arguments[index], _texture_visitor, _bindless_visitor);
        }
    }
}

// Freeze source command order before translation can
// change execution topology. Direct texture uses become concrete events;
// bindless uses remain symbolic until Submission can observe the last
// natively accepted descriptor membership.
VulkanPresentationSourceStateProgram BuildPresentationSourceStateProgram(const CmdSubmit& _submit, EQueueType _queue) {
    VulkanPresentationSourceStateProgram program{};
    program.events.reserve(_submit.cmds.size());

    const auto record_texture = [&](Texture* _texture, bool _ready) {
        if (!IsPresentationSourceTexture(_texture)) {
            return;
        }
        program.events.emplace_back(
            VulkanPresentationSourceStateEvent{
                .type    = EVulkanPresentationSourceStateEventType::TextureState,
                .texture = TextureRef(_texture),
                .ready   = _ready,
            }
        );
    };
    const auto record_bindless = [&](const BindlessArrayRef& _array) {
        if (!_array) {
            return;
        }
        program.events.emplace_back(
            VulkanPresentationSourceStateEvent{
                .type           = EVulkanPresentationSourceStateEventType::BindlessAccess,
                .bindless_array = _array,
            }
        );
    };
    const auto record_view = [&](const TextureView& _view) {
        record_texture(_view.GetTexture(), false);
    };
    const auto record_shader_arguments = [&](const auto& _arguments, UnorderedSet<BindlessArray*>& _seen_bindless) {
        auto record_unique_bindless = [&](const BindlessArrayRef& _array) {
            if (_array && _seen_bindless.emplace(_array.Get()).second) {
                record_bindless(_array);
            }
        };
        auto record_direct_texture = [&](Texture* _texture) {
            record_texture(_texture, false);
        };
        VisitPresentationTextureArguments(
            _arguments, _submit.cached_args, record_direct_texture, record_unique_bindless
        );
    };
    const auto record_render_pass = [&](const RenderPassInfo& _render_pass) {
        if (_render_pass.depth_attachment.Valid()) {
            record_texture(_render_pass.depth_attachment.target, false);
        }
        for (const ColorAttachment& attachment : _render_pass.color_attachments) {
            record_texture(attachment.target, false);
        }
    };
    const auto record_barrier = [&](const BarrierCmd& _command, uint64 _handle) {
        auto* texture = reinterpret_cast<VulkanTexture*>(_handle);
        if (!IsPresentationSourceTexture(texture)) {
            return;
        }

        size_t                        reference_count      = 0;
        const ExplicitTextureBarrier* explicit_publication = nullptr;
        for (const TextureBarrier& barrier : _command.ReadTextures()) {
            if (barrier.handle == _handle) {
                ++reference_count;
            }
        }
        for (const TextureBarrier& barrier : _command.WriteTextures()) {
            if (barrier.handle == _handle) {
                ++reference_count;
            }
        }
        for (const ExplicitTextureBarrier& barrier : _command.ExplicitTextures()) {
            if (barrier.handle == _handle) {
                ++reference_count;
                explicit_publication = &barrier;
            }
        }

        bool ready = false;
        if (_queue == EQueueType::Graphics && reference_count == 1) {
            ready = explicit_publication != nullptr &&
                    IsAcceptedPresentationSourcePublication(*explicit_publication, *texture);
        }
        record_texture(texture, ready);
    };

    for (const UniquePtr<Command>& command_owner : _submit.cmds) {
        const Command* command = command_owner.get();
        if (command == nullptr) {
            continue;
        }
        switch (command->Type()) {
            case Command::EType::CopyBackTexture: {
                const auto& copy = *static_cast<const CopyBackTextureCmd*>(command);
                record_texture(reinterpret_cast<Texture*>(copy.Handle()), false);
                break;
            }
            case Command::EType::BufferToTexture: {
                const auto& copy = *static_cast<const CopyBufferToTextureCmd*>(command);
                record_texture(reinterpret_cast<Texture*>(copy.DstHandle()), false);
                break;
            }
            case Command::EType::TextureToBuffer: {
                const auto& copy = *static_cast<const CopyTextureToBufferCmd*>(command);
                record_texture(reinterpret_cast<Texture*>(copy.SrcHandle()), false);
                break;
            }
            case Command::EType::UploadTexture: {
                const auto& upload = *static_cast<const UploadTextureCmd*>(command);
                record_texture(reinterpret_cast<Texture*>(upload.Handle()), false);
                break;
            }
            case Command::EType::TextureToTexture: {
                const auto& copy = *static_cast<const CopyTextureCmd*>(command);
                record_texture(reinterpret_cast<Texture*>(copy.SrcHandle()), false);
                record_texture(reinterpret_cast<Texture*>(copy.DstHandle()), false);
                break;
            }
            case Command::EType::ShaderDispatch: {
                const auto&                  dispatch = *static_cast<const DispatchCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                record_shader_arguments(dispatch.Args(_submit.cached_args), seen_bindless);
                break;
            }
            case Command::EType::TraceRay: {
                const auto&                  trace = *static_cast<const TraceRayCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                record_shader_arguments(trace.Args(), seen_bindless);
                break;
            }
            case Command::EType::Barrier: {
                const auto& barrier_command = *static_cast<const BarrierCmd*>(command);
                for (const TextureBarrier& barrier : barrier_command.ReadTextures()) {
                    record_barrier(barrier_command, barrier.handle);
                }
                for (const TextureBarrier& barrier : barrier_command.WriteTextures()) {
                    record_barrier(barrier_command, barrier.handle);
                }
                for (const ExplicitTextureBarrier& barrier : barrier_command.ExplicitTextures()) {
                    record_barrier(barrier_command, barrier.handle);
                }
                break;
            }
            case Command::EType::QueueTransfer: {
                const auto& transfer = *static_cast<const QueueTransferCmd*>(command);
                if (transfer.IsImport()) {
                    for (const auto& [view, state] : transfer.ImportTextures()) {
                        (void)state;
                        record_view(view);
                    }
                } else {
                    for (const auto& [view, state] : transfer.ExportTextures()) {
                        (void)state;
                        record_view(view);
                    }
                }
                break;
            }
            case Command::EType::SetDrawState: {
                const auto&                  draw = *static_cast<const SetDrawStateCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                record_shader_arguments(draw.Args(), seen_bindless);
                record_render_pass(draw.RenderPassInfo());
                break;
            }
            case Command::EType::MultiDraw: {
                const auto&                  draw = *static_cast<const MultiDrawCmd*>(command);
                UnorderedSet<BindlessArray*> seen_bindless{};
                for (const DrawBatchElement& element : draw.draw_batch.draw_cmds) {
                    record_shader_arguments(element.args, seen_bindless);
                }
                record_render_pass(draw.RenderPassInfo());
                break;
            }
            case Command::EType::ClearResource: {
                const auto& clear = *static_cast<const ClearResourceCmd*>(command);
                if (clear.IsTexture()) {
                    record_view(clear.Texture());
                }
                break;
            }
            case Command::EType::Custom: {
                const auto& custom = *static_cast<const CustomCmd*>(command);
                if (custom.CustomId() == CustomCmd::CustomCmdId::CUSTOM_DISPATCH) {
                    UnorderedSet<BindlessArray*> seen_bindless{};
                    auto                         record_unique_bindless = [&](const BindlessArrayRef& _array) {
                        if (_array && seen_bindless.emplace(_array.Get()).second) {
                            record_bindless(_array);
                        }
                    };
                    auto record_custom_texture = [&](Texture* _texture) {
                        record_texture(_texture, false);
                    };
                    static_cast<const CustomDispatchCmd&>(custom).IterateArgs([&](const TArg& argument,
                                                                                  ERHIAccessFlags) {
                        VisitPresentationTextureArgument(argument, record_custom_texture, record_unique_bindless);
                    });
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
                const auto&      update_command = *static_cast<const UpdateBindlessArrayCmd*>(command);
                BindlessArrayRef bindless_array(update_command.Handle());
                if (!bindless_array) {
                    throw std::logic_error("bindless update has no array");
                }
                for (const BindlessArray::UpdateCmd& update : update_command.UpdateCommands()) {
                    std::visit(
                        [&](const auto& _update) {
                            using TUpdate = std::decay_t<decltype(_update)>;
                            if constexpr (std::is_same_v<TUpdate, BindlessArray::TextureUpdateInfo>) {
                                if (!IsPresentationSourceTexture(_update.texture.Get())) {
                                    return;
                                }
                                program.events.emplace_back(
                                    VulkanPresentationSourceStateEvent{
                                        .type             = EVulkanPresentationSourceStateEventType::BindlessTextureMembership,
                                        .texture          = _update.texture,
                                        .bindless_array   = bindless_array,
                                        .bindless_slot    = _update.array_idx,
                                        .membership_delta = _update.free ? -1 : 1,
                                    }
                                );
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
                throw std::logic_error("unsupported command in presentation-state collection");
        }
    }
    return program;
}

VulkanPresentationSourceStateTransaction
EvaluatePresentationSourceStateProgram(const VulkanPresentationSourceStateProgram& _program) {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "presentation state evaluation requires the Submission owner"
    );
    using Membership = VulkanBindlessArray::PresentationSourceMembership;
    struct WorkingMembership {
        BindlessArrayRef            bindless_array{};
        std::shared_ptr<Membership> membership{};
    };

    VulkanPresentationSourceStateTransaction transaction{};
    transaction.delta.reserve(_program.events.size());

    Array<WorkingMembership>                   working_memberships{};
    UnorderedMap<VulkanBindlessArray*, size_t> working_membership_indices{};

    const auto& get_working_membership = [&](const BindlessArrayRef& _array) -> WorkingMembership& {
        if (!_array) {
            throw std::logic_error(
                "presentation-state program references a null "
                "bindless array"
            );
        }
        auto*      bindless = static_cast<VulkanBindlessArray*>(_array.Get());
        const auto existing = working_membership_indices.find(bindless);
        if (existing != working_membership_indices.end()) {
            return working_memberships[existing->second];
        }

        const auto accepted = bindless->SnapshotAcceptedPresentationSourceMembership();
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
                .membership     = std::make_shared<Membership>(*accepted),
            }
        );
        working_membership_indices.emplace(bindless, index);
        return working_memberships.back();
    };

    const auto record_texture = [&](const TextureRef& _texture, bool _ready) {
        if (!_texture || !IsPresentationSourceTexture(_texture.Get())) {
            return;
        }
        auto existing = std::find_if(
            transaction.delta.begin(), transaction.delta.end(), [&](const VulkanPresentationSourceStateDelta& _entry) {
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

    for (const VulkanPresentationSourceStateEvent& event : _program.events) {
        switch (event.type) {
            case EVulkanPresentationSourceStateEventType::TextureState:
                record_texture(event.texture, event.ready);
                break;
            case EVulkanPresentationSourceStateEventType::BindlessAccess: {
                const WorkingMembership& working = get_working_membership(event.bindless_array);
                for (const auto& [texture, record] : working.membership->textures) {
                    (void)texture;
                    if (record.count != 0) {
                        record_texture(record.texture, false);
                    }
                }
                break;
            }
            case EVulkanPresentationSourceStateEventType::BindlessTextureMembership: {
                if (!event.texture || !IsPresentationSourceTexture(event.texture.Get()) || event.bindless_slot == 0 ||
                    (event.membership_delta != 1 && event.membership_delta != -1)) {
                    throw std::logic_error(
                        "invalid bindless presentation membership "
                        "mutation"
                    );
                }
                WorkingMembership& working           = get_working_membership(event.bindless_array);
                auto&              membership        = *working.membership;
                const auto         decrement_texture = [&](const TextureRef& _texture) {
                    const auto texture = membership.textures.find(_texture.Get());
                    if (texture == membership.textures.end() || texture->second.count == 0) {
                        throw std::logic_error(
                            "bindless presentation membership "
                                    "slot/count mismatch"
                        );
                    }
                    if (--texture->second.count == 0) {
                        membership.textures.erase(texture);
                    }
                };
                const auto increment_texture = [&](const TextureRef& _texture) {
                    auto [texture, inserted] = membership.textures.try_emplace(
                        _texture.Get(),
                        VulkanBindlessArray::PresentationSourceMembershipRecord{
                            .texture = _texture,
                            .count   = 0,
                        }
                    );
                    (void)inserted;
                    if (texture->second.count == std::numeric_limits<uint32>::max()) {
                        throw std::logic_error(
                            "bindless presentation membership "
                            "count overflow"
                        );
                    }
                    ++texture->second.count;
                };
                auto slot = membership.slots.find(event.bindless_slot);
                if (event.membership_delta > 0) {
                    if (slot != membership.slots.end()) {
                        if (slot->second.Get() == event.texture.Get()) {
                            break;
                        }
                        decrement_texture(slot->second);
                        slot->second = event.texture;
                    } else {
                        membership.slots.emplace(event.bindless_slot, event.texture);
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

    transaction.membership_commits.reserve(working_memberships.size());
    for (WorkingMembership& working : working_memberships) {
        transaction.membership_commits.emplace_back(
            VulkanPresentationSourceMembershipCommit{
                .bindless_array = std::move(working.bindless_array),
                .membership     = std::shared_ptr<const Membership>(std::move(working.membership)),
            }
        );
    }
    return transaction;
}

static void
CommitAcceptedPresentationSourceStateDelta(const Array<VulkanPresentationSourceStateDelta>& _delta) noexcept {
    for (const VulkanPresentationSourceStateDelta& entry : _delta) {
        if (!entry.texture) {
            continue;
        }
        static_cast<VulkanTexture*>(entry.texture.Get())->PublishPresentationSourceReady(entry.ready);
    }
}

void CommitAcceptedPresentationSourceStateTransaction(VulkanPresentationSourceStateTransaction& _transaction) noexcept {
    assert(
        GetCurrentRHIThreadRole() == ERHIThreadRole::Submission &&
        "presentation state publication requires the Submission owner"
    );
    for (VulkanPresentationSourceMembershipCommit& commit : _transaction.membership_commits) {
        if (!commit.bindless_array || !commit.membership) {
            continue;
        }
        static_cast<VulkanBindlessArray*>(commit.bindless_array.Get())
            ->PublishAcceptedPresentationSourceMembership(std::move(commit.membership));
    }
    CommitAcceptedPresentationSourceStateDelta(_transaction.delta);
}

VulkanRuntimeSubmissionResult
VkCommandQueue::PresentForRuntime(SwapchainRef _swapchain, TextureView _source, PresentReceiptRef _receipt) noexcept {
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
        auto* vk_source_texture = static_cast<VulkanTexture*>(_source.texture);
        if (
            !vk_source_texture->IsPresentationSourceReady()
#if MOER_VULKAN_TEST_SUPPORT
            && (scripted_override == nullptr || scripted_override->require_present_source_ready)
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
                "accepted producer export published GENERAL / TRANSFER_READ"
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
            PendingSubmission recorded{CmdSubmit{{}, {}, {}, {}}, context, current_timeline};
            recorded.submit.callbacks.emplace_back([swapchain = std::move(_swapchain),
                                                    source    = std::move(source_texture)] {});
            recorded.deferred_releases  = TakeDeferredReleases();
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

void VkCommandQueue::Present(SwapchainRef, TextureView, PresentReceiptRef _receipt) {
    LogQueueErrorNoexcept(
        "[RHIExecutor][Vulkan] direct Present is unsupported; "
        "publish work through RHIExecutor"
    );
    vk_device.RecordRejectedPresent();
    ResolvePresentReceiptNoexcept(_receipt, false, EPresentStatus::Rejected, EPresentStage::Validation);
}

VulkanRuntimeSubmissionResult VkCommandQueue::PresentSwapchain(
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
    PendingSubmission     recorded{CmdSubmit{{}, {}, {}, {}}, context, _timeline};
    auto*                 swapchain = ResourceCast(_swapchain.Get());
    VulkanOperationResult final_outcome{EVulkanOperationStatus::Rejected, VK_ERROR_UNKNOWN};
    bool                  present_accepted      = false;
    bool                  recoverable_rejection = false;
    EPresentStatus        receipt_status        = EPresentStatus::Unresolved;
    EPresentStage         receipt_stage         = EPresentStage::None;

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
                   !static_cast<VulkanTexture*>(_source_view.texture)->IsPresentationSourceReady()) {
#if MOER_VULKAN_TEST_SUPPORT
            InvokePresentSourceRejectionCallbackForTesting(scripted_override);
#endif
            final_outcome = ClassifyPresentSourceRejection(vk_device, recoverable_rejection);
            vk_device.RecordRejectedPresent();
            AccumulatePresentReceiptOutcome(receipt_status, receipt_stage, final_outcome, EPresentStage::Validation);
            LOG_ERROR(
                "[RHIExecutor][Vulkan] rejected Present source contract: {}",
                source_contract.valid ? "producer export is not ready" : source_contract.reason
            );
        } else {
            recorded.command_contexts.submitted.push_back(AcquireCommandContext());
            auto&      command_context = *recorded.command_contexts.submitted.back();
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
                const uint            image_index       = acquire.image_index;
                auto*                 swapchain_texture = ResourceCast(swapchain->GetSwapchainImage(image_index).texture);
                VK_CHECK_RESULT(command_list.Begin());
                const std::string present_label = std::format("Present: {}", source_texture->GetName());
                command_list.BeginLabel(present_label, {0.0f, 0.85f, 0.95f, 1.0f});
                // The producer exports GENERAL; Present consumes that boundary.
                barriers.Append(
                    source_texture,
                    VulkanEnumTranslator::METoVKImageAspectFlags(source_texture->GetAspectFlags()),
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
                    VK_IMAGE_LAYOUT_GENERAL,
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

                recorded.ordered_cmd_lists.push_back(&command_list);
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
                recorded.deferred_releases = TakeDeferredReleases();
                // Keep Copy inputs alive until Present has been issued, even
                // if GPU execution finishes immediately.
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
    VulkanRuntimeSubmissionResult result{
        .outcome               = final_outcome,
        .recoverable_rejection = recoverable_rejection,
    };
    if (gpu_submitted) {
        result.completion = WaitEvent{uint64(timeline), _timeline};
    }
    return result;
}

} // namespace Moer::Render
