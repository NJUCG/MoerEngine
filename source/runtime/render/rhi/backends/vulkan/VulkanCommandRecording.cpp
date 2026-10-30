#include "PixelFormat.h"
#include "VulkanBarrier.h"
#include "VulkanCommandProfiler.h"
#include "VulkanCommandRecorder.h"
#include "VulkanCustomCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "VulkanPipeline.h"
#include "VulkanQueueInternal.h"
#include "VulkanRaytracingResource.h"
#include "log/LogSystem.h"
#include "misc/Alignment.h"
#include "misc/Timer.h"
#include "misc/Types.h"
#include "rhi/RHIMultiview.h"
#include "shader/ShaderPipeline.h"
#if MOER_VULKAN_TEST_SUPPORT
#include "vulkan/VulkanSubmissionTestHooksInternal.h"
#endif

#include <algorithm>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <variant>

namespace Moer::Render {

namespace {
void ValidateTextureTransferDataSize(const VulkanTexture& _texture, uint3 _extent, uint64 _byte_size) {
    const uint64 expected_byte_size = GetSizeFromImageFormat(_texture.GetFormat(), _extent);
    if (_byte_size != expected_byte_size) {
        throw std::invalid_argument(
            std::format(
                "Texture transfer data size mismatch: texture='{}', bytes={}, expected={}",
                _texture.GetName(),
                _byte_size,
                expected_byte_size
            )
        );
    }
}

VkRenderingAttachmentInfo FromColorAttachmentInfo(const ColorAttachment& _attachment) {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    VulkanTexture* vk_texture = static_cast<VulkanTexture*>(_attachment.target);
    attachment_info.imageView = vk_texture->GetAttachmentView(
        static_cast<uint8>(_attachment.mip_level),
        1,
        static_cast<uint8>(_attachment.array_layer),
        static_cast<uint8>(_attachment.array_count)
    );

    attachment_info.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment_info.loadOp      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(_attachment.action));
    attachment_info.storeOp     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_attachment.action));

    attachment_info.clearValue.color = {
        _attachment.clear_color.x, _attachment.clear_color.y, _attachment.clear_color.z, _attachment.clear_color.w
    };

    return attachment_info;
}

static bool FormatHasStencil(EPixelFormat format) {
    return format == PF_D32_SFLOAT_S8_UINT || format == PF_D24_UNORM_S8_UINT || format == PF_D16_UNORM_S8_UINT ||
           format == PF_S8_UINT;
}

static bool PipelineUsesStencilAttachment(const PipelineHandle& pipeline) {
    if (!pipeline.IsValid()) {
        return false;
    }

    auto* vk_pso = static_cast<VulkanPipelineState*>(pipeline.GetState());
    return vk_pso->UsesStencilAttachment();
}

static uint32 PipelineViewMask(const PipelineHandle& pipeline) {
    if (!pipeline.IsValid()) {
        return 0;
    }

    auto* vk_pso = static_cast<VulkanPipelineState*>(pipeline.GetState());
    return vk_pso->GetViewMask();
}

static void ValidateRenderPassMultiview(const RenderPassInfo& pass_info) {
    const uint32 required_view_count = Multiview::RequiredViewCount(pass_info.view_mask);
    auto         validate_attachment = [required_view_count, view_mask = pass_info.view_mask](
                                           Texture*         texture,
                                           uint32           mip_level,
                                           uint32           base_layer,
                                           uint32           layer_count,
                                           std::string_view attachment_name
                                       ) {
        if (texture == nullptr || mip_level >= texture->GetNumMips() ||
            !Multiview::IsAttachmentRangeValid(base_layer, layer_count, texture->GetNumArray())) {
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
            attachment.target, attachment.mip_level, attachment.array_layer, attachment.array_count, "color"
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
    const uint32 pipeline_view_mask = PipelineViewMask(pipeline);
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

    VulkanTexture* vk_texture = static_cast<VulkanTexture*>(_attachment.target);
    attachment_info.imageView = vk_texture->GetAttachmentView(
        static_cast<uint8>(_attachment.mip_level),
        1,
        static_cast<uint8>(_attachment.array_layer),
        static_cast<uint8>(_attachment.array_count)
    );

    bool has_stencil = FormatHasStencil(_attachment.target->GetFormat());
    attachment_info.imageLayout =
        has_stencil ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    attachment_info.loadOp =
        VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(GetDepthAction(_attachment.action)));
    attachment_info.storeOp =
        VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(GetDepthAction(_attachment.action)));
    attachment_info.clearValue.depthStencil = {_attachment.clear_depth, _attachment.clear_stencil};

    return attachment_info;
}

