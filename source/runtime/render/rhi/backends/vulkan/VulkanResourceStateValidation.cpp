#include "VulkanResourceStateValidation.h"
#include "VulkanCustomCommand.h"
#include "VulkanDevice.h"
#include "VulkanPipeline.h"
#include "VulkanRHIResource.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIThreadOwnership.h"

#include <algorithm>
#include <cassert>
#include <format>
#include <iterator>
#include <stdexcept>

namespace Moer::Render {
namespace {
constexpr uint32 UnknownFamily = ~uint32{0};

bool StagesInclude(ERHIPipelineStageFlags available, ERHIPipelineStageFlags required) {
    using Stage = ERHIPipelineStageFlags;
    if ((available & Stage::PS_ALL_COMMANDS) != Stage::PS_NONE) {
        return true;
    }
    if ((available & Stage::PS_ALL_GRAPHICS) != Stage::PS_NONE) {
        available |= Stage::PS_DRAW_INDIRECT | Stage::PS_VERTEX_INPUT | Stage::PS_VERTEX_SHADER |
                     Stage::PS_TESSELLATION_CONTROL_SHADER | Stage::PS_TESSELLATION_EVALUATION_SHADER |
                     Stage::PS_GEOMETRY_SHADER | Stage::PS_FRAGMENT_SHADER | Stage::PS_EARLY_FRAGMENT_TESTS |
                     Stage::PS_LATE_FRAGMENT_TESTS | Stage::PS_COLOR_ATTACHMENT_OUTPUT | Stage::PS_TASK_SHADER | Stage::PS_MESH_SHADER;
    }
    return (available & required) == required;
}

ERHIPipelineStageFlags ShaderStages(const VulkanArgumentInfo* info) {
    using Stage = ERHIPipelineStageFlags;
    if (!info) {
        return Stage::PS_NONE;
    }
    constexpr std::pair<VkPipelineStageFlags2, Stage> stages[] = {
        {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, Stage::PS_VERTEX_SHADER},
        {VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT, Stage::PS_TESSELLATION_CONTROL_SHADER},
        {VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT, Stage::PS_TESSELLATION_EVALUATION_SHADER},
        {VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT, Stage::PS_GEOMETRY_SHADER},
        {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, Stage::PS_FRAGMENT_SHADER},
        {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, Stage::PS_COMPUTE_SHADER},
        {VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR, Stage::PS_RAY_TRACING_SHADER},
        {VK_PIPELINE_STAGE_2_TASK_SHADER_BIT_EXT, Stage::PS_TASK_SHADER},
        {VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT, Stage::PS_MESH_SHADER},
    };
    auto result = Stage::PS_NONE;
    for (const auto& [native, stage] : stages) {
        if ((info->stages & native) != 0) {
            result |= stage;
        }
    }
    return result;
}

bool Includes(ERHIAccessFlags available, ERHIAccessFlags required) {
    if ((available & ERHIAccessFlags::MEMORY_READ) != ERHIAccessFlags::UNDEFINED) {
        available |= ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::TRANSFER_READ |
                     ERHIAccessFlags::UNIFORM_READ | ERHIAccessFlags::INPUT_ATTACHMENT_READ |
                     ERHIAccessFlags::SHADER_SAMPLED_READ | ERHIAccessFlags::SHADER_RESOURCE_VIEW |
                     ERHIAccessFlags::CPU_READ_BIT | ERHIAccessFlags::ACCELERATION_STRUCTURE_READ_BIT |
                     ERHIAccessFlags::VERTEX_ATTRIBUTE_READ | ERHIAccessFlags::INDEX_READ |
                     ERHIAccessFlags::INDIRECT_COMMAND_READ | ERHIAccessFlags::COLOR_ATTACHMENT_READ |
                     ERHIAccessFlags::DEPTH_STENCIL_READ;
    }
    if ((available & ERHIAccessFlags::MEMORY_WRITE) != ERHIAccessFlags::UNDEFINED) {
        available |= ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::TRANSFER_WRITE |
                     ERHIAccessFlags::COLOR_ATTACHMENT_WRITE | ERHIAccessFlags::DEPTH_STENCIL_WRITE;
    }
    return (available & required) == required;
}

// The callback receives normalized, half-open intervals. A texture cell never
// aliases another aspect, mip or layer; image extent is irrelevant to layouts.
template<typename F>
void Intervals(const RHIResourceAccess& access, F&& visit) {
    std::visit([&](const auto& view) {
        using View = std::decay_t<decltype(view)>;
        if constexpr (std::is_same_v<View, TextureView>) {
            const auto* texture = view.GetTexture();
            if (!texture || !view.num_mips || !view.num_array ||
                uint32(view.mip_level) + view.num_mips > texture->GetNumMips() ||
                uint32(view.array_layer) + view.num_array > texture->GetNumArray() ||
                access.texture_aspects == ETextureAspectFlags::NONE ||
                (access.texture_aspects & texture->GetAspectFlags()) != access.texture_aspects) {
                throw std::logic_error("resource access has an invalid texture range/aspect");
            }
            for (uint32 bit = 0; bit < 8; ++bit) {
                if ((uint32(access.texture_aspects) & (1u << bit)) == 0) {
                    continue;
                }
                for (uint32 mip = view.mip_level; mip < uint32(view.mip_level) + view.num_mips; ++mip) {
                    const uint64 first = (uint64(bit) * texture->GetNumMips() + mip) * texture->GetNumArray() + view.array_layer;
                    visit(first, first + view.num_array);
                }
            }
        } else {
            if (!view.GetBuffer() || !view.GetByteSize() || view.GetByteOffset() > view.GetBuffer()->GetByteSize() ||
                view.GetByteSize() > view.GetBuffer()->GetByteSize() - view.GetByteOffset()) {
                throw std::logic_error("resource access has an invalid buffer range");
            }
            visit(view.GetByteOffset(), view.GetByteOffset() + view.GetByteSize());
        }
    },
               access.resource);
}

const RHIResource* Identity(const RHIResourceAccess& access) {
    return std::visit([](const auto& view) -> const RHIResource* {
        if constexpr (std::is_same_v<std::decay_t<decltype(view)>, TextureView>) {
            return view.GetTexture();
        } else {
            return view.GetBuffer();
        }
    },
                      access.resource);
}

std::string_view ResourceName(const RHIResourceAccess& access) {
    return std::visit([](const auto& view) {
        if constexpr (std::is_same_v<std::decay_t<decltype(view)>, TextureView>) {
            return view.GetTexture()->GetName();
        } else {
            return view.GetBuffer()->GetName();
        }
    },
                      access.resource);
}

bool Covered(std::span<const RHIResourceAccess> declarations, const RHIResourceAccess& actual, bool storage_binding = false) {
    bool covered = true;
    Intervals(actual, [&](uint64 first, uint64 end) {
        while (first < end) {
            uint64 next = first;
            for (const auto& declaration : declarations) {
                const bool access_matches = storage_binding ?
                                                (declaration.state.access & (ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE)) != ERHIAccessFlags::UNDEFINED :
                                                Includes(declaration.state.access, actual.state.access);
                if (Identity(declaration) != Identity(actual) || !access_matches ||
                    !StagesInclude(declaration.state.stage, actual.state.stage) ||
                    (std::holds_alternative<TextureView>(actual.resource) && declaration.state.texture_layout != actual.state.texture_layout)) {
                    continue;
                }
                Intervals(declaration, [&](uint64 a, uint64 b) {
                    if (a <= first && first < b) {
                        next = std::max(next, std::min(end, b));
                    }
                });
            }
            if (next == first) {
                covered = false;
                break;
            }
            first = next;
        }
    });
    return covered;
}

RHIResourceAccess TextureAccess(Texture* texture, uint32 mip, uint32 layer, uint32 layers, ERHIAccessFlags access, ETextureLayout layout) {
    if (!texture) {
        throw std::logic_error("null texture access");
    }
    TextureView view(texture);
    view.mip_level   = static_cast<uint8>(mip);
    view.array_layer = static_cast<uint8>(layer);
    view.num_mips    = 1;
    view.num_array   = static_cast<uint8>(layers);
    auto stage       = ERHIPipelineStageFlags::PS_TRANSFER;
    if ((access & ERHIAccessFlags::COLOR_ATTACHMENT_WRITE) != ERHIAccessFlags::UNDEFINED) {
        stage = ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT;
    } else if ((access & (ERHIAccessFlags::DEPTH_STENCIL_READ | ERHIAccessFlags::DEPTH_STENCIL_WRITE)) != ERHIAccessFlags::UNDEFINED) {
        stage = ERHIPipelineStageFlags::PS_EARLY_FRAGMENT_TESTS | ERHIPipelineStageFlags::PS_LATE_FRAGMENT_TESTS;
    }
    return {view, {stage, access, layout}, texture->GetAspectFlags()};
}

RHIResourceAccess BufferAccess(Buffer* buffer, uint64 offset, uint64 size, ERHIAccessFlags access) {
    auto stage = ERHIPipelineStageFlags::PS_TRANSFER;
    if ((access & (ERHIAccessFlags::VERTEX_ATTRIBUTE_READ | ERHIAccessFlags::INDEX_READ)) != ERHIAccessFlags::UNDEFINED) {
        stage = ERHIPipelineStageFlags::PS_VERTEX_INPUT;
    } else if ((access & ERHIAccessFlags::INDIRECT_COMMAND_READ) != ERHIAccessFlags::UNDEFINED) {
        stage = ERHIPipelineStageFlags::PS_DRAW_INDIRECT;
    }
    return {BufferView(buffer, offset, size, 1), BarrierState::Buffer(stage, access)};
}

void ValidateDeclaredAccess(
    const ResourceAccessScopeCmd*               scope,
    const Array<const ResourceAccessScopeCmd*>& parents,
    const Command&                              current,
    const RHIResourceAccess&                    access,
    bool                                        storage_binding
) {
    Intervals(access, [](uint64, uint64) {});
    bool       covered = !scope;
    const auto inspect = [&](const ResourceAccessScopeCmd* candidate) {
        if (!candidate || std::none_of(candidate->accesses.begin(), candidate->accesses.end(), [&](const auto& declaration) {
                return Identity(declaration) == Identity(access);
            })) {
            return false;
        }
        covered = Covered(candidate->accesses, access, storage_binding);
        return true;
    };
    if (scope && !inspect(scope)) {
        // Child graphs may describe only their private buffers. Resources
        // inherited from an enclosing pass retain that pass's declaration;
        // a child's own declaration still takes precedence over its parent.
        for (auto it = parents.rbegin(); it != parents.rend(); ++it) {
            if (inspect(*it)) {
                break;
            }
        }
    }
    if (!covered) {
        throw std::logic_error(std::format("pass '{}' command '{}' accesses undeclared resource '{}' or range", scope->name, current.name, ResourceName(access)));
    }
}

template<typename Record>
void CollectShaderArgumentAccesses(
    const TArg&                                 argument,
    const VulkanArgumentInfo*                   info,
    const ResourceAccessScopeCmd*               scope,
    const Array<const ResourceAccessScopeCmd*>& parents,
    const Record&                               record
) {
    std::visit([&](const auto& resource) {
        using Resource = std::decay_t<decltype(resource)>;
        if constexpr (std::is_same_v<Resource, BindlessArrayRef>) {
            const auto candidates = [](const ResourceAccessScopeCmd* declaration) {
                return declaration && std::any_of(declaration->accesses.begin(), declaration->accesses.end(), [](const auto& access) {
                           return (access.state.access & (ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE)) != ERHIAccessFlags::UNDEFINED;
                       });
            };
            if (resource && (!scope || (!candidates(scope) && std::none_of(parents.begin(), parents.end(), candidates)))) {
                throw std::logic_error("bindless access requires an explicit resource-access scope and candidate declarations");
            }
        } else if constexpr (std::is_same_v<Resource, TextureView> || std::is_same_v<Resource, TextureViewArray> ||
                             std::is_same_v<Resource, BufferView> || std::is_same_v<Resource, BufferViewArray>) {
            if (info && info->resource_type == SRT_INVALID) {
                return;
            }
            const bool writable = info && info->IsWritable();
            const auto use      = [&](const auto& view) {
                RHIResourceAccess access{view};
                access.state = {ShaderStages(info), writable ? ERHIAccessFlags::SHADER_WRITE : ERHIAccessFlags::SHADER_READ, writable || (info && info->image_sampling == VulkanImageSampling::Storage) ? ETextureLayout::TEXTURE_LAYOUT_COMMON : ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                if constexpr (std::is_same_v<std::decay_t<decltype(view)>, TextureView>) {
                    if (!view.GetTexture()) {
                        throw std::logic_error("null shader texture access");
                    }
                    access.texture_aspects = view.GetTexture()->GetAspectFlags();
                } else {
                    access.state.texture_layout = ETextureLayout::TEXTURE_LAYOUT_UNDEFINED;
                }
                record(access, writable);
            };
            if constexpr (std::is_same_v<Resource, TextureViewArray> || std::is_same_v<Resource, BufferViewArray>) {
                for (const auto& view : resource) {
                    use(view);
                }
            } else {
                use(resource);
            }
        }
    },
               argument);
}

template<typename Record>
void CollectPipelineAccesses(
    const ArrayArguments&                       arguments,
    const PipelineHandle&                       pipeline,
    const ResourceAccessScopeCmd*               scope,
    const Array<const ResourceAccessScopeCmd*>& parents,
    const Record&                               record
) {
    const auto* state = static_cast<const VulkanPipelineState*>(pipeline.GetState());
    if (!state) {
        throw std::logic_error("resource-state validation requires a valid shader pipeline");
    }
    const auto infos = state->GetArgumentInfos();
    for (size_t i = 0; i < arguments.args.size(); ++i) {
        if (i >= 64 || (state->GetActiveArgumentBits() & (uint64{1} << i)) == 0) {
            continue;
        }
        CollectShaderArgumentAccesses(arguments.args[i], i < infos.size() ? &infos[i] : nullptr, scope, parents, record);
    }
}

template<typename Record>
void CollectAttachmentAccesses(const RenderPassInfo& pass, bool scoped, const Record& record) {
    for (const auto& attachment : pass.color_attachments) {
        record(TextureAccess(attachment.target, attachment.mip_level, attachment.array_layer, attachment.array_count, ERHIAccessFlags::COLOR_ATTACHMENT_WRITE, ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT));
    }
    if (pass.depth_attachment.Valid()) {
        const auto& attachment = pass.depth_attachment;
        auto        access     = ERHIAccessFlags::DEPTH_STENCIL_READ;
        // An unscoped draw conservatively permits depth writes. A compiled
        // pass supplies their exact contract, but a clear always writes.
        if (!scoped || GetLoadOp(GetDepthAction(attachment.action)) == EAttachmentLoadOp::CLEAR ||
            ((attachment.target->GetAspectFlags() & ETextureAspectFlags::STENCIL_SLICE) != ETextureAspectFlags::NONE &&
             GetLoadOp(GetStencilAction(attachment.action)) == EAttachmentLoadOp::CLEAR)) {
            access |= ERHIAccessFlags::DEPTH_STENCIL_WRITE;
        }
        record(TextureAccess(attachment.target, attachment.mip_level, attachment.array_layer, attachment.array_count, access, ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE));
    }
}

template<typename Draw, typename Record>
void CollectDrawAccesses(const Draw& draw, bool scoped, const Record& record) {
    const auto buffers = [&](const auto& values, ERHIAccessFlags access) {
        for (const auto& [buffer, range] : values) {
            const auto [offset, size] = range;
            // Legacy draw envelopes conservatively carry the whole buffer's
            // size even when its starting offset is nonzero.
            record(BufferAccess(buffer, offset, std::min(size, buffer->GetByteSize() - offset), access));
        }
    };
    buffers(draw.VertexBuffers(), ERHIAccessFlags::VERTEX_ATTRIBUTE_READ);
    buffers(draw.IndexBuffers(), ERHIAccessFlags::INDEX_READ);
    buffers(draw.IndirectBuffers(), ERHIAccessFlags::INDIRECT_COMMAND_READ);
    if constexpr (requires { draw.DrawCountBuffers(); }) {
        buffers(draw.DrawCountBuffers(), ERHIAccessFlags::INDIRECT_COMMAND_READ);
    }
    CollectAttachmentAccesses(draw.RenderPassInfo(), scoped, record);
}

void AppendExplicitBarriers(VulkanResourceStateValidator::Program& program, const BarrierCmd& barriers) {
    for (const auto& barrier : barriers.ExplicitTextures()) {
        if (!barrier.handle) {
            throw std::logic_error("null texture barrier");
        }
        TextureView view(reinterpret_cast<Texture*>(barrier.handle));
        view.mip_level               = barrier.mip_level;
        view.num_mips                = barrier.mip_count;
        view.array_layer             = barrier.array_layer;
        view.num_array               = barrier.array_count;
        auto value                   = BarrierCreateInfo::Transition(view, barrier.src_state, barrier.dst_state, barrier.texture_aspects);
        value.queue_transfer         = barrier.queue_transfer;
        value.publish_external_state = barrier.publish_external_state;
        program.emplace_back(value);
    }
    for (const auto& barrier : barriers.ExplicitBuffers()) {
        auto value                   = BarrierCreateInfo::Transition(BufferView(reinterpret_cast<Buffer*>(barrier.handle), barrier.offset, barrier.byte_size, 1), barrier.src_state, barrier.dst_state);
        value.queue_transfer         = barrier.queue_transfer;
        value.publish_external_state = barrier.publish_external_state;
        program.emplace_back(value);
    }
}

template<typename Record>
void CollectCommandAccesses(
    const CmdSubmit&                            submit,
    const Command&                              command,
    const ResourceAccessScopeCmd*               scope,
    const Array<const ResourceAccessScopeCmd*>& parents,
    const Record&                               record
) {
    switch (command.Type()) {
        case Command::EType::UploadBuffer: {
            const auto& c = static_cast<const UploadBufferCmd&>(command);
            record(BufferAccess(reinterpret_cast<Buffer*>(c.Handle()), c.Offset(), c.ByteSize(), ERHIAccessFlags::TRANSFER_WRITE));
            break;
        }
        case Command::EType::CopyBackBuffer: {
            const auto& c = static_cast<const CopyBackBufferCmd&>(command);
            record(BufferAccess(reinterpret_cast<Buffer*>(c.Handle()), c.Offset(), c.ByteSize(), ERHIAccessFlags::TRANSFER_READ));
            break;
        }
        case Command::EType::BufferToBuffer: {
            const auto& c = static_cast<const CopyBufferCmd&>(command);
            record(BufferAccess(reinterpret_cast<Buffer*>(c.SrcHandle()), c.SrcOffset(), c.ByteSize(), ERHIAccessFlags::TRANSFER_READ));
            record(BufferAccess(reinterpret_cast<Buffer*>(c.DstHandle()), c.DstOffset(), c.ByteSize(), ERHIAccessFlags::TRANSFER_WRITE));
            break;
        }
        case Command::EType::UploadTexture: {
            const auto& c = static_cast<const UploadTextureCmd&>(command);
            record(TextureAccess(reinterpret_cast<Texture*>(c.Handle()), c.MipLevel(), c.ArrayLayer(), 1, ERHIAccessFlags::TRANSFER_WRITE, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST));
            break;
        }
        case Command::EType::CopyBackTexture: {
            const auto& c = static_cast<const CopyBackTextureCmd&>(command);
            record(TextureAccess(reinterpret_cast<Texture*>(c.Handle()), c.MipLevel(), c.ArrayLayer(), 1, ERHIAccessFlags::TRANSFER_READ, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC));
            break;
        }
        case Command::EType::BufferToTexture: {
            const auto& c = static_cast<const CopyBufferToTextureCmd&>(command);
            record(BufferAccess(reinterpret_cast<Buffer*>(c.SrcHandle()), c.SrcOffset(), c.ByteSize(), ERHIAccessFlags::TRANSFER_READ));
            record(TextureAccess(reinterpret_cast<Texture*>(c.DstHandle()), c.MipLevel(), c.ArrayLayer(), 1, ERHIAccessFlags::TRANSFER_WRITE, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST));
            break;
        }
        case Command::EType::TextureToBuffer: {
            const auto& c = static_cast<const CopyTextureToBufferCmd&>(command);
            record(TextureAccess(reinterpret_cast<Texture*>(c.SrcHandle()), c.MipLevel(), c.ArrayLayer(), 1, ERHIAccessFlags::TRANSFER_READ, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC));
            record(BufferAccess(reinterpret_cast<Buffer*>(c.DstHandle()), c.DstOffset(), GetSizeFromImageFormat(c.Format(), c.Size()), ERHIAccessFlags::TRANSFER_WRITE));
            break;
        }
        case Command::EType::TextureToTexture: {
            const auto& c = static_cast<const CopyTextureCmd&>(command);
            record(TextureAccess(reinterpret_cast<Texture*>(c.SrcHandle()), c.SrcMipLevel(), c.SrcArrayLayer(), 1, ERHIAccessFlags::TRANSFER_READ, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC));
            record(TextureAccess(reinterpret_cast<Texture*>(c.DstHandle()), c.DstMipLevel(), c.DstArrayLayer(), 1, ERHIAccessFlags::TRANSFER_WRITE, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST));
            break;
        }
        case Command::EType::ShaderDispatch: {
            const auto& c = static_cast<const DispatchCmd&>(command);
            CollectPipelineAccesses(c.Args(submit.cached_args), c.Pipeline(), scope, parents, record);
            const auto parameter = c.Param();
            if (const auto* indirect = std::get_if<DispatchIndirectParam>(&parameter)) {
                record({indirect->indirect, BarrierState::Buffer(ERHIPipelineStageFlags::PS_DRAW_INDIRECT, ERHIAccessFlags::INDIRECT_COMMAND_READ)});
            }
            break;
        }
        case Command::EType::SetDrawState: {
            const auto& c = static_cast<const SetDrawStateCmd&>(command);
            CollectPipelineAccesses(c.Args(), c.Pipeline(), scope, parents, record);
            CollectDrawAccesses(c, scope != nullptr, record);
            break;
        }
        case Command::EType::MultiDraw: {
            const auto& c = static_cast<const MultiDrawCmd&>(command);
            for (const auto& element : c.draw_batch.draw_cmds) {
                if (const auto* args = std::get_if<ArrayArguments>(&element.args)) {
                    CollectPipelineAccesses(*args, element.handle, scope, parents, record);
                } else {
                    CollectPipelineAccesses(submit.cached_args.at(std::get<ArrayArgReference>(element.args).handle), element.handle, scope, parents, record);
                }
            }
            CollectDrawAccesses(c, scope != nullptr, record);
            break;
        }
        case Command::EType::TraceRay: {
            const auto& c = static_cast<const TraceRayCmd&>(command);
            CollectPipelineAccesses(c.Args(), c.Pipeline(), scope, parents, record);
            break;
        }
        case Command::EType::ClearResource: {
            const auto& c = static_cast<const ClearResourceCmd&>(command);
            if (c.IsTexture()) {
                if (!c.Texture().GetTexture()) {
                    throw std::logic_error("null clear texture");
                }
                record({c.Texture(), {ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_WRITE, ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST}, c.Texture().GetTexture()->GetAspectFlags()});
            } else {
                record({c.Buffer(), BarrierState::Buffer(ERHIPipelineStageFlags::PS_TRANSFER, ERHIAccessFlags::TRANSFER_WRITE)});
            }
            break;
        }
        case Command::EType::Custom: {
            const auto& c = static_cast<const CustomCmd&>(command);
            if (c.CustomId() != CustomCmd::CustomCmdId::CUSTOM_DISPATCH) {
                throw std::logic_error("opaque custom commands require explicit resource-access declarations");
            }
            const auto* native = dynamic_cast<const VkCustomDispatchCmd*>(&c);
            if (!native) {
                throw std::logic_error("Vulkan custom dispatch requires a Vulkan resource-usage contract");
            }
            for (const auto& usage : native->GetResourceUsages()) {
                CollectShaderArgumentAccesses(usage.resource, &usage.info, scope, parents, record);
            }
            break;
        }
        default:
            break; // Query/markers/table storage/AS have their own native protocols.
    }
}
} // namespace

VulkanResourceStateValidator::Program VulkanResourceStateValidator::Compile(const CmdSubmit& submit) {
    Program                              program;
    const ResourceAccessScopeCmd*        scope = nullptr;
    Array<const ResourceAccessScopeCmd*> parents;
    for (const auto& owner : submit.cmds) {
        if (!owner) {
            throw std::logic_error("resource-state validation encountered a null command");
        }
        const auto& current = *owner;
        if (!current.CachedArgumentReferencesAreValid(submit.cached_args.size())) {
            throw std::logic_error("resource-state validation encountered an invalid cached argument reference");
        }
        const auto record = [&](const RHIResourceAccess& access, bool storage_binding = false) {
            ValidateDeclaredAccess(scope, parents, current, access, storage_binding);
            // Scoped writable bindings are capabilities; scope declarations carry
            // the actual per-subresource accesses and were appended at scope entry.
            if (!storage_binding || !scope) {
                program.emplace_back(access);
            }
        };
        switch (current.Type()) {
            case Command::EType::ResourceAccessScope: {
                const auto& marker = static_cast<const ResourceAccessScopeCmd&>(current);
                if (marker.begin) {
                    parents.push_back(scope);
                    scope = &marker;
                    for (const auto& access : marker.accesses) {
                        Intervals(access, [](uint64, uint64) {});
                        program.emplace_back(access);
                    }
                } else {
                    if (!scope) {
                        throw std::logic_error("unmatched resource-access scope end");
                    }
                    scope = parents.back();
                    parents.pop_back();
                }
                break;
            }
            case Command::EType::Barrier:
                AppendExplicitBarriers(program, static_cast<const BarrierCmd&>(current));
                break;
            default:
                CollectCommandAccesses(submit, current, scope, parents, record);
                break;
        }
    }
    if (scope) {
        throw std::logic_error("unclosed resource-access scope");
    }
    return program;
}

VulkanResourceStateValidator::VulkanResourceStateValidator(VulkanDevice& value, EQueueType role, const Program& program, const CmdSubmit* submit, VulkanTimeline* timeline, uint64 submission_point) :
    device(value),
    queue(role),
    point(submission_point) {
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Submission && "accepted resource state belongs to Submission");
    if (submit) {
        const auto merge = [&](const VulkanQueueClock& other) {
            for (size_t i = 0; i < clock.size(); ++i) {
                clock[i] = std::max(clock[i], other[i]);
            }
        };
        const auto topology = device.GetQueueTopology();
        const auto binding  = topology.Resolve(queue);
        for (uint32 i = 0; i < clock.size(); ++i) {
            if (topology.Resolve(static_cast<EQueueType>(i)).native_queue_id == binding.native_queue_id) {
                merge(device.accepted_queue_clocks[i]);
            }
        }
        for (const auto& wait : submit->wait_events) {
            merge(reinterpret_cast<VulkanTimeline*>(wait.timeline_handle)->GetSubmissionClock(wait.value));
        }
        clock[static_cast<size_t>(queue)] = point;
        signals.emplace_back(timeline, point);
        for (const auto& signal : submit->signal_events) {
            signals.emplace_back(reinterpret_cast<VulkanTimeline*>(signal.timeline_handle), signal.value);
        }
    }
    for (const auto& instruction : program) {
        std::visit([&](const auto& value) {
            if constexpr (std::is_same_v<std::decay_t<decltype(value)>, RHIResourceAccess>) {
                Apply(value);
            } else {
                Apply({value.resource, value.dst_state, value.texture_aspects}, &value);
            }
        },
                   instruction);
    }
    try {
        for (const auto& [fence, signal] : signals) {
            fence->PrepareSubmissionClock(signal);
        }
    } catch (...) {
        for (const auto& [fence, signal] : signals) {
            fence->DiscardSubmissionClock(signal);
        }
        throw;
    }
}

VulkanResourceStateValidator::~VulkanResourceStateValidator() {
    if (point) {
        for (const auto& [fence, signal] : signals) {
            fence->DiscardSubmissionClock(signal);
        }
    }
}

void VulkanResourceStateValidator::Apply(const RHIResourceAccess& access, const BarrierCreateInfo* transition) {
    std::shared_ptr<const Snapshot>* destination        = nullptr;
    uint64                           total              = 0;
    const bool                       image              = std::holds_alternative<TextureView>(access.resource);
    bool                             exclusive_resource = image;
    std::visit([&](const auto& view) {
        if constexpr (std::is_same_v<std::decay_t<decltype(view)>, TextureView>) {
            auto* resource = ResourceCast(view.GetTexture());
            if (!resource) {
                throw std::logic_error("null texture access");
            }
            destination = &resource->accepted_resource_state;
            total       = uint64(8) * resource->GetNumMips() * resource->GetNumArray();
        } else {
            auto* resource = ResourceCast(view.GetBuffer());
            if (!resource) {
                throw std::logic_error("null buffer access");
            }
            destination        = &resource->accepted_resource_state;
            total              = resource->GetByteSize();
            exclusive_resource = !resource->concurrent_sharing;
        }
    },
               access.resource);
    auto working = std::find_if(changes.begin(), changes.end(), [&](const auto& entry) {
        return entry.first == destination;
    });
    if (working == changes.end()) {
        auto snapshot = *destination ? std::make_shared<Snapshot>(**destination) : std::make_shared<Snapshot>();
        if (snapshot->ranges.empty()) {
            snapshot->ranges.push_back({.first = 0, .end = total});
        }
        changes.emplace_back(destination, std::move(snapshot));
        working = std::prev(changes.end());
    }
    auto&        ranges = working->second->ranges;
    const uint32 family = device.GetQueueFamilyIndex(queue);
    const auto   fail   = [&](std::string_view reason) {
        throw std::logic_error(std::format("resource '{}' queue {}: {} (required layout={}, access={})", ResourceName(access), uint32(queue), reason, uint32(access.state.texture_layout), uint64(access.state.access)));
    };
    Intervals(access, [&](uint64 first, uint64 end) {
        const auto split = [&](uint64 point) {
            const auto it = std::find_if(ranges.begin(), ranges.end(), [&](const auto& r) {
                return r.first < point && point < r.end;
            });
            if (it != ranges.end()) {
                auto right  = *it;
                right.first = point;
                it->end     = point;
                ranges.insert(std::next(it), right);
            }
        };
        split(first);
        split(end);
        for (auto& range : ranges) {
            if (range.end <= first || end <= range.first) {
                continue;
            }
            const bool layout_change = image && transition &&
                                       transition->src_state.texture_layout != transition->dst_state.texture_layout;
            const bool exclusive = BarrierStateWrites(access.state) || layout_change ||
                                   (transition && transition->queue_transfer.phase != EBarrierQueueTransferPhase::None);
            const bool evidence_only = transition && transition->publish_external_state &&
                                       transition->src_state == transition->dst_state &&
                                       !BarrierStateWrites(transition->src_state) &&
                                       transition->queue_transfer.phase == EBarrierQueueTransferPhase::None;
            if (evidence_only && ((image && range.state.texture_layout != access.state.texture_layout) ||
                                  !Includes(range.state.access, access.state.access) ||
                                  !StagesInclude(range.state.stage, access.state.stage))) {
                fail("external publication requires already established read visibility");
            }
            if (point != 0) {
                const auto& predecessors = exclusive ? range.uses : range.writes;
                for (size_t i = 0; i < clock.size(); ++i) {
                    if (predecessors[i] > clock[i]) {
                        fail("conflicting access on another queue requires an explicit wait");
                    }
                }
            }
            if (!transition) {
                // Buffers have no image layout. A fresh byte range has no
                // preceding GPU access to synchronize; its first use establishes
                // the contract without inventing a mandatory initialization barrier.
                if (!image && range.family == UnknownFamily && !range.release_source &&
                    range.state.access == ERHIAccessFlags::UNDEFINED) {
                    range.state = access.state;
                    if (!BarrierStateWrites(access.state)) {
                        range.state.stage = ERHIPipelineStageFlags::PS_ALL_COMMANDS;
                    }
                    range.family = family;
                }
                if (!image && range.writes == VulkanQueueClock{} &&
                    !BarrierStateWrites(range.state) && !BarrierStateWrites(access.state)) {
                    range.state.access |= access.state.access;
                }
                if ((image && range.state.texture_layout != access.state.texture_layout) ||
                    !Includes(range.state.access, access.state.access) || !StagesInclude(range.state.stage, access.state.stage)) {
                    fail("access requires an explicit resource transition");
                }
                if (exclusive_resource && (range.release_source || (range.family != UnknownFamily && range.family != family))) {
                    fail("resource ownership does not permit this access");
                }
                if (BarrierStateWrites(access.state)) {
                    range.external = false;
                }
                if (point) {
                    range.uses[static_cast<size_t>(queue)] = point;
                    if (exclusive) {
                        range.writes                             = {};
                        range.writes[static_cast<size_t>(queue)] = point;
                    }
                }
                continue;
            }
            const auto& transfer = transition->queue_transfer;
            const auto  phase    = transfer.phase;
            if (exclusive_resource) {
                if (phase == EBarrierQueueTransferPhase::Acquire && !transfer.external) {
                    if (!range.release_source || range.released_to != family ||
                        range.release_source->texture_layout != transition->src_state.texture_layout ||
                        range.state.texture_layout != transition->dst_state.texture_layout) {
                        fail("queue acquire has no matching accepted release");
                    }
                } else if (phase == EBarrierQueueTransferPhase::Acquire && transfer.external) {
                    if (range.family != UnknownFamily && (!range.release_source || range.released_to != UnknownFamily)) {
                        fail("external acquire has no preceding external release");
                    }
                } else {
                    if (range.release_source || (range.family != UnknownFamily && range.family != family)) {
                        fail("transition uses a resource owned by another queue family");
                    }
                    if (image && transition->src_state.texture_layout != ETextureLayout::TEXTURE_LAYOUT_UNDEFINED &&
                        range.state.texture_layout != transition->src_state.texture_layout) {
                        fail("barrier source layout differs from the accepted state");
                    }
                }
            }
            auto destination_state = transition->dst_state;
            if (!BarrierStateWrites(range.state) && !BarrierStateWrites(destination_state) &&
                range.state.texture_layout == destination_state.texture_layout && range.state.access == destination_state.access &&
                range.family == family && !range.release_source) {
                destination_state.stage |= range.state.stage;
            }
            range.state    = destination_state;
            range.family   = family;
            range.external = transition->publish_external_state;
            if (point && !evidence_only) {
                range.uses[static_cast<size_t>(queue)] = point;
                if (exclusive) {
                    range.writes                             = {};
                    range.writes[static_cast<size_t>(queue)] = point;
                }
            }
            range.release_source.reset();
            range.released_to = UnknownFamily;
            if (exclusive_resource && phase == EBarrierQueueTransferPhase::Release) {
                range.release_source = transition->src_state;
                range.released_to    = transfer.external ? UnknownFamily : device.GetQueueFamilyIndex(transfer.dst_queue);
                range.external       = false;
            }
        }
    });
}

void VulkanResourceStateValidator::Commit() noexcept {
    for (auto& [destination, snapshot] : changes) {
        *destination = std::move(snapshot);
    }
    if (point) {
        device.accepted_queue_clocks[static_cast<size_t>(queue)] = clock;
        for (const auto& [fence, signal] : signals) {
            fence->PublishSubmissionClock(signal, clock);
        }
        point = 0;
    }
}

bool VulkanResourceStateValidator::ValidateExternalAccess(VulkanDevice& device, EQueueType queue, const RHIResourceAccess& access) noexcept {
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Submission && "accepted resource state belongs to Submission");
    try {
        const Snapshot* snapshot           = nullptr;
        bool            exclusive_resource = true;
        std::visit([&](const auto& view) {
            if constexpr (std::is_same_v<std::decay_t<decltype(view)>, TextureView>) {
                if (const auto* resource = ResourceCast(view.GetTexture())) {
                    snapshot = resource->accepted_resource_state.get();
                }
            } else {
                if (const auto* resource = ResourceCast(view.GetBuffer())) {
                    snapshot           = resource->accepted_resource_state.get();
                    exclusive_resource = !resource->concurrent_sharing;
                }
            }
        },
                   access.resource);
        if (!snapshot) {
            return false;
        }
        const bool image  = std::holds_alternative<TextureView>(access.resource);
        const auto family = device.GetQueueFamilyIndex(queue);
        bool       valid  = true;
        Intervals(access, [&](uint64 first, uint64 end) {
            uint64 covered = first;
            for (const auto& range : snapshot->ranges) {
                if (range.end <= first || end <= range.first) {
                    continue;
                }
                valid &= range.first <= covered && range.external &&
                         (!image || range.state.texture_layout == access.state.texture_layout) &&
                         Includes(range.state.access, access.state.access) && StagesInclude(range.state.stage, access.state.stage) &&
                         (!exclusive_resource || (!range.release_source && range.family == family));
                covered = std::max(covered, std::min(end, range.end));
            }
            valid &= covered == end;
        });
        return valid;
    } catch (...) {
        return false;
    }
}
} // namespace Moer::Render
