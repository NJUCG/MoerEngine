#include "VulkanQueue.h"
#include "PixelFormat.h"
#include "RHICmdReorderer.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanRHIResource.h"
#include "VulkanSerialGolden.h"
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
#include "vulkan/vulkan_core.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
namespace Moer::Render {

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
        for (const auto& key : tracker.GetWritedStateTextures()) {
            if (vk_bindless_array->IsTextureViewAllocated(
                    uint64(key.texture), key.mip_level, key.mip_count, key.array_layer, key.array_count
                ) &&
                !writed_texture_resources.contains(key)) {
                to_read_textures.push_back(key);
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
        Moer::Array<VulkanBuffer*> to_read_buffers;
        for (const auto& i : tracker.GetWritedStateBuffers()) {
            if (vk_bindless_array->IsResourceAllocated(uint64(i)) &&
                !writed_buffer_resources.contains(uint64(i))) {
                to_read_buffers.push_back(i);
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
            case Command::EType::Scope:
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
        // EQueueType current_queue = this->allocator.
        EQueueType temp_queue = this->current_queue;
        if (_cmd->IsImport()) {
            _cmd->dst_queue = temp_queue;

            for (auto& barrier : _cmd->ImportTextures()) {
                auto* vk_texture = ResourceCast(barrier.texture.GetTexture());
                auto  access     = tracker.ReadTexture(vk_texture, barrier.state);
                tracker.QueueTransferAcquireResource(
                    vk_texture,
                    device.GetQueueFamilyIndex(_cmd->src_queue),
                    device.GetQueueFamilyIndex(_cmd->dst_queue),
                    vk_texture->GetQueuePreferredLayout(_cmd->src_queue),
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
                    device.GetQueueFamilyIndex(_cmd->src_queue),
                    device.GetQueueFamilyIndex(_cmd->dst_queue),
                    VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                );
            }

        } else {
            _cmd->src_queue = temp_queue;

            for (auto& barrier : _cmd->ExportTextures()) {
                auto* vk_texture = ResourceCast(barrier.texture.GetTexture());
                auto  access     = tracker.ReadTexture(vk_texture, barrier.state);
                tracker.QueueTransferReleaseResource(
                    vk_texture,
                    device.GetQueueFamilyIndex(_cmd->src_queue),
                    device.GetQueueFamilyIndex(_cmd->dst_queue),
                    vk_texture->GetQueuePreferredLayout(_cmd->src_queue),
                    std::get<1>(access)
                );
            }

            for (auto& barrier : _cmd->ExportBuffers()) {
                auto* vk_buffer = ResourceCast(barrier.buffer.GetBuffer());
                auto  access    = tracker.ReadBuffer(vk_buffer, barrier.state);
                tracker.QueueTransferReleaseResource(
                    vk_buffer,
                    device.GetQueueFamilyIndex(_cmd->src_queue),
                    device.GetQueueFamilyIndex(_cmd->dst_queue)
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
                arg.stride        = m_device->GetOptionalProperties()
                                        .descriptor_buffer_properties.sampledImageDescriptorSize >>
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
                arg.stride        = m_device->GetOptionalProperties()
                                        .descriptor_buffer_properties.storageBufferDescriptorSize >>
                                    2;

                cmd_list.BindDescriptors(
                    shuffle_sd.handle,
                    shuffle_sd.SetArgs(
                        arg,
                        buffer_indices_buf,
                        buffer_desc_staging,
                        bindless_array->bindless_buffer_descs->GetView()
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
    wait_infos.clear();
    signal_infos.clear();
    return outcome;
}

VulkanOperationResult VkNativeQueue::Submit(
    VulkanCmdList&                _cmdlist,
    const VulkanOperationContext& _context,
    VkFence                       _fence
) {
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

    VkSubmitInfo2   submit_info{};
    VkCommandBuffer cmd = _cmdlist.GetHandle();

    VkCommandBufferSubmitInfo cmd_info{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .pNext = nullptr, .commandBuffer = cmd
    };
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submit_info.pNext                    = nullptr;
    submit_info.waitSemaphoreInfoCount   = wait_infos.size();
    submit_info.pWaitSemaphoreInfos      = wait_infos.data();
    submit_info.signalSemaphoreInfoCount = signal_infos.size();
    submit_info.pSignalSemaphoreInfos    = signal_infos.data();
    submit_info.commandBufferInfoCount   = 1;
    submit_info.pCommandBufferInfos      = &cmd_info;

    if (context.operation == EVulkanFaultOperation::None) {
        context.operation = EVulkanFaultOperation::QueueSubmit;
    }
    const VulkanOperationResult outcome =
        device.SubmitOnQueue(queue, submit_info, _fence, context);
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
    VulkanDevice& _device, const Array<WaitEvent>& _wait_events
) {
    for (const WaitEvent& event : _wait_events) {
        auto* fence = reinterpret_cast<VulkanFence*>(event.timeline_handle);
        if (fence == nullptr || !fence->WaitSubmitted(event.value)) {
            return false;
        }
    }
    return !_device.IsFaulted();
}

static VkResult GetRejectedSubmitResult(VulkanDevice& _device) {
    return _device.IsFaulted() ? _device.GetFirstFaultResult() : VK_ERROR_UNKNOWN;
}

static void MarkSubmissionAccepted(
    VulkanFence* _queue_timeline, uint64 _timeline, const Array<SignalEvent>& _signal_events
) {
    _queue_timeline->MarkSubmitted(_timeline);
    for (const SignalEvent& event : _signal_events) {
        reinterpret_cast<VulkanFence*>(event.timeline_handle)->MarkSubmitted(event.value);
    }
}

static void FailSubmissionSignals(
    VulkanFence* _queue_timeline,
    const Array<SignalEvent>& _signal_events,
    VkResult _result
) {
    if (_queue_timeline != nullptr) {
        _queue_timeline->Fail(_result);
    }
    for (const SignalEvent& event : _signal_events) {
        reinterpret_cast<VulkanFence*>(event.timeline_handle)->Fail(_result);
    }
}

VkCommandQueue::VkCommandQueue(
    VulkanDevice& _device,
    EQueueType    _type,
    bool          _enable_rhi_thread,
    bool          _thread_profile_logging
) :
    CommandQueue(),
    vk_device(_device),
    queue(_type, _device),
    timestamp_pool(
        _device,
        VK_QUERY_TYPE_TIMESTAMP,
        s_queue_max_frame_in_flight * s_query_max_storage * 4
    ),
    profiler_storage(timestamp_pool),
    rhi_thread_enabled(_enable_rhi_thread) {
    if (_thread_profile_logging) {
        thread_profile = MakeUnique<RhiThreadProfileState>();
    }
    timeline = MoerNew(VulkanFence(vk_device));

    completion_worker_running = true;
    completion_thread         = std::jthread(&VkCommandQueue::CompletionThreadMain, this);

    if (rhi_thread_enabled) {
        rhi_worker_running = true;
        rhi_thread         = std::jthread(&VkCommandQueue::RhiThreadMain, this);
    }

    LOG_INFO(
        "[Threading] Vulkan {} queue RHI mode: {}",
        _type == EQueueType::Graphics ? "graphics" : "compute",
        rhi_thread_enabled ? "threaded" : "synchronous"
    );
}

VkCommandQueue::~VkCommandQueue() {
    Sync();

    if (rhi_thread_enabled) {
        {
            std::unique_lock<std::mutex> lock(rhi_work_mutex);
            rhi_worker_running = false;
        }
        rhi_work_cv.notify_all();
        rhi_thread.join();
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

WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
    if (vk_device.IsFaulted()) {
        vk_device.RecordRejectedSubmit();
        FailSubmissionSignals(nullptr, _submit.signal_events, vk_device.GetFirstFaultResult());
        for (auto& callback : _submit.callbacks) {
            callback();
        }
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

void VkCommandQueue::ExecuteNow(CmdSubmit&& _submit, uint64 _timeline, uint64 _serial) {
    assert(
        !rhi_thread_enabled ||
        rhi_thread_id.load(std::memory_order_acquire) == Platform::GetCurrentThreadID()
    );

    const VulkanOperationContext context{
        .operation   = EVulkanFaultOperation::QueueSubmit,
        .queue_type  = queue.GetType(),
        .queue       = queue.GetHandle(),
        .timeline    = _timeline,
        .work_serial = _serial,
    };
    if (vk_device.IsFaulted()) {
        vk_device.RecordRejectedSubmit();
        const VkResult rejected_result = vk_device.GetFirstFaultResult();
        FailSubmissionSignals(timeline, _submit.signal_events, rejected_result);
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(
            VulkanSubmissionEvent{
                {EVulkanOperationStatus::Rejected, rejected_result}, context
            },
            _timeline,
            false
        );
        if (!_submit.callbacks.empty()) {
            event_queue.emplace_back(
                VulkanCallbackBatch{std::move(_submit.callbacks), false}, _timeline, false
            );
        }
        if (!_submit.success_callbacks.empty()) {
            event_queue.emplace_back(
                VulkanCallbackBatch{std::move(_submit.success_callbacks), true}, _timeline, false
            );
        }
        EnqueueCompletionMarker(_timeline);
        lock.unlock();
        queue_cv.notify_one();
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

    Timer         timer{};
    Timer         reorder_timer{};
    FunctionTable function_table{
        .is_resource_write       = &IsBufferTextureWrite,
        .is_resource_read        = &IsBufferTextureRead,
        .is_texture_sampled      = &IsTextureSampled,
        .is_resource_in_bindless = &IsResourceInBindlessArray,
        .lock_bdls_array         = &LockBindlessArray,
        .unlock_bdls_array       = &UnlockBindlessArray
    };

    CmdReorderer reorderer{function_table, _submit.cached_args};
    //reorder commands base on resource read/write and manual scope
    reorder_timer.Start();
    for (const auto& cmd : _submit.cmds) {
        reorderer.AcceptCmd(cmd.get());
    }
    reorder_timer.Stop();
    double reorder_time = reorder_timer.ElapsedMilliseconds();

    //Get Allocators for buffer, texture and commandlist
    auto  allocator_ptr = std::move(GetAllocator());
    auto& vk_allocator  = *allocator_ptr;

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
        _submit.b_tick_profiling
    );

    //Visitor for barrier generation
    VkCmdPreprocessor preprocessor(vk_device, tracker, vk_allocator, function_table, _submit.cached_args);

    // LOG_INFO("Reorderer time {}", timer.ElapsedMilliseconds());
    const auto& cmd_lists = reorderer.m_cmd_lists;
    bool        has_cmd   = !reorderer.m_cmd_lists.empty();
    uint64      descriptor_frame = 0;
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
    if (has_cmd) {
        vk_allocator.GetCmdList().Begin();
        if (_submit.b_tick_profiling) {
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
            timer.Start();
        }

        if (queue.GetType() != EQueueType::Copy) {
            // Present work advances the queue timeline without consuming descriptor storage.
            // Keep ring reuse aligned with the execute-only in-flight limit.
            descriptor_frame = descriptor_submission++;
            vk_device.GetGlobalDescriptorHeap().BeginPushDescriptors(descriptor_frame);
            if (record_sample) {
                descriptor_begin_offset = vk_device.GetGlobalDescriptorHeap().current_offset;
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
                    vk_device.GetGlobalDescriptorHeap().current_offset;
                const auto  command_started = std::chrono::steady_clock::now();
                visitor.VisitCmd(cmd);
                const auto command_finished = std::chrono::steady_clock::now();
                const uint64 descriptor_after =
                    vk_device.GetGlobalDescriptorHeap().current_offset;
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
                if (_submit.b_tick_profiling && cmd->Type() == Command::EType::Scope) {
                    const auto& scope = *static_cast<const ScopeCmd*>(cmd);
                    if (scope.QueryTimestamp()) {
                        serial_golden->RecordQueryEvent(
                            scope.IsPush() ? SerialQueryEvent::Begin : SerialQueryEvent::End,
                            scope.ScopeName(),
                            scope.IsPush() ? ProfilerStorage::kBeginTimestampStage
                                           : ProfilerStorage::kEndTimestampStage
                        );
                    }
                } else if (_submit.b_tick_profiling && cmd->Type() == Command::EType::ShaderDispatch) {
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
        tracker.RestoreState();
        if (record_sample) {
            record_pending_barriers(std::numeric_limits<uint64>::max());
        }
        tracker.DispatchBarriers(vk_allocator.GetCmdList());
        if (_submit.b_tick_profiling) {
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
        vk_allocator.GetCmdList().End();
        if (queue.GetType() != EQueueType::Copy) {
            if (record_sample) {
                record_sample->descriptor_bytes =
                    vk_device.GetGlobalDescriptorHeap().current_offset - descriptor_begin_offset;
            }
            vk_device.GetGlobalDescriptorHeap().EndPushDescriptors(descriptor_frame);
        }
        if (record_sample && _submit.b_tick_profiling) {
            const QueryFrameDiagnostics query_diagnostics =
                profiler_storage.GetCurrentFrameQueryDiagnostics();
            record_sample->query_digest = query_diagnostics.digest;
            record_sample->used_queries = query_diagnostics.used_query_count;
        }
        tracker.Reset();
    }

    if (record_sample) {
        StableRecordHash descriptor_hash;
        descriptor_hash.Add(record_sample->descriptor_bytes);
        record_sample->descriptor_digest = descriptor_hash.Value();
        if (!_submit.b_tick_profiling) {
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

    Array<RHIResource*> deferred_releases = TakeDeferredReleases();

    const auto            end_tag = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VulkanOperationResult submit_outcome{};
    if (!WaitForSubmittedDependencies(vk_device, _submit.wait_events)) {
        vk_device.RecordRejectedSubmit();
        const VkResult rejected_result = GetRejectedSubmitResult(vk_device);
        submit_outcome = {EVulkanOperationStatus::Rejected, rejected_result};
        FailSubmissionSignals(timeline, _submit.signal_events, rejected_result);
    } else {
        queue.Signal(timeline, _timeline, end_tag);
        for (auto& evt : _submit.wait_events) {
            queue.Wait(
                reinterpret_cast<VulkanFence*>(evt.timeline_handle),
                evt.value,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
            );
        }
        for (auto& evt : _submit.signal_events) {
            queue.Signal(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value, end_tag);
        }

        submit_outcome = has_cmd ? queue.Submit(vk_allocator.GetCmdList(), context)
                                 : queue.SubmitEmpty(context);
        if (submit_outcome.WasSubmitted()) {
            MarkSubmissionAccepted(timeline, _timeline, _submit.signal_events);
        } else {
            FailSubmissionSignals(timeline, _submit.signal_events, submit_outcome.result);
        }
    }

    {
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(
            VulkanSubmissionEvent{submit_outcome, context, submit_outcome.WasSubmitted()},
            _timeline,
            false
        );
        event_queue.emplace_back(std::move(allocator_ptr), _timeline, false);
        for (auto& evt : _submit.signal_events) {
            event_queue.emplace_back(SignalEvent(evt.timeline_handle, evt.value), _timeline, false);
        }
        if (!_submit.callbacks.empty()) {
            event_queue.emplace_back(
                VulkanCallbackBatch{std::move(_submit.callbacks), false}, _timeline, false
            );
        }
        if (!_submit.success_callbacks.empty()) {
            event_queue.emplace_back(
                VulkanCallbackBatch{std::move(_submit.success_callbacks), true}, _timeline, false
            );
        }
        if (!deferred_releases.empty()) {
            event_queue.emplace_back(
                VulkanDeferredReleaseBatch{std::move(deferred_releases)}, _timeline, false
            );
        }
        EnqueueCompletionMarker(_timeline);
    }
    queue_cv.notify_one();

    if (has_cmd) {
        executed_queue.Enqueue(_timeline);
    }
    if (has_cmd && _submit.b_tick_profiling) {
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
    }
}

void VkCommandQueue::Present(
    SwapchainRef      _sc,
    TextureView       _view,
    PresentReceiptRef _receipt
) {
    assert(_view.texture != nullptr && "Present source texture is null");
    TextureRef source_texture{_view.texture};

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

void VkCommandQueue::PresentNow(
    SwapchainRef&& _sc,
    TextureRef&&   _source_texture,
    TextureView    _view,
    PresentReceiptRef _receipt,
    uint64         _timeline,
    uint64         _serial
) {
    assert(
        !rhi_thread_enabled ||
        rhi_thread_id.load(std::memory_order_acquire) == Platform::GetCurrentThreadID()
    );

    const VulkanOperationContext context{
        .operation   = EVulkanFaultOperation::PresentSubmit,
        .queue_type  = queue.GetType(),
        .queue       = queue.GetHandle(),
        .timeline    = _timeline,
        .work_serial = _serial,
    };
    if (vk_device.IsFaulted()) {
        vk_device.RecordRejectedPresent();
        if (_receipt) {
            _receipt->Resolve(false);
        }
        const VkResult rejected_result = vk_device.GetFirstFaultResult();
        timeline->Fail(rejected_result);
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(
            VulkanSubmissionEvent{
                {EVulkanOperationStatus::Rejected, rejected_result}, context, false
            },
            _timeline,
            false
        );
        EnqueueCompletionMarker(_timeline);
        lock.unlock();
        queue_cv.notify_one();
        return;
    }

    VkSwapchain* sc = ResourceCast(_sc.Get());
    auto         presentor = std::move(GetPresentor());
    auto& vk_allocator = *presentor;
    auto& vk_cmd_list  = vk_allocator.GetCmdList();
    auto& vk_tracker   = vk_allocator.GetTracker();
    if (!sc->WaitFrameInFlight()) {
        vk_device.RecordRejectedPresent();
        if (_receipt) {
            _receipt->Resolve(false);
        }
        const VkResult rejected_result = GetRejectedSubmitResult(vk_device);
        timeline->Fail(rejected_result);
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(
            VulkanSubmissionEvent{
                {EVulkanOperationStatus::Rejected, rejected_result}, context, false
            },
            _timeline,
            false
        );
        event_queue.emplace_back(std::move(presentor), _timeline, false);
        EnqueueCompletionMarker(_timeline);
        lock.unlock();
        queue_cv.notify_one();
        return;
    }
    auto acquire = sc->AquireNextImage(rhi_thread_enabled ? 0 : 50'000'000);
    bool recreate_swapchain =
        acquire.outcome.status == EVulkanOperationStatus::Recreate;

    Array<std::function<void()>> callbacks;
    if (!acquire.HasImage()) {
        if (_receipt) {
            _receipt->Resolve(false, recreate_swapchain);
        }
        if (acquire.outcome.status == EVulkanOperationStatus::Faulted ||
            acquire.outcome.status == EVulkanOperationStatus::Rejected) {
            timeline->Fail(acquire.outcome.result);
        } else {
            presentors.Push(presentor.release());
        }

        Array<RHIResource*> deferred_releases = TakeDeferredReleases();
        callbacks.emplace_back(
            [swapchain = std::move(_sc), source_texture = std::move(_source_texture)]() mutable {}
        );
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(
                VulkanSubmissionEvent{acquire.outcome, context, false}, _timeline, false
            );
            if (presentor) {
                event_queue.emplace_back(std::move(presentor), _timeline, false);
            }
            event_queue.emplace_back(
                VulkanCallbackBatch{std::move(callbacks), false}, _timeline, false
            );
            if (!deferred_releases.empty()) {
                event_queue.emplace_back(
                    VulkanDeferredReleaseBatch{std::move(deferred_releases)}, _timeline, false
                );
            }
            EnqueueCompletionMarker(_timeline);
        }
        queue_cv.notify_one();
        return;
    }

    //copy
    auto* vk_src_tex     = static_cast<VulkanTexture*>(_view.texture);
    const uint idx       = acquire.image_index;
    auto* swaphchain_tex = ResourceCast(sc->GetSwapchainImage(idx).texture);
    {
        vk_cmd_list.Begin();
        const std::string present_label = std::format("Present: {}", vk_src_tex->GetName());
        vk_cmd_list.BeginLabel(present_label, {0.0f, 0.85f, 0.95f, 1.0f});
        vk_tracker.SetPassType(EPassType::Graphics);
        vk_tracker.RecordState(vk_src_tex, vk_tracker.ReadTexture(vk_src_tex, ETextureState::TRANSFER));
        vk_tracker.RecordState(
            swaphchain_tex, vk_tracker.WriteTexture(swaphchain_tex, ETextureState::TRANSFER)
        );
        vk_tracker.ResolveBarriers();
        vk_tracker.DispatchBarriers(vk_cmd_list);
        //copy
        //todo: need transaction
        vk_cmd_list.InsertLabel("Copy Present Image", GpuMarkerPalette::Transfer());
        vk_cmd_list.CopyTexture(vk_src_tex, swaphchain_tex, _view.extent, {0, 0, 0}, {0, 0, 0}, 0, 0);
        vk_tracker.RecordState(
            swaphchain_tex, VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COPY_BIT
        );
        vk_tracker.ResolveBarriers();
        vk_tracker.DispatchBarriers(vk_cmd_list);

        vk_tracker.RestoreState();
        vk_tracker.DispatchBarriers(vk_cmd_list);
        vk_cmd_list.EndLabel();
        vk_cmd_list.End();
        vk_tracker.Reset();
        // vk_tracker.PropagateState();
    }

    Array<RHIResource*> deferred_releases = TakeDeferredReleases();
    callbacks.emplace_back(
        [swapchain = std::move(_sc), source_texture = std::move(_source_texture)]() mutable {}
    );

    queue.Signal(timeline, _timeline, VK_PIPELINE_STAGE_2_COPY_BIT);
    queue.Wait(acquire.ready_semaphore, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    queue.Signal(sc->GetRenderFinishedFence(idx), VK_PIPELINE_STAGE_2_COPY_BIT);
    VulkanOperationResult submit_outcome = queue.Submit(vk_allocator.GetCmdList(), context);
    if (submit_outcome.WasSubmitted()) {
        timeline->MarkSubmitted(_timeline);
    } else {
        timeline->Fail(submit_outcome.result);
    }
    VulkanOperationResult final_outcome = submit_outcome;
    bool                  present_accepted = false;
    if (submit_outcome.WasSubmitted()) {
        const VulkanOperationResult present_outcome =
            sc->Present(queue.GetHandle(), idx, _timeline, _serial);
        present_accepted =
            present_outcome.result == VK_SUCCESS || present_outcome.result == VK_SUBOPTIMAL_KHR;
        recreate_swapchain =
            recreate_swapchain ||
            present_outcome.status == EVulkanOperationStatus::Recreate;
        if (!present_outcome.Succeeded()) {
            final_outcome = present_outcome;
        }
    }
    if (_receipt) {
        _receipt->Resolve(present_accepted, recreate_swapchain);
    }
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(
            VulkanSubmissionEvent{final_outcome, context, submit_outcome.WasSubmitted()},
            _timeline,
            false
        );
        event_queue.emplace_back(std::move(presentor), _timeline, false);
        event_queue.emplace_back(
            VulkanCallbackBatch{std::move(callbacks), false}, _timeline, false
        );
        if (!deferred_releases.empty()) {
            event_queue.emplace_back(
                VulkanDeferredReleaseBatch{std::move(deferred_releases)}, _timeline, false
            );
        }
        EnqueueCompletionMarker(_timeline);
    }
    if (submit_outcome.WasSubmitted()) {
        presented_queue.Enqueue(_timeline);
    }
    queue_cv.notify_one();
}

void VkCommandQueue::Sync() {
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

    Complete(target_timeline);

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
    if (executed_queue.Full()) {
        Complete(executed_queue.Front());
    }
    auto allocator = std::move(UniquePtr<VulkanAllocator>(allocators.Pop()));
    if (allocator) {
        // allocator->ResetCmdList();
        return std::move(allocator);
    }
    return MakeUnique<VulkanAllocator>(&vk_device, queue.GetType());
}

UniquePtr<VulkanPresentor> VkCommandQueue::GetPresentor() {
    if (presented_queue.Full()) {
        Complete(presented_queue.Front());
    }
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
    event_queue.emplace_back(VulkanCompletionMarker{}, _timeline, true);
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
                        if (_submission.outcome.status == EVulkanOperationStatus::Faulted ||
                            _submission.outcome.status == EVulkanOperationStatus::Rejected) {
                            timeline->Fail(_submission.outcome.result);
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
                [this, &batch_gpu_success](UniquePtr<VulkanAllocator>& _allocator) {
                    if (batch_gpu_success && !vk_device.IsFaulted()) {
                        _allocator->CompleteSuccess();
                        if (_allocator->Reset()) {
                            allocators.Push(_allocator.release());
                            return;
                        }
                    }
                    vk_device.RecordAllocatorQuarantine();
                    vk_device.RecordSkippedCommandPoolReset();
                    allocator_quarantine.emplace_back(std::move(_allocator));
                },
                [this, &batch_gpu_success](UniquePtr<VulkanPresentor>& _presentor) {
                    if (batch_gpu_success && !vk_device.IsFaulted()) {
                        _presentor->CompleteSuccess();
                        if (_presentor->Reset()) {
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
                        for (auto& func : _batch.callbacks) {
                            func();
                        }
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
            queue_cv.notify_all();
        }
    }
}

void VkCommandQueue::Complete(uint64 _timeline) {
    if (_timeline == 0) {
        return;
    }

    std::unique_lock<std::mutex> lock(event_mutex);
    queue_cv.wait(lock, [this, _timeline]() {
        return cpu_settled_frame.load(std::memory_order_acquire) >= _timeline;
    });
}

#pragma endregion

#pragma region[ copy queue ]

VkCopyQueue::VkCopyQueue(VulkanDevice& _device) :
    CopyQueue(),
    device(_device),
    queue(EQueueType::Copy, _device) {
    timeline = MoerNew(VulkanFence)(_device);
    enabled  = true;
    thread   = std::jthread([this]() {
        ExecuteThread();
    });
}

VkCopyQueue::~VkCopyQueue() {
    Complete(last_frame);
    enabled = false;
    queue_cv.notify_all();
    thread.join();
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
    Array<UniquePtr<Command>> cmds = std::move(_evt.cmds);

    auto current_timeline = last_frame;
    if (device.IsFaulted()) {
        device.RecordRejectedSubmit();
        FailSubmissionSignals(nullptr, _evt.signal_events, device.GetFirstFaultResult());
        for (auto& callback : _evt.callbacks) {
            callback();
        }
        return {uint64(timeline.Get()), current_timeline};
    }
    const bool has_queue_work =
        !cmds.empty() || !_evt.wait_events.empty() || !_evt.signal_events.empty() ||
        !_evt.callbacks.empty() || !_evt.success_callbacks.empty();
    if (has_queue_work) {

        FunctionTable function_table{
            .is_resource_write       = &IsBufferTextureWrite,
            .is_resource_read        = &IsBufferTextureRead,
            .is_texture_sampled      = &IsTextureSampled,
            .is_resource_in_bindless = &IsResourceInBindlessArray
        };
        CmdReorderer reorderer{function_table, _evt.cached_args};

        std::unique_lock<std::mutex> lk(exec_mutex);
        auto                         allocator    = GetAllocator();
        auto&                        vk_allocator = *allocator;
        auto&                        vk_cmd_list  = vk_allocator.GetCmdList();
        auto&                        vk_tracker   = vk_allocator.GetTracker();

        VkCmdPreprocessor preprocessor(
            device, vk_tracker, vk_allocator, {}, _evt.cached_args, EQueueType::Copy
        );
        VkCmdVisitor visitor(device, vk_allocator, vk_tracker, vk_cmd_list, _evt.cached_args);

        for (const auto& cmd : cmds) {
            // preprocessor.VisitCmd(cmd.get());
            reorderer.AcceptCmd(cmd.get());
        }

        vk_cmd_list.Begin();
        vk_cmd_list.BeginLabel("Copy", {0.0f, 1.0f, 1.0f, 1.0f});
        const auto& cmd_lists = reorderer.m_cmd_lists;

        for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
            if (cmd_list.head == nullptr) {
                continue;
            }
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                preprocessor.VisitCmd(cmdnode->cmd);
            }
            vk_tracker.ResolveBarriers();
            vk_tracker.DispatchBarriers(vk_cmd_list);
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                const auto* cmd = cmdnode->cmd;
                visitor.VisitCmd(cmd);
            }
        }

        // vk_tracker.ResolveBarriers();
        // vk_tracker.DispatchBarriers(vk_cmd_list);
        // for (const auto& cmd : cmds) {
        //     visitor.VisitCmd(cmd.get());
        // }
        //copy
        vk_tracker.RestoreState();
        vk_tracker.DispatchBarriers(vk_cmd_list);
        vk_cmd_list.EndLabel();
        vk_cmd_list.End();
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
        VulkanOperationResult submit_outcome{};
        if (!WaitForSubmittedDependencies(device, _evt.wait_events)) {
            device.RecordRejectedSubmit();
            const VkResult rejected_result = GetRejectedSubmitResult(device);
            submit_outcome = {EVulkanOperationStatus::Rejected, rejected_result};
            FailSubmissionSignals(timeline.Get(), _evt.signal_events, rejected_result);
        } else {
            queue.Signal(timeline, current_timeline, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            if (current_timeline > 1) {
                queue.Wait(timeline, current_timeline - 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            }
            for (auto& evt : _evt.wait_events) {
                queue.Wait(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value);
            }
            for (auto& evt : _evt.signal_events) {
                queue.Signal(
                    reinterpret_cast<VulkanFence*>(evt.timeline_handle),
                    evt.value,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
                );
            }
            submit_outcome = queue.Submit(vk_allocator.GetCmdList(), context);
            if (submit_outcome.WasSubmitted()) {
                MarkSubmissionAccepted(timeline.Get(), current_timeline, _evt.signal_events);
            } else {
                FailSubmissionSignals(timeline.Get(), _evt.signal_events, submit_outcome.result);
            }
        }
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(
                VulkanSubmissionEvent{submit_outcome, context, submit_outcome.WasSubmitted()},
                current_timeline,
                false
            );
            event_queue.emplace_back(std::move(allocator), current_timeline, false);
            for (auto& evt : _evt.signal_events) {
                event_queue.emplace_back(
                    IOSignalEvt(evt.timeline_handle, evt.value), current_timeline, false
                );
            }
            if (!_evt.callbacks.empty()) {
                event_queue.emplace_back(
                    VulkanCallbackBatch{std::move(_evt.callbacks), false}, current_timeline, false
                );
            }
            if (!_evt.success_callbacks.empty()) {
                event_queue.emplace_back(
                    VulkanCallbackBatch{std::move(_evt.success_callbacks), true}, current_timeline, false
                );
            }
            event_queue.emplace_back(VulkanCompletionMarker{}, current_timeline, true);
            queue_cv.notify_one();
        }
        return {uint64(timeline.Get()), current_timeline};
    }
    return {uint64(timeline.Get()), current_timeline};
}

void VkCopyQueue::Sync(uint64 _timeline) {
    Complete(_timeline);
}

FenceRef VkCopyQueue::GetFenceHandle() {
    return FenceRef(timeline);
}

void VkCopyQueue::ExecuteThread() {
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
                        if (_submission.outcome.status == EVulkanOperationStatus::Faulted ||
                            _submission.outcome.status == EVulkanOperationStatus::Rejected) {
                            timeline->Fail(_submission.outcome.result);
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
                [this, &batch_gpu_success](UniquePtr<VulkanAllocator>& _allocator) {
                    if (batch_gpu_success && !device.IsFaulted()) {
                        _allocator->CompleteSuccess();
                        if (_allocator->Reset()) {
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
                        for (auto& callback : _batch.callbacks) {
                            callback();
                        }
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
        vk_cmd_list.Begin();
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
        vk_tracker.RestoreState();
        vk_tracker.DispatchBarriers(vk_cmd_list);
        vk_cmd_list.EndLabel();
        vk_cmd_list.End();
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
            event_queue.emplace_back(VulkanCompletionMarker{}, current_timeline, true);
            queue_cv.notify_one();
        }
        {
            std::unique_lock<std::mutex> io_lock(rhi_mutex);
            io_rhi_cmdlists.pop();
        }
    }
}

UniquePtr<VulkanAllocator> VkCopyQueue::GetAllocator() {
    if (last_frame >= device.cmd_alloc_limits) {
        Complete(last_frame);
    }
    auto allocator = std::move(UniquePtr<VulkanAllocator>(allocators.Pop()));
    if (allocator) {
        // allocator->ResetCmdList();
        return std::move(allocator);
    }
    return MakeUnique<VulkanAllocator>(&device, EQueueType::Copy);
}

void VkCopyQueue::Complete(uint64 _timeline) {
    if (_timeline == 0) {
        return;
    }
    std::unique_lock<std::mutex> lock(event_mutex);
    settled_cv.wait(lock, [this, _timeline]() {
        return cpu_settled_frame.load(std::memory_order_acquire) >= _timeline;
    });
}
#pragma endregion
} // namespace Moer::Render