static std::optional<uint32> UniformRenderPassSampleCount(const RenderPassInfo& _pass_info) {
    std::optional<uint32>   sample_count;
    auto                    merge_samples = [&](const Texture* _texture) {
        if (_texture == nullptr) {
            return false;
        }
        const uint32 attachment_samples = _texture->GetNumSamples();
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
    if (_pass_info.depth_attachment.Valid() && !merge_samples(_pass_info.depth_attachment.target)) {
        return std::nullopt;
    }
    return sample_count.value_or(1u);
}

// Owns every array/address referenced by the Vulkan rendering and inheritance
// structures. Build it on the coordinator before worker dispatch, then use the
// same immutable signature for vkBeginCommandBuffer and vkCmdBeginRendering.
class VulkanDynamicRenderingEnvelope {
public:
    VulkanDynamicRenderingEnvelope(const RenderPassInfo& _pass_info, bool _uses_stencil_attachment) :
        pass_info(&_pass_info),
        uses_stencil_attachment(_uses_stencil_attachment) {
        const std::optional<uint32> uniform_samples = UniformRenderPassSampleCount(_pass_info);
        if (!uniform_samples.has_value()) {
            throw std::logic_error("dynamic rendering attachments use incompatible sample counts");
        }
        rasterization_samples = VulkanEnumTranslator::METoVKSampleCountFlagBits(uniform_samples.value());

        color_attachments.reserve(_pass_info.color_attachments.size());
        color_formats.reserve(_pass_info.color_attachments.size());
        for (const ColorAttachment& attachment : _pass_info.color_attachments) {
            color_attachments.push_back(FromColorAttachmentInfo(attachment));
            color_formats.push_back(VulkanEnumTranslator::METoVKFormat(attachment.target->GetFormat()));
        }
        if (_pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment.emplace(FromDepthAttachmentInfo(_pass_info.depth_attachment));
            depth_format   = VulkanEnumTranslator::METoVKFormat(_pass_info.depth_attachment.target->GetFormat());
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
            .colorAttachmentCount = static_cast<uint32>(color_attachments.size()),
            .pColorAttachments    = color_attachments.data(),
            .pDepthAttachment     = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr,
            .pStencilAttachment   = depth_stencil_attachment.has_value() && uses_stencil_attachment ?
                                        &depth_stencil_attachment.value() :
                                        nullptr,
        };
    }

    VkCommandBufferInheritanceRenderingInfo InheritanceInfo() const {
        return {
            .sType                   = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO,
            .pNext                   = nullptr,
            .flags                   = 0,
            .viewMask                = pass_info->view_mask,
            .colorAttachmentCount    = static_cast<uint32>(color_formats.size()),
            .pColorAttachmentFormats = color_formats.data(),
            .depthAttachmentFormat   = depth_format,
            .stencilAttachmentFormat = stencil_format,
            .rasterizationSamples    = rasterization_samples,
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
    VkSampleCountFlagBits                    rasterization_samples{VK_SAMPLE_COUNT_1_BIT};
};

} // namespace

class StaleBindlessUpdateBatch final : public std::runtime_error {
public:
    StaleBindlessUpdateBatch() :
        std::runtime_error("stale bindless update batch cannot be submitted with dependent commands") {}
};

std::shared_ptr<const VulkanDescriptorRecordingPlan> PrepareDescriptorRecordingPlan(
    VulkanDevice&                      _device,
    const CmdSubmit&                   _submit,
    const VulkanDescriptorBufferLease& _lease
) {
    auto                               plan = std::make_shared<VulkanDescriptorRecordingPlan>();
    std::vector<std::vector<uint64>>   requests;
    std::vector<const Command*>        commands;
    for (const auto& command : _submit.cmds) {
        std::vector<uint64>   binds;
        auto                  add = [&](const PipelineHandle& pipeline, size_t count = 1) {
            const auto* state = static_cast<const VulkanPipelineState*>(pipeline.GetState());
            if (state->GetDescriptorMode() != VulkanDescriptorMode::Sets) {
                const uint64 size = state->binding_template->resource_descriptor_allocation_size;
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

class VulkanCommandVisitor : VulkanDeviceObject {
    VulkanCommandList&     cmd_list;
    VulkanCommandContext&  command_context;
    const TCachedArgArray& cached_args;
    ProfilerStorage*       profiler                = nullptr;
    bool                   query_profiling_enabled = false;

    class ScopedNativeLabel {
    public:
        ScopedNativeLabel(VulkanCommandList& _cmd_list, std::string_view _name, float4 _color) : cmd_list(_cmd_list) {
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
    VulkanCommandVisitor(
        VulkanDevice&          _device,
        VulkanCommandContext&  _command_context,
        VulkanCommandList&     _cmd_list,
        const TCachedArgArray& _cached_args,
        ProfilerStorage*       _profiler                = nullptr,
        bool                   _query_profiling_enabled = false
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
                break;
            case Command::EType::SetDrawState:
                Visit(static_cast<const SetDrawStateCmd&>(*_cmd));
                break;
            case Command::EType::MultiDraw:
                Visit(static_cast<const MultiDrawCmd&>(*_cmd));
                break;
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
        auto              tmp_buffer = _cmd.staging_buffer;
        VulkanBuffer*     buffer     = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
        cmd_list.CopyBuffer(
            static_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            buffer,
            _cmd.ByteSize(),
            tmp_buffer.GetByteOffset(),
            _cmd.Offset()
        );
    }

    void Visit(const UploadTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        auto              tmp_buffer = _cmd.staging_buffer;
        VulkanTexture*    texture    = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
        cmd_list.CopyBufferToTexture(
            static_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
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
        VulkanBuffer*     src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
        VulkanBuffer*     dst_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

        cmd_list.CopyBuffer(src_buffer, dst_buffer, _cmd.ByteSize(), _cmd.SrcOffset(), _cmd.DstOffset());
    }

    void Visit(const CopyBackBufferCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanBuffer*     src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
        auto              tmp_buffer = _cmd.staging_buffer;

        cmd_list.CopyBuffer(
            src_buffer,
            static_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            _cmd.ByteSize(),
            _cmd.Offset(),
            tmp_buffer.GetByteOffset()
        );

        if (_cmd.HasOwningReadback()) {
            const ReadbackToken readback     = _cmd.OwningReadback();
            const uint64        logical_size = _cmd.ByteSize();
            command_context.AddOnComplete([tmp_buffer, device = m_device, readback, logical_size] {
                struct WriterContext {
                    VulkanDevice* device{};
                    BufferView    staging{};
                    uint64        byte_size{};
                } context{device, tmp_buffer, logical_size};
                (void)ReadbackBackendAccess::MaterializePayload(
                    readback, &context, [](void* _context, std::span<byte> _destination) {
                        auto* writer = static_cast<WriterContext*>(_context);
                        if (writer == nullptr || _destination.size_bytes() != writer->byte_size) {
                            throw std::runtime_error("Vulkan buffer readback payload size mismatch");
                        }
                        writer->device->CopyData(_destination.data(), writer->staging, writer->byte_size);
                    }
                );
            });
        } else {
            command_context.AddOnComplete(
                [tmp_buffer, device = m_device, destination(_cmd.Data()), logical_size = _cmd.ByteSize()] {
                    device->CopyData(destination, tmp_buffer, logical_size);
                }
            );
        }
    }

    void Visit(const CopyBackTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanTexture*    src_texture = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
        auto              tmp_buffer  = _cmd.staging_buffer;

        cmd_list.CopyTextureToBuffer(
            src_texture,
            static_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            _cmd.Offset(),
            tmp_buffer.GetByteOffset(),
            _cmd.Size(),
            _cmd.MipLevel(),
            _cmd.ArrayLayer()
        );

        if (_cmd.HasOwningReadback()) {
            const ReadbackToken readback     = _cmd.OwningReadback();
            const uint64        logical_size = _cmd.ByteSize();
            command_context.AddOnComplete([tmp_buffer, device = m_device, readback, logical_size] {
                struct WriterContext {
                    VulkanDevice* device{};
                    BufferView    staging{};
                    uint64        byte_size{};
                } context{device, tmp_buffer, logical_size};
                (void)ReadbackBackendAccess::MaterializePayload(
                    readback, &context, [](void* _context, std::span<byte> _destination) {
                        auto* writer = static_cast<WriterContext*>(_context);
                        if (writer == nullptr || _destination.size_bytes() != writer->byte_size) {
                            throw std::runtime_error("Vulkan texture readback payload size mismatch");
                        }
                        writer->device->CopyData(_destination.data(), writer->staging, writer->byte_size);
                    }
                );
            });
        } else {
            command_context.AddOnComplete(
                [tmp_buffer, device = m_device, destination(_cmd.Data()), logical_size = _cmd.ByteSize()] {
                    device->CopyData(destination.data(), tmp_buffer, logical_size);
                }
            );
        }
    }

    void Visit(const CopyTextureCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanTexture*    src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
        VulkanTexture*    dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

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
        VulkanBuffer*     src_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
        VulkanTexture*    dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

        cmd_list.CopyBufferToTexture(
            src_buffer, dst_texture, _cmd.SrcOffset(), _cmd.DstOffset(), _cmd.Size(), _cmd.MipLevel(), _cmd.ArrayLayer()
        );
    }

    void Visit(const CopyTextureToBufferCmd& _cmd) {
        ScopedNativeLabel marker(cmd_list, _cmd.name, GpuMarkerPalette::Transfer());
        VulkanTexture*    src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
        VulkanBuffer*     dst_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

        cmd_list.CopyTextureToBuffer(
            src_texture, dst_buffer, _cmd.SrcOffset(), _cmd.DstOffset(), _cmd.Size(), _cmd.MipLevel(), _cmd.ArrayLayer()
        );
    }

    void Visit(const DispatchCmd& _cmd) {
        static float4 dispatch_color = {0.0f, 0.0f, 1.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, dispatch_color);
        const auto& param                = _cmd.Param();
        const auto  profile_section_name = _cmd.ProfileSectionName();
        const bool  query_timestamp      = query_profiling_enabled && profile_section_name != "Other";

        if (query_timestamp) {
            assert(profiler && "profiler is not set");
            profiler->BeginProfilerSession(cmd_list, profile_section_name, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
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
                        static_cast<VulkanBuffer*>(_param.indirect.GetBuffer()), _param.indirect.GetByteOffset()
                    );
                }
            },
            param
        );

        if (query_timestamp) {
            profiler->EndProfilerSession(cmd_list, profile_section_name, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }

        cmd_list.EndLabel();
    }

    // Explicit barriers are emitted by VulkanCommandPreparation before dispatch.
    void Visit(const BarrierCmd&) {}

    void SetRenderArea(const Rect2D& rect) {
        VkViewport viewport{
            .x        = float(rect.offset.x),
            .y        = float(rect.offset.y) + float(rect.extent.height),
            .width    = float(rect.extent.width),
            .height   = -float(rect.extent.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f
        };
        cmd_list.SetViewport(viewport);
        cmd_list.SetScissor({rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height});
    }

    void RecordMeshDraw(const MeshDrawData& draw_data) {
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

                    cmd_list.SetIndexBuffer(
                        static_cast<VulkanBuffer*>(index_buffer.GetBuffer()),
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
                        VulkanBuffer* indirect_buffer = ResourceCast(draw_data.indirect_draw_param->buffer.GetBuffer());
                        if (draw_data.indirect_draw_param->count_buffer.has_value()) {
                            //draw indirect with count buffer
                            auto* count_buffer = ResourceCast(draw_data.indirect_draw_param->count_buffer->GetBuffer());
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
                [&](uint32) {
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
                        VulkanBuffer* indirect_buffer = ResourceCast(draw_data.indirect_draw_param->buffer.GetBuffer());
                        if (draw_data.indirect_draw_param->count_buffer.has_value()) {
                            //draw indirect with count buffer
                            auto* count_buffer = ResourceCast(draw_data.indirect_draw_param->count_buffer->GetBuffer());
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

    void Visit(const SetDrawStateCmd& _cmd) {

        ValidateRenderPassMultiview(_cmd.RenderPassInfo());
        ValidatePipelineViewMask(_cmd.Pipeline(), _cmd.RenderPassInfo());

        static float4 draw_color = {0.0f, 1.0f, 0.0f, 1.0f};
        cmd_list.BeginLabel(_cmd.name, draw_color);

        const auto&           args = _cmd.Args();
        const PipelineHandle& pso  = _cmd.Pipeline();

        const auto& pass_info = _cmd.RenderPassInfo();

        uint32 tex_min_width  = pass_info.render_area.extent.width + pass_info.render_area.offset.x;
        uint32 tex_min_height = pass_info.render_area.extent.height + pass_info.render_area.offset.y;

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
        const bool uses_stencil_attachment = pass_info.depth_attachment.Valid() &&
                                             FormatHasStencil(pass_info.depth_attachment.target->GetFormat()) &&
                                             PipelineUsesStencilAttachment(_cmd.Pipeline());
        VulkanDynamicRenderingEnvelope rendering_envelope(pass_info, uses_stencil_attachment);
        VkRenderingInfo                dynamic_rendering_info = rendering_envelope.RenderingInfo();
        cmd_list.BeginRendering(dynamic_rendering_info);

        cmd_list.BindPipeline(_cmd.Pipeline());

        cmd_list.BindShaderArguments(pso, args);

        const auto& draw_datas = _cmd.DrawData();
        SetRenderArea(pass_info.render_area);
        for (const MeshDrawData& draw_data : draw_datas) {
            RecordMeshDraw(draw_data);
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

        const auto& pass_info               = _cmd.RenderPassInfo();
        const bool  uses_stencil_attachment = pass_info.depth_attachment.Valid() &&
                                             FormatHasStencil(pass_info.depth_attachment.target->GetFormat()) &&
                                             DrawBatchUsesStencilAttachment(_cmd.draw_batch);
        VulkanDynamicRenderingEnvelope rendering_envelope(pass_info, uses_stencil_attachment);
        VkRenderingInfo                dynamic_rendering_info = rendering_envelope.RenderingInfo();
        cmd_list.BeginRendering(dynamic_rendering_info);
        SetRenderArea(pass_info.render_area);

        for (const DrawBatchElement& draw_cmd : _cmd.draw_batch.draw_cmds) {

            cmd_list.BindPipeline(draw_cmd.handle);
            const ArrayArguments& arguments = std::visit(
                Overload{
                    [](const ArrayArguments& args) -> const ArrayArguments& {
                        return args;
                    },
                    [&](ArrayArgReference reference) -> const ArrayArguments& {
                        return cached_args.at(reference());
                    },
                    [](TEmptyShaderArg) -> const ArrayArguments& {
                        throw std::invalid_argument("Vulkan MultiDraw requires shader arguments");
                    }
                },
                draw_cmd.args
            );
            cmd_list.BindShaderArguments(draw_cmd.handle, arguments);

            std::visit(
                Overload{
                    [&](const Array<MeshDrawData>& _mesh_draw_cmds) {
                        for (const MeshDrawData& draw_data : _mesh_draw_cmds) {
                            RecordMeshDraw(draw_data);
                        }
                    },

                    [&](const Array<DispatchMeshData>& _mesh) {
                        for (const auto& draw_data : _mesh) {
                            std::visit(
                                Overload{
                                    [&](const IndirectDrawParam& _indirect) {
                                        VulkanBuffer* indirect_buffer = ResourceCast(_indirect.buffer.GetBuffer());
                                        if (_indirect.count_buffer.has_value()) {
                                            //draw indirect with count buffer
                                            auto* count_buffer = ResourceCast(_indirect.count_buffer->GetBuffer());
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
                            [&](uint32 _val) {
                                value = VkClearColorValue{.uint32 = {_val, _val, _val, _val}};
                            }
                        },
                        _cmd.ClearValue()
                    );
                    cmd_list.ClearColorTexture(vk_texture, value, range);
                },
                [&](BufferView _arg) {
                    auto* vk_buffer = ResourceCast(_arg.GetBuffer());
                    cmd_list.ClearBufferUInt(vk_buffer, _arg.GetByteOffset(), _arg.GetByteSize(), _cmd.UIntValue());
                }
            },
            _cmd.Resource()
        );
    }

    void Visit(const UpdateBindlessArrayCmd& _cmd) {
        VulkanBindlessArray* bindless_array = static_cast<VulkanBindlessArray*>(_cmd.Handle());
        if (!_cmd.HandOffUpdates()) {
            return;
        }
        const bool       update_is_current = bindless_array->BeginUpdateCommand(_cmd.UpdateCommands());
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
            LOG_WARNING("Rejecting command submission containing a stale bindless update batch");
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
            uint32 storage_alignment = 256u;

            //calculate all staging size
            uint32 current_offset = 0;
            uint32 staging_size = 0, array_staging_offset = 0, texture_staging_offset = 0, buffer_staging_offset = 0;
            uint32 array_staging_size = 0, texture_staging_size = 0, buffer_staging_size = 0;
            uint32 array_indices_offset = 0, texture_indices_offset = 0, buffer_indices_offset = 0;
            uint32 array_indices_size = 0, texture_indices_size = 0, buffer_indices_size = 0;

            if (b_array) {
                array_staging_size   = array_data.size();
                staging_size         = array_staging_size;
                array_indices_offset = Moer::AlignUp(array_staging_size, storage_alignment);

                array_indices_size = sizeof(uint32) * (array_indices_dat.size()) * 2;

                current_offset = Moer::AlignUp(array_indices_size + array_indices_offset, storage_alignment);
            }

            if (b_texture) {
                texture_staging_size   = texture_data.size();
                texture_staging_offset = current_offset;

                texture_indices_offset =
                    Moer::AlignUp(texture_staging_size + texture_staging_offset, storage_alignment);
                texture_indices_size = sizeof(uint32) * texture_indices_dat.size() * 2;
                current_offset       = Moer::AlignUp(texture_indices_size + texture_indices_offset, storage_alignment);
            }

            if (b_buffer) {
                buffer_staging_size   = buffer_data.size();
                buffer_staging_offset = current_offset;
                buffer_indices_offset = Moer::AlignUp(buffer_staging_size + buffer_staging_offset, storage_alignment);
                buffer_indices_size   = sizeof(uint32) * buffer_indices_dat.size() * 2;

                current_offset = Moer::AlignUp(buffer_indices_size + buffer_indices_offset, storage_alignment);
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
                texture_desc_staging = staging_buffer.buffer->GetView(texture_staging_offset, texture_staging_size);
                texture_indices_buf  = staging_buffer.buffer->GetView(texture_indices_offset, texture_indices_size);
                m_device->CopyData(texture_desc_staging, texture_data.data(), texture_staging_size);
                m_device->CopyData(texture_indices_buf, texture_indices_dat.data(), texture_indices_size);
            }

            if (b_buffer) {
                buffer_desc_staging = staging_buffer.buffer->GetView(buffer_staging_offset, buffer_staging_size);
                buffer_indices_buf  = staging_buffer.buffer->GetView(buffer_indices_offset, buffer_indices_size);
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
                arg.stride        = sizeof(uint32) >> 2;

                cmd_list.BindShaderArguments(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg, array_indices_buf, array_staging, bindless_array->GetIndexBuffer()->GetView()
                    )
                );
                cmd_list.Dispatch((array_indices_dat.size() + 63) / 64, 1, 1);
            }

            if (b_texture) {
                arg.component_cnt = texture_indices_dat.size();
                arg.stride =
                    m_device->GetGlobalDescriptorHeap().GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) >> 2;

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
                arg.stride =
                    m_device->GetGlobalDescriptorHeap().GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) >> 2;

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
            build_info.mode          = VulkanEnumTranslator::METoVKBuildAccelerationStructureMode(build_param.mode);
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

        VulkanBuffer*                instance_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.InstanceBufferHandle());
        VulkanBuffer*                scratch_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.ScratchBufferHandle());
        VulkanAccelerationStructure* tlas            = reinterpret_cast<VulkanAccelerationStructure*>(_cmd.TlasHandle());

        if (to_update.size() == 0 && !_cmd.ForceUpdate()) {
            return;
        }

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

            Array<std::pair<uint32, uint32>> to_update_indices(to_update.size());

            for (size_t i = 0; i < to_update.size(); ++i) {
                const auto& id       = to_update[i];
                to_update_indices[i] = {i, id};
            }

            m_device->CopyData(
                staging, _cmd.InstanceData().data(), to_update.size() * sizeof(VkAccelerationStructureInstanceKHR)
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
                (instance_buffer->GetByteSize() - alignment_offset) / sizeof(VkAccelerationStructureInstanceKHR),
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
        build_info.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR; //now force build each frame

        VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType                       = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.flags                              = 0;
        geometry.geometry.instances.sType           = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
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

        uint32 instance_count = _cmd.InstanceCount();
        vkGetAccelerationStructureBuildSizesKHR(
            m_device->GetDevice(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &build_info,
            &instance_count,
            &size_infos
        );

        assert(size_infos.accelerationStructureSize > 0 && "Invalid acceleration structure size!");
        assert(size_infos.buildScratchSize <= scratch_buffer->GetByteSize() && "Invalid scratch buffer size!");

        const uint64 scratch_alignment =
            m_device->GetOptionalProperties()
                .acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;
        build_info.scratchData.deviceAddress = Moer::AlignUp(scratch_buffer->DeviceAddress(), scratch_alignment);

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
                assert(false && "Custom Command Not Supported for VulkanCommandVisitor");
        }
        cmd_list.EndLabel();
    }

    void Visit(const VkCustomDispatchCmd& _cmd) {
        const VkCustomDispatchCmd::VkDispatchContext context = {
            m_device->GetInstance(), m_device->GetGpu(), m_device->GetDevice(), cmd_list.GetHandle()
        };
        _cmd.Execute(context);
    }
};

// Validate before native recording or consuming command payloads. Build/update
// commands retain their backend-private protocols; application dependencies
// must already be compiled into the submission.
bool VulkanCommandRecorder::SupportsCompiledCommands(const CmdSubmit& submit, EQueueType queue) {
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
    VulkanCommandVisitor&         visitor,
    const UpdateBindlessArrayCmd& command
) {
    auto*              table = static_cast<VulkanBindlessArray*>(command.Handle());
    VulkanBarrierBatch before, after;
    auto               protect = [&](VulkanBuffer* buffer, uint64 offset, uint64 size, VkAccessFlags2 read_access) {
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
            table->bindings.size() * heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            table->GetDescriptorReadAccess()
        );
    }
    if (command.HasTextureUpdates()) {
        protect(
            table->GetTextureSamplerDescriptorBuffer(),
            table->texture_offset_in_buffer,
            table->bindings.size() * heap.GetDescriptorSize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
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
    VulkanCommandList&              native,
    VulkanCommandVisitor&           visitor,
    const UpdateRaytracingSceneCmd& command
) {
    if (command.InstancesToUpdate().empty() && !command.ForceUpdate()) {
        return;
    }
    VulkanBarrierBatch before;
    const auto         protect = [&](VulkanBuffer* buffer, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask       = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask        = stage;
        barrier.dstAccessMask       = access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer              = buffer->GetHandle();
        barrier.size                = VK_WHOLE_SIZE;
        before.buffer_barriers.push_back(barrier);
    };
    protect(
        ResourceCast(command.InstanceBuffer().Get()),
        command.InstancesToUpdate().empty() ? VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR :
                                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        command.InstancesToUpdate().empty() ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_SHADER_WRITE_BIT
    );
    protect(
        ResourceCast(command.ScratchBuffer().Get()),
        VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
        VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
    );
    before.Record(native);
    // The visitor records the private upload -> build dependency. Public BLAS
    // input and TLAS output dependencies have already been emitted by RDG.
    visitor.VisitCmd(&command);
}

// Backend-private allocation and explicit barrier preparation. Each staging
// slice is exclusively owned until completion; no application state is inferred.
struct VulkanCommandPreparation {
    VulkanDevice&                   device;
    VulkanTemporaryBufferAllocator& temporary_buffer_allocator;
    EQueueType                      current_queue;
    VulkanBarrierRecorder           barriers;

    void UploadDependency(const BufferView& view) {
        barriers.Append(
            ResourceCast(view.GetBuffer()),
            view.GetByteOffset(),
            view.GetByteSize(),
            EBarrierQueueTransferPhase::None,
            VK_QUEUE_FAMILY_IGNORED,
            VK_QUEUE_FAMILY_IGNORED,
            VK_PIPELINE_STAGE_2_HOST_BIT,
            VK_ACCESS_2_HOST_WRITE_BIT,
            VK_PIPELINE_STAGE_2_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT
        );
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
                scratch_size += param.mode == ERaytracingBuildMode::BUILD ? vk_geo->build_sizes_info.buildScratchSize :
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

        if (_cmd->GetSrcQueue() != current_queue || _cmd->GetDstQueue() != current_queue) {
            LOG_CRITICAL(
                "Explicit BarrierCmd queue affinity mismatch: command={} "
                "current={}",
                static_cast<uint32>(_cmd->GetSrcQueue()),
                static_cast<uint32>(current_queue)
            );
            throw std::logic_error("explicit barrier recorded on the wrong queue");
        }

        const auto resolve_transfer = [&](const BarrierQueueTransfer& transfer) {
            std::pair<uint32, uint32> families{
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
                throw std::invalid_argument("Vulkan ownership barrier carries an unknown phase");
            }
            if (transfer.external) {
                const bool release  = transfer.phase == EBarrierQueueTransferPhase::Release;
                const auto managed  = release ? transfer.src_queue : transfer.dst_queue;
                const auto external = release ? transfer.dst_queue : transfer.src_queue;
                if (managed != current_queue || external != EQueueType::Ignore) {
                    throw std::invalid_argument("Vulkan external ownership barrier endpoint mismatch");
                }
                const auto family = device.GetQueueFamilyIndex(managed);
                if (family == VK_QUEUE_FAMILY_IGNORED) {
                    throw std::invalid_argument("Vulkan external ownership barrier has no native queue");
                }
                return release ? std::pair{family, uint32(VK_QUEUE_FAMILY_EXTERNAL)} :
                                 std::pair{uint32(VK_QUEUE_FAMILY_EXTERNAL), family};
            }
            if ((transfer.src_queue != EQueueType::Graphics && transfer.src_queue != EQueueType::Compute &&
                 transfer.src_queue != EQueueType::Copy) ||
                (transfer.dst_queue != EQueueType::Graphics && transfer.dst_queue != EQueueType::Compute &&
                 transfer.dst_queue != EQueueType::Copy) ||
                transfer.src_queue == transfer.dst_queue) {
                throw std::invalid_argument(
                    "Vulkan ownership barrier requires distinct managed "
                    "Graphics/Compute/Copy endpoints"
                );
            }
            const EQueueType expected_queue =
                transfer.phase == EBarrierQueueTransferPhase::Release ? transfer.src_queue : transfer.dst_queue;
            if (current_queue != expected_queue) {
                throw std::logic_error("Vulkan ownership barrier recorded on the wrong endpoint");
            }
            families.first  = device.GetQueueFamilyIndex(transfer.src_queue);
            families.second = device.GetQueueFamilyIndex(transfer.dst_queue);
            if (families.first == VK_QUEUE_FAMILY_IGNORED || families.second == VK_QUEUE_FAMILY_IGNORED ||
                families.first == families.second) {
                throw std::invalid_argument("Vulkan ownership barrier requires two distinct queue families");
            }
            return families;
        };

        for (const ExplicitBufferBarrier& barrier : _cmd->ExplicitBuffers()) {
            if (barrier.src_state.texture_layout != ETextureLayout::TEXTURE_LAYOUT_UNDEFINED ||
                barrier.dst_state.texture_layout != ETextureLayout::TEXTURE_LAYOUT_UNDEFINED) {
                throw std::invalid_argument("explicit buffer barrier cannot carry a texture layout");
            }
            const auto [src_family, dst_family] = resolve_transfer(barrier.queue_transfer);
            barriers.Append(
                reinterpret_cast<VulkanBuffer*>(barrier.handle),
                barrier.offset,
                barrier.byte_size,
                barrier.queue_transfer.phase,
                src_family,
                dst_family,
                VulkanEnumTranslator::METoVkPipelineStageFlags2(barrier.src_state.stage),
                VulkanEnumTranslator::METoVkAccessFlags2(barrier.src_state.access),
                VulkanEnumTranslator::METoVkPipelineStageFlags2(barrier.dst_state.stage),
                VulkanEnumTranslator::METoVkAccessFlags2(barrier.dst_state.access)
            );
        }
        for (const ExplicitTextureBarrier& barrier : _cmd->ExplicitTextures()) {
            const VkImageLayout src_layout = VulkanEnumTranslator::METoVKImageLayout(barrier.src_state.texture_layout);
            const VkImageLayout dst_layout = VulkanEnumTranslator::METoVKImageLayout(barrier.dst_state.texture_layout);
            if (src_layout == VK_IMAGE_LAYOUT_MAX_ENUM || dst_layout == VK_IMAGE_LAYOUT_MAX_ENUM) {
                throw std::invalid_argument("explicit texture barrier carries an unsupported layout");
            }
            const auto [src_family, dst_family] = resolve_transfer(barrier.queue_transfer);
            barriers.Append(
                reinterpret_cast<VulkanTexture*>(barrier.handle),
                VulkanEnumTranslator::METoVKImageAspectFlags(barrier.texture_aspects),
                barrier.mip_level,
                barrier.mip_count,
                barrier.array_layer,
                barrier.array_count,
                barrier.queue_transfer.phase,
                src_family,
                dst_family,
                VulkanEnumTranslator::METoVkPipelineStageFlags2(barrier.src_state.stage),
                VulkanEnumTranslator::METoVkAccessFlags2(barrier.src_state.access),
                src_layout,
                VulkanEnumTranslator::METoVkPipelineStageFlags2(barrier.dst_state.stage),
                VulkanEnumTranslator::METoVkAccessFlags2(barrier.dst_state.access),
                dst_layout
            );
        }
    }
};

void RecordCompiledCommands(
    VulkanDevice&         device,
    VulkanCommandContext& context,
    CmdSubmit&            submit,
    VulkanCommandVisitor& visitor,
    EQueueType            queue,
    bool                  suppress_timestamp_queries = false
) {
    VulkanCommandPreparation preparation{device, context.GetTemporaryBufferAllocator(), queue};
    for (const auto& command : submit.cmds) {
        if (suppress_timestamp_queries && command->Type() == Command::EType::Query) {
            continue;
        }
        if (command->Type() == Command::EType::UpdateBindlessArray) {
            RecordCompiledBindlessUpdate(
                device, context.GetCommandList(), visitor, static_cast<const UpdateBindlessArrayCmd&>(*command)
            );
            continue;
        }
        if (command->Type() == Command::EType::BuildAccel) {
            const auto* build = static_cast<const BuildAccelerationStructuresCmd*>(command.get());
            preparation.Visit(build);
            // Scratch is backend-private. Vertex/index inputs and BLAS output
            // are exclusively synchronized by the graph's declared resources.
            VulkanBarrierBatch     before;
            VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            barrier.srcStageMask  = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            barrier.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier.dstStageMask  = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
            barrier.dstAccessMask =
                VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer                                            = ResourceCast(build->Scratch().GetBuffer())->GetHandle();
            barrier.offset                                            = build->Scratch().byte_offset;
            barrier.size                                              = build->Scratch().GetByteSize();
            before.buffer_barriers.push_back(barrier);
            before.Record(context.GetCommandList());
            visitor.VisitCmd(command.get());
            continue;
        }
        if (command->Type() == Command::EType::BuildTLAS) {
            RecordCompiledTlasUpdate(
                context.GetCommandList(), visitor, static_cast<const UpdateRaytracingSceneCmd&>(*command)
            );
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
                                        static_cast<const CopyBackTextureCmd*>(command.get())->staging_buffer :
                                        BufferView{};
        if (readback.GetBuffer()) {
            preparation.barriers.Append(
                ResourceCast(readback.GetBuffer()),
                readback.GetByteOffset(),
                readback.GetByteSize(),
                EBarrierQueueTransferPhase::None,
                VK_QUEUE_FAMILY_IGNORED,
                VK_QUEUE_FAMILY_IGNORED,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_HOST_BIT,
                VK_ACCESS_2_HOST_READ_BIT
            );
            preparation.barriers.Flush(context.GetCommandList());
        }
    }
}

void VulkanCommandRecorder::RecordCopyCommands(
    VulkanCommandContext& context,
    CmdSubmit&            submit,
    bool                  suppress_timestamp_queries
) {
    auto&                 native = context.GetCommandList();
    VulkanCommandVisitor  visitor(device, context, native, submit.cached_args);
    VK_CHECK_RESULT(native.Begin());
    native.BeginLabel("Copy", {0.0f, 1.0f, 1.0f, 1.0f});
    RecordCompiledCommands(device, context, submit, visitor, EQueueType::Copy, suppress_timestamp_queries);
    native.EndLabel();
    VK_CHECK_RESULT(native.End());
}

namespace {

struct VulkanCommandRecordingFailure {
    std::string_view reason;
    VkResult         result;
};

struct QueryRequirements {
    uint32 timestamps{0};
    uint32 occlusions{0};
};

QueryRequirements ValidateRecordingRequest(VulkanDevice& device, const CmdSubmit& submit, EQueueType queue) {
    size_t timestamps = 0;
    size_t occlusions = 0;
    for (const auto& command : submit.cmds) {
        if (!command || command->Type() != Command::EType::Query) {
            continue;
        }
        const auto& query = static_cast<const QueryCmd&>(*command);
        timestamps += query.IsTimestamp();
        occlusions += query.IsOcclusion() && query.IsBegin();
    }
    if (timestamps != 0 && device.GetTimestampValidBits(queue) == 0) {
        throw VulkanCommandRecordingFailure{
            "queue family does not support timestamp queries", VK_ERROR_FEATURE_NOT_PRESENT
        };
    }
    if (occlusions != 0 && queue != EQueueType::Graphics) {
        throw VulkanCommandRecordingFailure{
            "occlusion queries require the Graphics queue", VK_ERROR_FEATURE_NOT_PRESENT
        };
    }
    if (timestamps > std::numeric_limits<uint32>::max() || occlusions > std::numeric_limits<uint32>::max()) {
        throw VulkanCommandRecordingFailure{"query capacity exceeds uint32", VK_ERROR_OUT_OF_POOL_MEMORY};
    }
    if (!submit.cmds.empty() && submit.resource_state_ownership != ERHIResourceStateOwnership::Compiled) {
        throw VulkanCommandRecordingFailure{
            "submission did not provide a compiled resource plan", VK_ERROR_FEATURE_NOT_PRESENT
        };
    }
    return {static_cast<uint32>(timestamps), static_cast<uint32>(occlusions)};
}

void PrepareQueryPools(
    VulkanCommandContext&    context,
    const QueryRequirements& queries,
    EQueueType               queue,
    uint64                   timeline
) {
#if MOER_VULKAN_TEST_SUPPORT
    if (queries.timestamps != 0) {
        if (const auto* override = GetVulkanScriptedQueryPreparationOverrideForTesting()) {
            VkResult result = override->callback(override->context, queue, timeline, queries.timestamps);
            // A scripted preparation failure must not masquerade as device loss.
            if (result == VK_ERROR_DEVICE_LOST) {
                result = VK_ERROR_FEATURE_NOT_PRESENT;
            }
            if (result != VK_SUCCESS) {
                throw VulkanCommandRecordingFailure{"scripted timestamp query preparation rejection", result};
            }
        }
    }
#endif
    // Reserve before Begin(): preparation failure must not follow native side effects.
    context.EnsureQueryCapacity(queries.timestamps, queries.occlusions);
}

} // namespace

void VulkanCommandRecorder::RecordCommands(VulkanRecordedSubmission& pending) {
    assert(GetCurrentRHIThreadRole() == ERHIThreadRole::Translate);
    if (device.IsFaulted()) {
        RejectRecording(pending, {EVulkanOperationStatus::Rejected, device.GetFirstFaultResult()}, "device is faulted");
        return;
    }

    QueryRequirements queries;
    try {
        pending.presentation_source_program = BuildPresentationSourceStateProgram(pending.submit, queue_type);
        queries                             = ValidateRecordingRequest(device, pending.submit, queue_type);
        if (!SupportsCompiledCommands(pending.submit, queue_type)) {
            throw VulkanCommandRecordingFailure{
                "compiled submission contains an unsupported command or inferred barrier", VK_ERROR_FEATURE_NOT_PRESENT
            };
        }
    } catch (const VulkanCommandRecordingFailure& failure) {
        RejectRecording(pending, {EVulkanOperationStatus::Rejected, failure.result}, failure.reason);
        return;
    } catch (const std::logic_error& error) {
        RejectRecording(pending, {EVulkanOperationStatus::Rejected, VK_ERROR_FEATURE_NOT_PRESENT}, error.what());
        return;
    } catch (const std::bad_alloc& error) {
        RejectRecording(pending, {EVulkanOperationStatus::Rejected, VK_ERROR_OUT_OF_HOST_MEMORY}, error.what());
        return;
    }

    std::shared_ptr<const VulkanDescriptorRecordingPlan> descriptor_plan;
    try {
        pending.descriptor_lease = device.GetGlobalDescriptorHeap().CreateDescriptorBufferLease();
        descriptor_plan          = PrepareDescriptorRecordingPlan(device, pending.submit, pending.descriptor_lease);
    } catch (const std::bad_alloc&) {
        RejectRecording(
            pending,
            {EVulkanOperationStatus::Rejected, VK_ERROR_OUT_OF_HOST_MEMORY},
            "descriptor reservation metadata allocation failed"
        );
        return;
    }
    if (!descriptor_plan) {
        RejectRecording(
            pending,
            device.IsFaulted() ? VulkanOperationResult{EVulkanOperationStatus::Faulted, device.GetFirstFaultResult()} :
                                 VulkanOperationResult{EVulkanOperationStatus::Rejected, VK_ERROR_OUT_OF_POOL_MEMORY},
            "descriptor batch does not fit available storage"
        );
        return;
    }

    // Establish resource ownership before recording. The reserved vectors let
    // success transfer a context without allocating after native side effects.
    pending.command_contexts.abandoned.reserve(1);
    pending.command_contexts.submitted.reserve(1);
    pending.ordered_cmd_lists.reserve(1);
    pending.command_contexts.abandoned.emplace_back(AcquireContext());
    VulkanCommandContext& context = *pending.command_contexts.abandoned.back();
    try {
        PrepareQueryPools(context, queries, queue_type, pending.timeline);
    } catch (const VulkanCommandRecordingFailure& failure) {
        RejectRecording(pending, {EVulkanOperationStatus::Rejected, failure.result}, failure.reason);
        return;
    } catch (const std::exception& error) {
        const bool faulted = device.IsFaulted();
        RejectRecording(
            pending,
            {faulted ? EVulkanOperationStatus::Faulted : EVulkanOperationStatus::Rejected,
             faulted ? device.GetFirstFaultResult() : VK_ERROR_OUT_OF_POOL_MEMORY},
            error.what()
        );
        return;
    } catch (...) {
        const bool faulted = device.IsFaulted();
        RejectRecording(
            pending,
            {faulted ? EVulkanOperationStatus::Faulted : EVulkanOperationStatus::Rejected,
             faulted ? device.GetFirstFaultResult() : VK_ERROR_OUT_OF_POOL_MEMORY},
            "query pool preparation failed"
        );
        return;
    }

    const auto fail_recording = [&](std::string_view reason, VkResult result) {
        // Native recording can have CPU-side effects. Do not replay this request.
        device.TryLatchFirstFault(pending.context, result, false, false, true);
        RejectRecording(pending, {EVulkanOperationStatus::Rejected, result}, reason);
        pending.deferred_releases = TakeDeferredReleases();
    };
    double elapsed_ms = 0.0;
    try {
        elapsed_ms = RecordPreparedCommands(pending, descriptor_plan);
    } catch (const VulkanCommandRecordingFailure& failure) {
        fail_recording(failure.reason, failure.result);
        return;
    } catch (const StaleBindlessUpdateBatch&) {
        fail_recording("stale-bindless-update", VK_ERROR_VALIDATION_FAILED_EXT);
        return;
    } catch (const std::exception& error) {
        fail_recording(error.what(), VK_ERROR_OUT_OF_POOL_MEMORY);
        return;
    } catch (...) {
        fail_recording("unknown-recording-exception", VK_ERROR_UNKNOWN);
        return;
    }

    pending.deferred_releases = TakeDeferredReleases();
    if (!pending.submit.cmds.empty()) {
        pending.ordered_cmd_lists.emplace_back(&context.GetCommandList());
    }
    pending.command_contexts.submitted.emplace_back(std::move(pending.command_contexts.abandoned.back()));
    pending.command_contexts.abandoned.clear();
    profiler->FinishRecording(pending.submit, elapsed_ms);
}

double VulkanCommandRecorder::RecordPreparedCommands(
    VulkanRecordedSubmission&                                   _pending,
    const std::shared_ptr<const VulkanDescriptorRecordingPlan>& _descriptor_plan
) {
    const CmdSubmit&      submit                   = _pending.submit;
    VulkanCommandContext& context                  = *_pending.command_contexts.abandoned.back();
    VulkanCommandList&    command_list             = context.GetCommandList();
    const bool            has_cmd                  = !submit.cmds.empty();
    const bool            begins_profiling_frame   = submit.BeginsProfilingFrame();
    const bool            ends_profiling_frame     = submit.EndsProfilingFrame();
    const bool            complete_profiling_frame = submit.ProfilingPhase() == ERHIProfilingPhase::Complete;
    Timer                 timer;
    VulkanCommandVisitor  visitor(
        device, context, command_list, submit.cached_args, &profiler->storage, submit.EmitsProfilingQueries()
    );
    if (has_cmd) {
        command_list.SetDescriptorRecordingPlan(_descriptor_plan);
        const VkResult begin_result = command_list.Begin();
        if (begin_result != VK_SUCCESS) {
            throw VulkanCommandRecordingFailure{"command-buffer-begin-failed", begin_result};
        }
        if (begins_profiling_frame) {
            profiler->storage.CollectProfiling(command_list.GetHandle());

            {
                std::unique_lock<std::mutex> profiler_lock(profiler->mutex);
                profiler->cached_entry = profiler->storage.GetProfilerEntry();
            }
            profiler->storage.BeginProfilerSession(command_list, "Graphics Exec");

            if (complete_profiling_frame) {
                profiler->ResetSplitFrame();
                timer.Start();
            } else {
                profiler->BeginSplitFrame();
            }
        }

        const std::string_view queue_label = !submit.debug_label.empty()        ? std::string_view(submit.debug_label) :
                                             queue_type == EQueueType::Graphics ? "Graphics Exec" :
                                             queue_type == EQueueType::Compute  ? "Compute Exec" :
                                                                                  "Copy Exec";
        const float4           queue_label_color =
            !submit.debug_label.empty() ? submit.debug_label_color : float4{1.0f, 0.0f, 0.0f, 1.0f};
        command_list.BeginLabel(queue_label, queue_label_color);
    }

    RecordCompiledCommands(device, context, _pending.submit, visitor, queue_type);
    if (has_cmd) {
        if (ends_profiling_frame) {
            profiler->storage.EndProfilerSession(command_list, "Graphics Exec");
        }
        command_list.EndLabel();
        const VkResult end_result = command_list.End();
        if (end_result != VK_SUCCESS) {
            throw VulkanCommandRecordingFailure{"command-buffer-end-failed", end_result};
        }
        device.GetGlobalDescriptorHeap().FlushDescriptorWrites(_pending.descriptor_lease);
    }

    if (has_cmd && complete_profiling_frame) {
        timer.Stop();
        return timer.ElapsedMilliseconds();
    }
    return 0.0;
}

} // namespace Moer::Render
