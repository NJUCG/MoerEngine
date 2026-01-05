#include "VulkanQueue.h"
#include "RHICmdReorderer.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanRHIResource.h"
#include "misc/Alignment.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "misc/Traits.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIIO.h"
#include "rhi/RHIResource.h"

#include "VulkanCustomCommand.h"
#include "shader/ShaderPipeline.h"
#include "vulkan/vulkan_core.h"

#include <memory>
#include <mutex>
#include <thread>
#include <variant>
namespace Moer::Render {

#pragma region[ utils ]

VkRenderingAttachmentInfo FromColorAttachmentInfo(const ColorAttachment& _attachment) {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
    attachment_info.imageView = vk_texture->GetView();

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

VkRenderingAttachmentInfo FromDepthAttachmentInfo(const DepthAttachment& _attachment) {
    VkRenderingAttachmentInfo attachment_info{};
    attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    attachment_info.pNext = nullptr;

    VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
    attachment_info.imageView = vk_texture->GetView(_attachment.mip_level, 1, _attachment.array_layer, 1);

    attachment_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
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

    UnorderedSet<uint64>   writed_resources;
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

        Moer::Array<VulkanTexture*> to_read_textures;
        for (const auto& i : tracker.GetWritedStateTextures()) {
            if (vk_bindless_array->IsResourceAllocated(uint64(i)) && !writed_resources.contains(uint64(i))) {
                to_read_textures.push_back(i);
            }
        }
        if (!to_read_textures.empty()) {
            for (const auto& i : to_read_textures) {
                tracker.RecordState(i, tracker.ReadTexture(i, ETextureState::SAMPLE, pass_type));
            }
        }
        Moer::Array<VulkanBuffer*> to_read_buffers;
        for (const auto& i : tracker.GetWritedStateBuffers()) {
            if (vk_bindless_array->IsResourceAllocated(uint64(i)) && !writed_resources.contains(uint64(i))) {
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
                        writed_resources.insert(uint64(vk_buffer));
                    }
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    if (_flag.resource_type == SRT_INVALID)
                        return;
                    auto* vk_texture = reinterpret_cast<VulkanTexture*>(_arg.GetTexture());
                    tracker.RecordState(
                        vk_texture,
                        GetTextureAccess(_flag),
                        GetTextureLayout(_flag),
                        _pipelines,
                        _arg.mip_level,
                        _arg.num_mips
                    );
                    if (IsBufferTextureWrite(_flag)) {
                        writed_resources.insert(uint64(vk_texture));
                    }
                } else if constexpr (std::is_same_v<T, std::span<TextureView>>) {
                    for (auto&& i : _arg) {
                        VisitArgs(i, _flag, _pipelines);
                    }
                } else if constexpr (std::is_same_v<T, std::span<BufferView>>) {
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
            _cmd->MipLevel()
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
            _cmd->MipLevel()
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

        writed_resources.clear();

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
                tracker.RecordState(
                    vk_texture, tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type)
                );
            }
            for (auto& barrier : _cmd->WriteTextures()) {
                auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                tracker.RecordState(
                    vk_texture, tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type)
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
            tracker.RecordState(
                vk_texture,
                tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type),
                src_queue_family,
                dst_queue_family
            );
        }
        for (auto& barrier : _cmd->WriteTextures()) {
            auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
            tracker.RecordState(
                vk_texture,
                tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type),
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

        writed_resources.clear();

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
                0,
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
                0,
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

        writed_resources.clear();

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
                0,
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
                0,
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
                        _arg.num_mips
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

public:
    VkCmdVisitor(
        VulkanDevice&          _device,
        VulkanAllocator&       _allocator,
        VkTracker&             _tracker,
        VulkanCmdList&         _cmd_list,
        const TCachedArgArray& _cached_args,
        ProfilerStorage*       _profiler = nullptr
    ) :
        VulkanDeviceObject(&_device),
        allocator(_allocator),
        tracker(_tracker),
        cmd_list(_cmd_list),
        cached_args(_cached_args),
        profiler(_profiler) {}

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
        }
    };
    void Visit(const UploadBufferCmd& _cmd) {
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
        auto           tmp_buffer = _cmd.staging_buffer;
        VulkanTexture* texture    = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
        cmd_list.CopyBufferToTexture(
            reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
            texture,
            tmp_buffer.GetByteSize(),
            tmp_buffer.GetByteOffset(),
            _cmd.Offset(),
            _cmd.Size(),
            _cmd.MipLevel(),
            _cmd.ArrayLayer()
        );
    }

    void Visit(const CopyBufferCmd& _cmd) {
        VulkanBuffer* src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
        VulkanBuffer* dst_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

        cmd_list.CopyBuffer(src_buffer, dst_buffer, _cmd.ByteSize(), _cmd.SrcOffset(), _cmd.DstOffset());
    }

    void Visit(const CopyBackBufferCmd& _cmd) {
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

        PipelineHandle& pso = _cmd.Pipeline();
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
        PipelineHandle& pso  = _cmd.Pipeline();

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
        if (pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
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
#if WITH_CUDA
            .pStencilAttachment = nullptr
#else
            .pStencilAttachment =
                depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr
#endif
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
        if (pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
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
#if WITH_CUDA
            .pStencilAttachment = nullptr
#else
            .pStencilAttachment =
                depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr
#endif
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
        std::visit(
            Overload{
                [&](const TextureView& _arg) {
                    auto*                   vk_texture = ResourceCast(_arg.GetTexture());
                    VkImageSubresourceRange range{
                        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                        .baseMipLevel   = _arg.mip_level,
                        .levelCount     = _arg.num_mips,
                        .baseArrayLayer = 0,
                        .layerCount     = 1
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
            cmd_list.BeginLabel(_cmd.ScopeName(), {0.0f, 1.0f, 0.0f, 1.0f});
            if (_cmd.QueryTimestamp()) {
                assert(profiler && "profiler is not set");
                profiler->BeginProfilerSession(cmd_list, _cmd.ScopeName());
            }
        } else {
            cmd_list.EndLabel();
            if (_cmd.QueryTimestamp()) {
                assert(profiler && "profiler is not set");
                profiler->EndProfilerSession(cmd_list, _cmd.ScopeName());
            }
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

            cmd_list.BindDescriptors(
                shuffle_sd.handle, shuffle_sd.SetArgs(arg, indices, staging, instance_buffer->GetView())
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
        geometry.geometry.instances.arrayOfPointers    = VK_FALSE;
        geometry.geometry.instances.data.deviceAddress = instance_buffer->DeviceAddress();

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

VkNativeQueue::VkNativeQueue(EQueueType _type, VulkanDevice& _device) : type(_type) {
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

void VkNativeQueue::SubmitEmpty(VkFence _fence) {
    VkSubmitInfo2 submit_info{};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;

    submit_info.pNext                    = nullptr;
    submit_info.waitSemaphoreInfoCount   = wait_infos.size();
    submit_info.pWaitSemaphoreInfos      = wait_infos.data();
    submit_info.signalSemaphoreInfoCount = signal_infos.size();
    submit_info.pSignalSemaphoreInfos    = signal_infos.data();
    submit_info.commandBufferInfoCount   = 0;
    submit_info.pCommandBufferInfos      = VK_NULL_HANDLE;
    vkQueueSubmit2(queue, 1, &submit_info, _fence);
    wait_infos.clear();
    signal_infos.clear();
}

void VkNativeQueue::Submit(VulkanCmdList& _cmdlist, VkFence _fence) {
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
    vkQueueSubmit2(queue, 1, &submit_info, _fence);
    wait_infos.clear();
    signal_infos.clear();
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
    VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    label.pLabelName = _label.data();
    label.color[0]   = _color.x;
    label.color[1]   = _color.y;
    label.color[2]   = _color.z;
    label.color[3]   = _color.w;
    vkQueueBeginDebugUtilsLabelEXT(queue, &label);
}

void VkNativeQueue::EndLabel() {
    vkQueueEndDebugUtilsLabelEXT(queue);
}

void VkNativeQueue::InsertLabel(std::string_view _label, float4 _color) {
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
    if (name2sample.find(_name.data()) == name2sample.end()) {
        name2sample[_name.data()] = Sample(name2sample.size());
    }
    return name2sample[_name.data()].index;
}

void ProfilerStorage::BeginProfilerSession(VulkanCmdList& _cmd_list, std::string_view _name) {
    if (!active) {
        return;
    }
    uint idx = GetQueryStorageIndex(_name) * 2 + 0 + cur_frame * s_max_num_profiler_queries_per_frame;
    vkCmdWriteTimestamp(
        _cmd_list.GetHandle(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamp_pool.GetHandle(), idx
    );
    SetQueryUsed(idx);
    assert(IsQueryUsed(idx + 1) == false && "Query already used");
}

void ProfilerStorage::EndProfilerSession(VulkanCmdList& _cmd_list, std::string_view _name) {
    if (!active) {
        return;
    }
    uint idx = GetQueryStorageIndex(_name) * 2 + 1 + cur_frame * s_max_num_profiler_queries_per_frame;
    vkCmdWriteTimestamp(
        _cmd_list.GetHandle(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool.GetHandle(), idx
    );
    SetQueryUsed(idx);
    assert(IsQueryUsed(idx - 1) == true && "Query not used");
}
#pragma endregion

#pragma region[ VkCommandQueue ]
WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
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
    double                       reorder_time = reorder_timer.ElapsedMilliseconds();
    std::unique_lock<std::mutex> lock(exec_mtx); //currently only one thread can execute commands at a time

    //Get Allocators for buffer, texture and commandlist
    auto  allocator_ptr = std::move(GetAllocator());
    auto& vk_allocator  = *allocator_ptr;

    //Get Resource State Tracker
    auto& tracker = vk_allocator.GetTracker();

    //Visitor for actual command recording
    VkCmdVisitor visitor(
        vk_device, vk_allocator, tracker, vk_allocator.GetCmdList(), _submit.cached_args, &profiler_storage
    );

    //Visitor for barrier generation
    VkCmdPreprocessor preprocessor(vk_device, tracker, vk_allocator, function_table, _submit.cached_args);

    // LOG_INFO("Reorderer time {}", timer.ElapsedMilliseconds());
    const auto& cmd_lists = reorderer.m_cmd_lists;
    bool        has_cmd   = !reorderer.m_cmd_lists.empty();
    uint64      last_time = last_frame;

    //Set Descriptor buffer ringbuffer offset and start debug region
    double preprocess_time = 0.0;
    if (has_cmd) {
        vk_allocator.GetCmdList().Begin();
        if (_submit.b_tick_profiling) {
            profiler_storage.CollectProfiling(vk_allocator.GetCmdList().GetHandle());
            cached_profiler_entry = profiler_storage.GetProfilerEntry();
            profiler_storage.BeginProfilerSession(vk_allocator.GetCmdList(), "Graphics Exec");
            timer.Start();
        }

        if (queue.GetType() != EQueueType::Copy) {
            vk_device.GetGlobalDescriptorHeap().BeginPushDescriptors(last_time + 1);
        }

        std::string_view queue_label = queue.GetType() == EQueueType::Graphics ? "Graphics Exec" :
                                       queue.GetType() == EQueueType::Compute  ? "Compute Exec" :
                                                                                 "Copy Exec";
        vk_allocator.GetCmdList().BeginLabel(queue_label, {1.0f, 0.0f, 0.0f, 1.0f});
    }

    uint layer = 0;

    for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
        if (layer == 0) {
            vk_allocator.GetCmdList().BeginLabel("Begin Layers", {0.0f, 0.0f, 1.0f, 1.0f});
        }
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
        vk_allocator.GetCmdList().InsertLabel(std::format("Layer {}", layer++), {0.0f, 0.0f, 1.0f, 1.0f});
        for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
            const auto* cmd = cmdnode->cmd;
            visitor.VisitCmd(cmd);
        }
    }
    if (layer > 0) {
        vk_allocator.GetCmdList().EndLabel();
    }

    if (has_cmd) {
        tracker.RestoreState();
        tracker.DispatchBarriers(vk_allocator.GetCmdList());
        if (_submit.b_tick_profiling) {
            profiler_storage.EndProfilerSession(vk_allocator.GetCmdList(), "Graphics Exec");
        }
        vk_allocator.GetCmdList().EndLabel();
        vk_allocator.GetCmdList().End();
        if (queue.GetType() != EQueueType::Copy) {
            vk_device.GetGlobalDescriptorHeap().EndPushDescriptors(last_time + 1);
        }
        tracker.Reset();
    }

    if (has_cmd) {
        Array<RHIResource*> deleted_resources;
        vk_device.deferred_release_queue.PopAll(deleted_resources);
        _submit.callbacks.emplace_back([deleted_resources(std::move(deleted_resources))]() {
            for (auto* resource : deleted_resources) {
                MoerDelete(resource);
            }
        });
    }

    if (_submit.cmds.empty()) {
        allocators.Push(allocator_ptr.release());
        std::unique_lock<std::mutex> lock(event_mutex);
        bool                         b_wake_up = false;

        b_wake_up = _submit.callbacks.size() != 0 || _submit.wait_events.size() != 0 ||
                    _submit.signal_events.size() != 0;
        if (b_wake_up) {
            auto end_tag = queue.GetType() == EQueueType::Graphics ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT :
                                                                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            if (_submit.callbacks.size() > 0) {
                event_queue.emplace_back(std::move(_submit.callbacks), last_time, true);
            }
            for (auto& evt : _submit.signal_events) {
                event_queue.emplace_back(WaitEvent(evt.timeline_handle, evt.value), last_frame, false);
                event_queue.emplace_back(SignalEvent(evt.timeline_handle, evt.value), last_time, false);
            }
            queue.SubmitEmpty();
        }

        if (b_wake_up) {
            queue_cv.notify_one();
        }
        return {uint64(timeline), last_time};
    } else {
        auto current_timeline = ++last_frame;

        auto end_tag = queue.GetType() == EQueueType::Graphics ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT :
                                                                 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        queue.Signal(timeline, current_timeline, end_tag);
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
        queue.Submit(vk_allocator.GetCmdList());

        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(std::move(allocator_ptr), current_timeline, true);
        for (auto& evt : _submit.signal_events) {
            event_queue.emplace_back(SignalEvent(evt.timeline_handle, evt.value), current_timeline, false);
        }
        if (_submit.callbacks.size() > 0) {
            event_queue.emplace_back(std::move(_submit.callbacks), current_timeline, false);
        }
        queue_cv.notify_one();
        executed_queue.Enqueue(current_timeline);
        if (_submit.b_tick_profiling) {
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
        // LOG_INFO("Submit time {}", timer.ElapsedMilliseconds());
        return {uint64(timeline), current_timeline};
    }
    return {uint64(timeline), 0ull};
}

void VkCommandQueue::Present(SwapchainRef _sc, TextureView _view) {
    VkSwapchain* sc = ResourceCast(_sc.Get());
    // auto         allocator    = std::move(GetAllocator());
    std::unique_lock<std::mutex> lock(exec_mtx); //currently only one thread can execute commands at a time
    auto                         presentor = std::move(GetPresentor());
    // auto& vk_allocator = *allocator;
    auto& vk_allocator = *presentor;
    auto& vk_cmd_list  = vk_allocator.GetCmdList();
    auto& vk_tracker   = vk_allocator.GetTracker();
    sc->WaitFrameInFlight();
    auto [fence, idx, present_timeline] = sc->AquireNextImage();
    if (idx == UINT32_MAX) {
        //present null
        presentor->Reset();
        presentors.Push(presentor.release());
        return;
    }
    //copy
    auto* vk_src_tex     = static_cast<VulkanTexture*>(_view.texture);
    auto* swaphchain_tex = ResourceCast(sc->GetSwapchainImage(idx).texture);
    {
        vk_cmd_list.Begin();
        vk_cmd_list.BeginLabel("Present", {0.0f, 1.0f, 1.0f, 1.0f});
        vk_tracker.SetPassType(EPassType::Graphics);
        vk_tracker.RecordState(vk_src_tex, vk_tracker.ReadTexture(vk_src_tex, ETextureState::TRANSFER));
        vk_tracker.RecordState(
            swaphchain_tex, vk_tracker.WriteTexture(swaphchain_tex, ETextureState::TRANSFER)
        );
        vk_tracker.ResolveBarriers();
        vk_tracker.DispatchBarriers(vk_cmd_list);
        //copy
        //todo: need transaction
        vk_cmd_list.InsertLabel("Copy Present Image", {0.0f, 0.0f, 0.0f, 1.0f});
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

    auto current_timeline = ++last_frame;
    queue.Signal(timeline, current_timeline, VK_PIPELINE_STAGE_2_COPY_BIT);
    queue.Wait(sc->GetImageReadyFence(idx), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    queue.Signal(sc->GetRenderFinishedFence(), VK_PIPELINE_STAGE_2_COPY_BIT);
    queue.Submit(vk_allocator.GetCmdList(), sc->GetInFlightFence(present_timeline));
    sc->Present(queue.GetHandle(), idx);
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.emplace_back(std::move(presentor), current_timeline, true);
        queue_cv.notify_one();
        presented_queue.Enqueue(current_timeline);
    }
}

void VkCommandQueue::Sync() {
    Complete(last_frame);
}

ProfileData VkCommandQueue::GetProfilerEntry() {
    return profiler_storage.GetProfilerEntry();
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

void VkCommandQueue::ExecuteThread() {
    while (enabled) {
        uint64 timeline;
        bool   b_wake_up = false;

        auto wait_util_reach_timeline = [&timeline, &b_wake_up, this]() {
            if (!b_wake_up) {
                return;
            }
            uint64 prev_timeline = executed_frame;
            while (prev_timeline < timeline &&
                   !executed_frame.compare_exchange_weak(prev_timeline, timeline)) {
                std::this_thread::yield();
            }
        };

        auto visit_allocator = [&,
                                &allocators(this->allocators),
                                fence(this->timeline)](UniquePtr<VulkanAllocator>& _allocator) {
            _allocator->Complete(fence, timeline);
            // LOG_INFO("timeline {} complete", timeline);
            _allocator->Reset();
            allocators.Push(_allocator.release());
            wait_util_reach_timeline();
        };
        auto visit_presentor = [&,
                                &presentors(this->presentors),
                                fence(this->timeline)](UniquePtr<VulkanPresentor>& _presentor) {
            _presentor->Complete(fence, timeline);
            _presentor->Reset();
            presentors.Push(_presentor.release());
            wait_util_reach_timeline();
        };
        auto visit_fence = [&](FencePlaceHoler& _fence) {
            this->timeline->HostWait(timeline);
            wait_util_reach_timeline();
        };
        auto visit_funcs = [&](Array<std::function<void()>>& _funcs) {
            for (auto& func : _funcs) {
                func();
            }
            wait_util_reach_timeline();
        };

        auto visit_signal_event = [&](SignalEvent& _evt) {
            auto* fence = reinterpret_cast<VulkanFence*>(_evt.timeline_handle);
            fence->Notify(_evt.value);
        };
        auto visit_wait_event = [&](WaitEvent& _evt) {
            auto* fence = reinterpret_cast<VulkanFence*>(_evt.timeline_handle);
            fence->Wait(_evt.value);
        };
        while (true) {
            std::optional<QueueEvent> evt;
            {
                std::unique_lock<std::mutex> lock(event_mutex);
                if (!event_queue.empty()) {
                    auto& event = event_queue.front();
                    evt.emplace(std::move(event));
                    event_queue.pop_front();
                }
            }
            if (!evt.has_value()) {
                break;
            }
            timeline  = evt->timeline;
            b_wake_up = evt->wake_thread;

            std::visit(
                Overload{
                    [&](UniquePtr<VulkanAllocator>& _allocator) {
                        visit_allocator(_allocator);
                    },
                    [&](UniquePtr<VulkanPresentor>& _presentor) {
                        visit_presentor(_presentor);
                    },
                    [&](FencePlaceHoler& _fence) {
                        visit_fence(_fence);
                    },
                    [&](Array<std::function<void()>>& _funcs) {
                        visit_funcs(_funcs);
                    },
                    [&](SignalEvent& _evt) {
                        visit_signal_event(_evt);
                    },
                    [&](WaitEvent& _evt) {
                        // visit_wait_event(_evt);
                    },
                    [&](VulkanFence* _fence) {
                        assert(false && "Invalid event");
                    }
                },

                evt->event
            );
        }
        {
            //wait for queue submission
            std::unique_lock<std::mutex> lock(event_mutex);
            while (enabled && event_queue.empty()) {
                queue_cv.wait(lock);
            }
        }
    }
}

void VkCommandQueue::Complete(uint64 _timeline) {
    while (executed_frame < _timeline) {
        std::this_thread::yield();
    }
    // vk_device.FlushDeferredReleases();
}

void VkCommandQueue::Signal() {
    auto current_timeline = ++last_frame;
    queue.Signal(timeline, current_timeline);
    {
        std::unique_lock<std::mutex> lock(event_mutex);
        event_queue.push_back({FencePlaceHoler{}, current_timeline, true});
        queue_cv.notify_one();
    }
}

#pragma endregion

#pragma region[ copy queue ]

VkCopyQueue::VkCopyQueue(VulkanDevice& _device) :
    CopyQueue(),
    device(_device),
    queue(EQueueType::Copy, _device) {
    timeline = MoerNew(VulkanFence)(_device);
    thread   = std::jthread([this]() {
        ExecuteThread();
    });
    enabled  = true;
}

VkCopyQueue::~VkCopyQueue() {
    enabled = false;
    queue_cv.notify_all();
    thread.join();
    //clear allocators
    Array<VulkanAllocator*> allocs;
    allocators.PopAll(allocs);
    for (auto& allocator : allocs) {
        MoerDelete(allocator);
    }
    timeline = nullptr;
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

IOWaitEvt VkCopyQueue::Execute(CmdSubmit&& _evt) {
    Array<UniquePtr<Command>> cmds = std::move(_evt.cmds);

    auto current_timeline = last_frame;
    if (!cmds.empty()) {

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
        queue.Submit(vk_allocator.GetCmdList());
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(std::move(allocator), current_timeline, true);
            for (auto& evt : _evt.signal_events) {
                event_queue.emplace_back(
                    IOSignalEvt(evt.timeline_handle, evt.value), current_timeline, false
                );
            }
            event_queue.emplace_back(std::move(_evt.callbacks), current_timeline, false);
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
    while (enabled) {
        uint64 timeline;
        bool   b_wake_up = false;

        auto wait_util_reach_timeline = [&timeline, &b_wake_up, this]() {
            if (!b_wake_up) {
                return;
            }
            uint64 prev_timeline = executed_frame;
            while (prev_timeline < timeline &&
                   !executed_frame.compare_exchange_weak(prev_timeline, timeline)) {
                std::this_thread::yield();
            }
        };

        auto visit_allocator = [&,
                                &allocators(this->allocators),
                                fence(this->timeline)](UniquePtr<VulkanAllocator>& _allocator) {
            _allocator->Complete(fence, timeline);
            // LOG_INFO("timeline {} complete", timeline);
            _allocator->Reset();
            allocators.Push(_allocator.release());
            wait_util_reach_timeline();
        };
        auto visit_fence = [&](Placeholder& _fence) {
            this->timeline->HostWait(timeline);
            wait_util_reach_timeline();
        };
        auto visit_funcs = [&](Array<std::function<void()>>& _funcs) {
            for (auto& func : _funcs) {
                func();
            }
            wait_util_reach_timeline();
        };

        auto visit_signal_event = [&](IOSignalEvt& _evt) {
            auto* fence = reinterpret_cast<VulkanFence*>(_evt.handle);
            fence->Notify(_evt.timeline);
        };
        auto visit_wait_event = [&](IOWaitEvt& _evt) {
            auto* fence = reinterpret_cast<VulkanFence*>(_evt.handle);
            fence->HostWait(_evt.timeline);
        };
        while (true) {
            std::optional<IOEvent> evt;
            {
                std::unique_lock<std::mutex> lock(event_mutex);
                if (!event_queue.empty()) {
                    auto& event = event_queue.front();
                    evt.emplace(std::move(event));
                    event_queue.pop_front();
                }
            }
            if (!evt.has_value()) {
                break;
            }
            timeline  = evt->timeline;
            b_wake_up = evt->wake_thread;
            std::visit(
                Overload{
                    [&](UniquePtr<VulkanAllocator>& _evt) {
                        visit_allocator(_evt);
                    },
                    [&](Placeholder& _evt) {
                        visit_fence(_evt);
                    },
                    [&](Array<std::function<void()>>& _evt) {
                        visit_funcs(_evt);
                    },
                    [&](IOSignalEvt& _evt) {
                        visit_signal_event(_evt);
                    },
                    [&](IOWaitEvt& _evt) {
                        // visit_wait_event(_evt);
                    },
                    [&](VulkanFence* _fence) {
                        assert(false && "Invalid event");
                    },
                    [&](UniquePtr<VulkanPresentor>& _evt) {
                        assert(false && "Invalid event");
                    }

                },
                evt->event
            );
        }
        {
            //wait for queue submission
            std::unique_lock<std::mutex> lock(event_mutex);
            while (enabled && event_queue.empty()) {
                queue_cv.wait(lock);
            }
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
        queue.Signal(this->timeline, current_timeline, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        queue.Wait(this->timeline, current_timeline - 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        queue.Submit(vk_allocator.GetCmdList());
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(std::move(allocator), current_timeline, true);
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
    while (executed_frame < _timeline) {
        std::this_thread::yield();
    }
}
#pragma endregion
} // namespace Moer::Render