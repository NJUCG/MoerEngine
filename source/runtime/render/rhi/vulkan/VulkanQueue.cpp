#include "VulkanQueue.h"
#include "PixelFormat.h"
#include "RHICmdReorderer.h"
#include "VulkanAllocator.h"
#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanRHITrace.h"
#include "VulkanRHIResource.h"
#include "VulkanSubmissionExecutor.h"
#include "misc/Alignment.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "misc/Traits.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIIO.h"
#include "rhi/RHIResource.h"
#include "trace/Trace.h"

#include "VulkanCustomCommand.h"
#include "shader/ShaderPipeline.h"
#include "vulkan/vulkan_core.h"

#include <memory>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <thread>
#include <limits>
#include <variant>
namespace Moer::Render {

#pragma region[ utils ]

static const char* QueueTypeName(EQueueType type) {
    switch (type) {
        case EQueueType::Graphics:
            return "Graphics";
        case EQueueType::Compute:
            return "Compute";
        case EQueueType::Copy:
            return "Copy";
        default:
            return "Unknown";
    }
}

static uint64_t SteadyNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

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
            case Command::EType::Query:
                break;
            case Command::EType::CopyScope:
                assert(false && "CopyScope must be split before VkCommandQueue preprocessing");
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
                    static_cast<uint8>(barrier.array_count)
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
                    static_cast<uint8>(barrier.array_count)
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
            tracker.EmitLocalTransition(
                vk_buffer,
                tracker.ReadBuffer(vk_buffer, barrier.state, barrier.pass_type),
                src_queue_family,
                dst_queue_family
            );
        }
        for (auto& barrier : _cmd->WriteBuffers()) {
            auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
            tracker.EmitLocalTransition(
                vk_buffer,
                tracker.WriteBuffer(vk_buffer, barrier.state, barrier.pass_type),
                src_queue_family,
                dst_queue_family
            );
        }

        for (auto& barrier : _cmd->ReadTextures()) {
            auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
            auto  access     = tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type);
            tracker.EmitLocalTransition(
                vk_texture,
                std::get<0>(access),
                std::get<1>(access),
                std::get<2>(access),
                static_cast<uint8>(barrier.mip_level),
                static_cast<uint8>(barrier.mip_cnt),
                static_cast<uint8>(barrier.array_layer),
                static_cast<uint8>(barrier.array_count),
                src_queue_family,
                dst_queue_family
            );
        }
        for (auto& barrier : _cmd->WriteTextures()) {
            auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
            auto  access     = tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type);
            tracker.EmitLocalTransition(
                vk_texture,
                std::get<0>(access),
                std::get<1>(access),
                std::get<2>(access),
                static_cast<uint8>(barrier.mip_level),
                static_cast<uint8>(barrier.mip_cnt),
                static_cast<uint8>(barrier.array_layer),
                static_cast<uint8>(barrier.array_count),
                src_queue_family,
                dst_queue_family
            );
        }

        //queue transition
    }

    void Visit(const QueueTransferCmd* _cmd) {
        // EQueueType current_queue = this->allocator.
        EQueueType temp_queue = this->current_queue;
        EPassType  pass_type  = EPassType::Graphics;
        if (temp_queue == EQueueType::Compute) {
            pass_type = EPassType::Compute;
        }
        if (_cmd->IsImport()) {
            _cmd->dst_queue = temp_queue;

            for (auto& barrier : _cmd->ImportTextures()) {
                auto* vk_texture = ResourceCast(barrier.texture.GetTexture());
                auto  access     = barrier.access_write ?
                                       tracker.WriteTexture(vk_texture, barrier.state, pass_type) :
                                       tracker.ReadTexture(vk_texture, barrier.state, pass_type);
                tracker.EmitAcquirePlan(
                    vk_texture,
                    device.GetQueueFamilyIndex(_cmd->src_queue),
                    device.GetQueueFamilyIndex(_cmd->dst_queue),
                    vk_texture->GetQueuePreferredLayout(_cmd->src_queue),
                    std::get<1>(access),
                    static_cast<VkAccessFlagBits2>(std::get<0>(access)),
                    static_cast<VkPipelineStageFlagBits2>(std::get<2>(access))
                );
            }

            for (auto& barrier : _cmd->ImportBuffers()) {
                auto* vk_buffer = ResourceCast(barrier.buffer.GetBuffer());
                auto  access    = barrier.access_write ?
                                      tracker.WriteBuffer(vk_buffer, barrier.state, pass_type) :
                                      tracker.ReadBuffer(vk_buffer, barrier.state, pass_type);
                tracker.EmitAcquirePlan(
                    vk_buffer,
                    device.GetQueueFamilyIndex(_cmd->src_queue),
                    device.GetQueueFamilyIndex(_cmd->dst_queue),
                    static_cast<VkAccessFlagBits2>(std::get<0>(access)),
                    static_cast<VkPipelineStageFlagBits2>(std::get<1>(access))
                );
            }

        } else {
            _cmd->src_queue = temp_queue;

            for (auto& barrier : _cmd->ExportTextures()) {
                auto* vk_texture = ResourceCast(barrier.texture.GetTexture());
                auto  access     = tracker.ReadTexture(vk_texture, barrier.state);
                tracker.EmitReleasePlan(
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
                tracker.EmitReleasePlan(
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
    EQueueType             current_queue{EQueueType::Graphics};

public:
    VkCmdVisitor(
        VulkanDevice&          _device,
        VulkanAllocator&       _allocator,
        VkTracker&             _tracker,
        VulkanCmdList&         _cmd_list,
        const TCachedArgArray& _cached_args,
        ProfilerStorage*       _profiler = nullptr,
        EQueueType             _queue = EQueueType::Graphics
    ) :
        VulkanDeviceObject(&_device),
        allocator(_allocator),
        tracker(_tracker),
        cmd_list(_cmd_list),
        cached_args(_cached_args),
        profiler(_profiler),
        current_queue(_queue) {}

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
                Visit(static_cast<const QueueTransferCmd&>(*_cmd));
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
            case Command::EType::CopyScope:
                assert(false && "CopyScope must be split before VkCmdVisitor recording");
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
        bool                                     has_stencil = false;
        if (pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
            has_stencil              = FormatHasStencil(pass_info.depth_attachment.target->GetFormat());
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
            .pStencilAttachment = (depth_stencil_attachment.has_value() && has_stencil) ?
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
        bool                                     has_stencil = false;
        if (pass_info.depth_attachment.Valid()) {
            depth_stencil_attachment = FromDepthAttachmentInfo(pass_info.depth_attachment);
            has_stencil              = FormatHasStencil(pass_info.depth_attachment.target->GetFormat());
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
            .pStencilAttachment = (depth_stencil_attachment.has_value() && has_stencil) ?
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

    void Visit(const QueueTransferCmd& _cmd) {
        (void)_cmd;
        // QueueTransferCmd only emits barriers in VkCmdPreprocessor. Recording
        // it again here duplicates ownership acquire/release in the same cmd buffer.
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

    void Visit(const QueryCmd& _cmd) {
        if (!profiler) {
            return;
        }
        profiler->VisitQueryCmd(cmd_list, _cmd);
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
    // 这个锁只在AMD GPU上使用，因为AMD GPU没有TransferQueue
    // 在现代NVIDIA GPU上，这个锁不会被触发，接近0开销，不用在意性能
    std::unique_lock<std::mutex> guard;
    if (submit_mutex)
        guard = std::unique_lock<std::mutex>(*submit_mutex);

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
    std::unique_lock<std::mutex> guard;
    if (submit_mutex)
        guard = std::unique_lock<std::mutex>(*submit_mutex);

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

    VkResult submit_result = vkQueueSubmit2(queue, 1, &submit_info, _fence);
    if (submit_result != VK_SUCCESS) {
        LOG_ERROR(
            "[VkNativeQueue] vkQueueSubmit2 FAILED! result={}, queue={:#x}, type={}, "
            "wait_count={}, signal_count={}",
            (int)submit_result,
            (uint64)queue,
            (int)type,
            wait_infos.size(),
            signal_infos.size()
        );
    }
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
    if (fence != nullptr) {
        fence->HostWait(_evt.value);
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
namespace {
std::atomic<uint64_t> g_profiler_internal_query_id{1};
}

ProfilerStorage::ProfilerStorage(VulkanQueryRuntime& _query_runtime) : query_runtime(_query_runtime) {
    timestamp_period = query_runtime.GetTimestampPeriod();
    active           = true;
    name2sample.reserve(100);
}

void ProfilerStorage::CollectProfiling() {
    if (!active) {
        return;
    }

    timestamp_period = query_runtime.GetTimestampPeriod();
    resolved_gpu_samples.clear();
    resolved_gpu_samples.reserve(name2sample.size());

    Array<PendingSample> unresolved{};
    unresolved.reserve(pending_samples.size());

    for (auto& pending : pending_samples) {
        if (!pending.token.Valid()) {
            continue;
        }
        if (!pending.token.IsReady()) {
            unresolved.emplace_back(std::move(pending));
            continue;
        }

        QueryResult query_result = pending.token.GetFuture().Get();
        if (query_result.status != QueryStatus::Ready ||
            !std::holds_alternative<TimestampQueryResult>(query_result.payload)) {
            continue;
        }

        const auto& timestamp_result = std::get<TimestampQueryResult>(query_result.payload);
        auto        sample_iter      = name2sample.find(pending.name);
        if (sample_iter == name2sample.end()) {
            sample_iter = name2sample.emplace(pending.name, Sample{}).first;
        }

        uint64_t duration_ns = 0;
        if (timestamp_result.end_tick >= timestamp_result.begin_tick) {
            duration_ns = static_cast<uint64_t>(
                double(timestamp_result.end_tick - timestamp_result.begin_tick) *
                double(timestamp_period)
            );
        } else if (timestamp_result.duration_ns > 0.0) {
            duration_ns = static_cast<uint64_t>(timestamp_result.duration_ns);
        }

        if (duration_ns > 0) {
            sample_iter->second.Record(duration_ns);
            resolved_gpu_samples.emplace_back(
                ResolvedGpuSample{
                    .name       = pending.name,
                    .begin_tick = timestamp_result.begin_tick,
                    .end_tick   = timestamp_result.end_tick
                }
            );
        }
    }

    pending_samples = std::move(unresolved);
}

void ProfilerStorage::BeginProfilerSession(VulkanCmdList& _cmd_list, std::string_view _name) {
    if (!active) {
        return;
    }

    const uint64_t query_id = g_profiler_internal_query_id.fetch_add(1, std::memory_order_relaxed);
    QueryToken     token(
            query_id, QueryKind::Timestamp, std::string(_name), QueryFuture::Create()
    );
    query_runtime.BeginTimestamp(_cmd_list, token);
    active_scope_queries[std::string(_name)].emplace_back(token);
}

void ProfilerStorage::EndProfilerSession(VulkanCmdList& _cmd_list, std::string_view _name) {
    if (!active) {
        return;
    }

    const std::string key{_name};
    auto              iter = active_scope_queries.find(key);
    if (iter == active_scope_queries.end() || iter->second.empty()) {
        return;
    }

    QueryToken token = iter->second.back();
    iter->second.pop_back();
    if (iter->second.empty()) {
        active_scope_queries.erase(iter);
    }

    query_runtime.EndTimestamp(_cmd_list, token);
    pending_samples.emplace_back(PendingSample{.name = key, .token = token});
}

void ProfilerStorage::VisitQueryCmd(VulkanCmdList& _cmd, const QueryCmd& _query_cmd) {
    query_runtime.HandleQueryCommand(_cmd, _query_cmd);
    if (!active) {
        return;
    }
    if (_query_cmd.Token().kind == QueryKind::Timestamp &&
        _query_cmd.Op() == QueryCmd::EOp::EndTimestamp) {
        pending_samples.emplace_back(
            PendingSample{.name = _query_cmd.Token().name, .token = _query_cmd.Token()}
        );
    }
}
#pragma endregion

#pragma region[ VkCommandQueue ]
VulkanRecordedSubmit VkCommandQueue::Translate(CmdSubmit&& _submit, const CmdReorderer* _reordered, TrackerSeed seed) {
    TRACE_SCOPE_CAT("VkCommandQueue.Translate", "Queue");
    struct PreparedCmdBatch {
        CmdSubmit               submit;
        FunctionTable           function_table;
        UniquePtr<CmdReorderer> owned_reorderer;
        const CmdReorderer*     reorderer{nullptr};
        uint64                  last_time{0};
        double                  reorder_time_ms{0.0};
        bool                    has_cmd{false};
    };

    PreparedCmdBatch prepared{
        .submit = std::move(_submit),
        .function_table =
            FunctionTable{
                .is_resource_write       = &IsBufferTextureWrite,
                .is_resource_read        = &IsBufferTextureRead,
                .is_texture_sampled      = &IsTextureSampled,
                .is_resource_in_bindless = &IsResourceInBindlessArray,
                .lock_bdls_array         = &LockBindlessArray,
                .unlock_bdls_array       = &UnlockBindlessArray
            },
        .owned_reorderer = nullptr,
        .reorderer = nullptr,
        .last_time = last_frame,
        .reorder_time_ms = 0.0,
        .has_cmd = false
    };
    if (_reordered != nullptr) {
        prepared.reorderer = _reordered;
        prepared.has_cmd   = !prepared.reorderer->m_cmd_lists.empty();
    } else {
        prepared.owned_reorderer = MakeUnique<CmdReorderer>(prepared.function_table, prepared.submit.cached_args);
        prepared.reorderer       = prepared.owned_reorderer.get();

        Timer reorder_timer{};
        reorder_timer.Start();
        for (const auto& cmd : prepared.submit.cmds) {
            prepared.owned_reorderer->AcceptCmd(cmd.get());
        }
        reorder_timer.Stop();
        prepared.reorder_time_ms = reorder_timer.ElapsedMilliseconds();
        prepared.has_cmd         = !prepared.reorderer->m_cmd_lists.empty();
    }

    VulkanRecordedSubmit         recorded{};
    recorded.allocator = std::move(GetAllocator());
    recorded.submit.emplace(std::move(prepared.submit));
    recorded.has_cmd   = prepared.has_cmd;
    recorded.reorder_time_ms = prepared.reorder_time_ms;
    const uint64 descriptor_frame = ++record_frame;

    if (!recorded.has_cmd) {
        return recorded;
    }

    auto& vk_allocator = *recorded.allocator;
    auto& tracker      = vk_allocator.GetTracker();

    // §9.3: Initialize tracker src states from preprocess seed (§7.2 seed_tracker).
    if (!seed.textures.empty() || !seed.buffers.empty()) {
        tracker.InitFromSeed(seed);
    }

    VkCmdVisitor visitor(
        vk_device,
        vk_allocator,
        tracker,
        vk_allocator.GetCmdList(),
        recorded.submit->cached_args,
        &profiler_storage,
        queue.GetType()
    );
    VkCmdPreprocessor preprocessor(
        vk_device,
        tracker,
        vk_allocator,
        prepared.function_table,
        recorded.submit->cached_args,
        queue.GetType()
    );

    vk_allocator.GetCmdList().Begin();
    query_runtime.BeginRecord(vk_allocator.GetCmdList(), recorded.submit->query_tokens);
    if (recorded.submit->b_tick_profiling) {
        profiler_storage.CollectProfiling();
        cached_profiler_entry = profiler_storage.GetProfilerEntry();
        cached_gpu_samples    = profiler_storage.GetResolvedGpuSamples();
#if defined(MOER_TRACE_ENABLED) && MOER_TRACE_ENABLED && defined(MOER_TRACE_GPU_ENABLED) && \
    MOER_TRACE_GPU_ENABLED
        EmitResolvedGpuTraceEvents(cached_gpu_samples);
#endif
        profiler_storage.BeginProfilerSession(vk_allocator.GetCmdList(), "Graphics Exec");
    }

    if (queue.GetType() != EQueueType::Copy) {
        vk_device.GetGlobalDescriptorHeap().BeginPushDescriptors(static_cast<uint>(descriptor_frame));
    }

    std::string_view queue_label = queue.GetType() == EQueueType::Graphics ? "Graphics Exec" :
                                   queue.GetType() == EQueueType::Compute  ? "Compute Exec" :
                                                                             "Copy Exec";
    vk_allocator.GetCmdList().BeginLabel(queue_label, {1.0f, 0.0f, 0.0f, 1.0f});

    Timer preprocess_timer{};
    uint  layer_count = 0;
    for (const CmdReorderer::LinkedCommandList& cmd_list : prepared.reorderer->m_cmd_lists) {
        if (layer_count == 0) {
            vk_allocator.GetCmdList().BeginLabel("Begin Layers", {0.0f, 0.0f, 1.0f, 1.0f});
        }
        if (cmd_list.head == nullptr) {
            continue;
        }
        preprocess_timer.Start();
        for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
            RHITRACE_LOG(
                verbose,
                "[RHITrace][Preprocess][{}][Layer {}] cmd_type={} name={}",
                QueueTypeName(queue.GetType()),
                layer_count,
                uint(cmdnode->cmd->Type()),
                cmdnode->cmd->name
            );
            preprocessor.VisitCmd(cmdnode->cmd);
        }
        tracker.ResolveBarriers();
        tracker.DispatchBarriers(vk_allocator.GetCmdList());
        preprocess_timer.Stop();
        recorded.preprocess_time_ms += preprocess_timer.ElapsedMilliseconds();

        vk_allocator.GetCmdList().InsertLabel(
            std::format("Layer {}", layer_count++), {0.0f, 0.0f, 1.0f, 1.0f}
        );
        for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
            RHITRACE_LOG(
                verbose,
                "[RHITrace][ExecuteCmd][{}][Layer {}] cmd_type={} name={}",
                QueueTypeName(queue.GetType()),
                layer_count - 1,
                uint(cmdnode->cmd->Type()),
                cmdnode->cmd->name
            );
            visitor.VisitCmd(cmdnode->cmd);
        }
    }
    if (layer_count > 0) {
        vk_allocator.GetCmdList().EndLabel();
    }

    if (recorded.submit->b_tick_profiling) {
        profiler_storage.EndProfilerSession(vk_allocator.GetCmdList(), "Graphics Exec");
    }
    vk_allocator.GetCmdList().EndLabel();
    vk_allocator.GetCmdList().End();

    if (queue.GetType() != EQueueType::Copy) {
        vk_device.GetGlobalDescriptorHeap().EndPushDescriptors(static_cast<uint>(descriptor_frame));
    }
    tracker.Reset();

    Array<RHIResource*> deleted_resources;
    vk_device.deferred_release_queue.PopAll(deleted_resources);
    recorded.submit->callbacks.emplace_back([deleted_resources(std::move(deleted_resources))]() {
        for (auto* resource : deleted_resources) {
            MoerDelete(resource);
        }
    });

    return recorded;
}

WaitEvent VkCommandQueue::SubmitRecorded(VulkanRecordedSubmit&& _recorded) {
    TRACE_SCOPE_CAT("VkCommandQueue.SubmitRecorded", "Queue");
    std::unique_lock<std::mutex> lock(exec_mtx);

    if (!_recorded.has_cmd || !_recorded.submit.has_value() || _recorded.submit->cmds.empty()) {
        if (_recorded.submit.has_value() && !_recorded.submit->query_tokens.empty()) {
            query_runtime.ResolveAsError(_recorded.submit->query_tokens, "submit without commands");
        }
        if (_recorded.allocator) {
            allocators.Push(_recorded.allocator.release());
        }

        const bool has_submit = _recorded.submit.has_value();
        if (has_submit && (!_recorded.submit->callbacks.empty() || !_recorded.submit->signal_events.empty())) {
            const uint64 current_timeline = ++last_frame;
            queue.Signal(timeline, current_timeline);
            queue.SubmitEmpty();
            Array<WaitEvent> completion_waits{};
            completion_waits.emplace_back(uint64(timeline), current_timeline);
            VulkanSubmissionExecutor::EnqueueQueueCompletion(
                current_timeline,
                std::move(completion_waits),
                this,
                current_timeline,
                UniquePtr<VulkanAllocator>{},
                std::move(_recorded.submit->callbacks),
                std::move(_recorded.submit->signal_events)
            );
            return {uint64(timeline), current_timeline};
        }
        return {uint64(timeline), last_frame};
    }

    auto& vk_allocator = *_recorded.allocator;

    auto current_timeline = ++last_frame;
    auto end_tag          = queue.GetType() == EQueueType::Graphics ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT :
                                                                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    queue.Signal(timeline, current_timeline, end_tag);
    for (auto& evt : _recorded.submit->wait_events) {
        queue.Wait(
            reinterpret_cast<VulkanFence*>(evt.timeline_handle),
            evt.value,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
        );
    }
    for (auto& evt : _recorded.submit->signal_events) {
        queue.Signal(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value, end_tag);
    }
    queue.Submit(vk_allocator.GetCmdList());
    query_runtime.FinalizeSubmit(current_timeline, vk_allocator.GetCmdList().GetHandle());
    executed_queue.Enqueue(current_timeline);
    Array<WaitEvent> completion_waits{};
    completion_waits.emplace_back(uint64(timeline), current_timeline);
    VulkanSubmissionExecutor::EnqueueQueueCompletion(
        current_timeline,
        std::move(completion_waits),
        this,
        current_timeline,
        std::move(_recorded.allocator),
        std::move(_recorded.submit->callbacks),
        std::move(_recorded.submit->signal_events)
    );

    if (_recorded.submit->b_tick_profiling) {
        profiler_storage.RegisterCpuTimestamp("Command Reorder", _recorded.reorder_time_ms);
        profiler_storage.RegisterCpuTimestamp("Command Preprocess", _recorded.preprocess_time_ms);
        profiler_storage.AdvanceFrame();
    }
    return {uint64(timeline), current_timeline};
}

WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
    TRACE_SCOPE_CAT("VkCommandQueue.Execute", "Queue");
    VulkanRecordedSubmit recorded = Translate(std::move(_submit));
    return SubmitRecorded(std::move(recorded));
}

void VkCommandQueue::Present(SwapchainRef _sc, TextureView _view) {
    (void)_sc;
    (void)_view;
    LOG_ERROR("VkCommandQueue::Present is deprecated; use RHIExecutor::Submit(..., present) instead");
    assert(false && "VkCommandQueue::Present is unsupported after the submission executor rework");
}

void VkCommandQueue::Sync() {
    Complete(last_frame);
}

WaitEvent VkCommandQueue::SubmitRestoreTransitions(
    Array<ReadBuffer>&& _read_buffers,
    Array<ReadTexture>&& _read_textures,
    std::optional<WaitEvent> _wait_evt
) {
    if (_read_buffers.empty() && _read_textures.empty()) {
        return {uint64(timeline), last_frame};
    }
    CommandList restore_cmd{};
    if (!_read_buffers.empty()) {
        restore_cmd.BufferBarriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Graphics,
            std::move(_read_buffers),
            Array<WriteBuffer>{}
        );
    }
    if (!_read_textures.empty()) {
        restore_cmd.TextureBarriers(
            EQueueType::Graphics, EQueueType::Graphics, EPassType::Graphics, std::move(_read_textures)
        );
    }
    CmdSubmit restore_submit = restore_cmd.Submit();
    if (_wait_evt.has_value()) {
        restore_submit.wait_events.emplace_back(_wait_evt.value());
    }
    return Execute(std::move(restore_submit));
}

#if defined(MOER_TRACE_ENABLED) && MOER_TRACE_ENABLED && defined(MOER_TRACE_GPU_ENABLED) && \
    MOER_TRACE_GPU_ENABLED
void VkCommandQueue::EmitResolvedGpuTraceEvents(const Array<ProfilerStorage::ResolvedGpuSample>& _samples) {
    if (_samples.empty() || !Moer::Trace::IsRecording()) {
        return;
    }
    const float timestamp_period = query_runtime.GetTimestampPeriod();
    if (timestamp_period <= 0.0f) {
        return;
    }

    uint64_t first_tick = std::numeric_limits<uint64_t>::max();
    for (const auto& sample : _samples) {
        if (sample.end_tick < sample.begin_tick) {
            continue;
        }
        first_tick = std::min(first_tick, sample.begin_tick);
    }
    if (first_tick == std::numeric_limits<uint64_t>::max()) {
        return;
    }

    if (!gpu_trace_anchor_valid || first_tick < gpu_trace_tick_anchor) {
        gpu_trace_tick_anchor    = first_tick;
        gpu_trace_time_anchor_ns = SteadyNowNs();
        gpu_trace_anchor_valid   = true;
    }

    const uint64_t    track_id =
        Moer::Trace::MakeGpuQueueTrackId(0u, static_cast<uint32_t>(queue.GetType()));
    const std::string track_name = std::format("GPU0/Queue({})", QueueTypeName(queue.GetType()));

    auto tick_to_ns = [&](uint64_t tick) -> uint64_t {
        if (tick < gpu_trace_tick_anchor) {
            return gpu_trace_time_anchor_ns;
        }
        const long double delta_ticks = static_cast<long double>(tick - gpu_trace_tick_anchor);
        const long double delta_ns    = delta_ticks * static_cast<long double>(timestamp_period);
        return gpu_trace_time_anchor_ns + static_cast<uint64_t>(std::max<long double>(0.0, delta_ns));
    };

    for (const auto& sample : _samples) {
        if (sample.end_tick < sample.begin_tick) {
            continue;
        }

        uint64_t begin_ns = tick_to_ns(sample.begin_tick);
        uint64_t end_ns   = tick_to_ns(sample.end_tick);
        if (end_ns < begin_ns) {
            end_ns = begin_ns;
        }

        Moer::Trace::EmitScope(
            Moer::Trace::EmitScopeDesc{
                .name        = sample.name,
                .category    = "GPU",
                .track_type  = Moer::Trace::TrackType::GPUQueue,
                .track_id    = track_id,
                .depth       = 0,
                .ts_begin_ns = begin_ns,
                .ts_end_ns   = end_ns,
                .track_name  = track_name
            }
        );
    }
}
#endif

ProfileData VkCommandQueue::GetProfilerEntry() {
    return profiler_storage.GetProfilerEntry();
}

UniquePtr<VulkanAllocator> VkCommandQueue::GetAllocator() {
    std::lock_guard<std::mutex> alloc_lock(alloc_mtx);
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

void VkCommandQueue::Complete(uint64 _timeline) {
    while (executed_frame < _timeline) {
        timeline->HostWait(_timeline);
        std::this_thread::yield();
    }
}

void VkCommandQueue::MarkExecutionComplete(uint64 _timeline) {
    timeline->Notify(_timeline);
    uint64 prev_timeline = executed_frame.load(std::memory_order_acquire);
    while (prev_timeline < _timeline &&
           !executed_frame.compare_exchange_weak(prev_timeline, _timeline, std::memory_order_acq_rel)
    ) {
        std::this_thread::yield();
    }
}

void VkCommandQueue::ResolveAllocatorCompletion(UniquePtr<VulkanAllocator>&& _allocator, uint64 _timeline) {
    if (_allocator) {
        _allocator->Complete(timeline, _timeline);
        query_runtime.ResolveCompleted(_timeline);
        _allocator->Reset();
        allocators.Push(_allocator.release());
    }
}

#pragma endregion

#pragma region[ copy queue ]

VkCopyQueue::VkCopyQueue(VulkanDevice& _device) :
    CopyQueue(),
    device(_device),
    queue(EQueueType::Copy, _device) {
    timeline = MoerNew(VulkanFence)(_device);
}

VkCopyQueue::~VkCopyQueue() {
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

//TODO:看看barrier
VulkanRecordedSubmit VkCopyQueue::Translate(CmdSubmit&& _evt, const CmdReorderer* _reordered) {
    if (!_evt.query_tokens.empty()) {
        QueryResult error_result{};
        error_result.status = QueryStatus::Error;
        error_result.name   = "Query unsupported on copy queue";
        for (const auto& token : _evt.query_tokens) {
            if (!token.Valid()) {
                continue;
            }
            error_result.kind     = token.kind;
            error_result.query_id = token.id;
            token.Resolve(error_result);
        }
    }

    VulkanRecordedSubmit recorded{};
    recorded.submit.emplace(std::move(_evt));
    if (recorded.submit->cmds.empty()) {
        return recorded;
    }

    FunctionTable function_table{
        .is_resource_write       = &IsBufferTextureWrite,
        .is_resource_read        = &IsBufferTextureRead,
        .is_texture_sampled      = &IsTextureSampled,
        .is_resource_in_bindless = &IsResourceInBindlessArray
    };
    UniquePtr<CmdReorderer> owned_reorder{};
    const CmdReorderer*     reorder = _reordered;
    if (reorder == nullptr) {
        owned_reorder = MakeUnique<CmdReorderer>(function_table, recorded.submit->cached_args);
        for (const auto& cmd : recorded.submit->cmds) {
            owned_reorder->AcceptCmd(cmd.get());
        }
        reorder = owned_reorder.get();
    }
    recorded.has_cmd = (reorder != nullptr) && !reorder->m_cmd_lists.empty();
    if (!recorded.has_cmd) {
        return recorded;
    }

    auto allocator     = GetAllocator();
    auto& vk_allocator = *allocator;
    auto& vk_cmd_list  = vk_allocator.GetCmdList();
    auto& vk_tracker   = vk_allocator.GetTracker();

    VkCmdPreprocessor preprocessor(
        device, vk_tracker, vk_allocator, {}, recorded.submit->cached_args, EQueueType::Copy
    );
    VkCmdVisitor visitor(
        device,
        vk_allocator,
        vk_tracker,
        vk_cmd_list,
        recorded.submit->cached_args,
        nullptr,
        EQueueType::Copy
    );

    vk_cmd_list.Begin();
    vk_cmd_list.BeginLabel("Copy", {0.0f, 1.0f, 1.0f, 1.0f});
    const auto& cmd_lists = reorder->m_cmd_lists;
    for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
        if (cmd_list.head == nullptr) {
            continue;
        }
        for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
            RHITRACE_LOG(
                verbose,
                "[RHITrace][Preprocess][Copy] cmd_type={} name={}",
                uint(cmdnode->cmd->Type()),
                cmdnode->cmd->name
            );
            preprocessor.VisitCmd(cmdnode->cmd);
        }
        vk_tracker.ResolveBarriers();
        vk_tracker.DispatchBarriers(vk_cmd_list);
        for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
            RHITRACE_LOG(
                verbose,
                "[RHITrace][ExecuteCmd][Copy] cmd_type={} name={}",
                uint(cmdnode->cmd->Type()),
                cmdnode->cmd->name
            );
            visitor.VisitCmd(cmdnode->cmd);
        }
    }

    vk_cmd_list.EndLabel();
    vk_cmd_list.End();
    vk_tracker.Reset();
    recorded.allocator = std::move(allocator);
    return recorded;
}

IOWaitEvt VkCopyQueue::SubmitRecorded(VulkanRecordedSubmit&& _recorded) {
    std::unique_lock<std::mutex> lk(exec_mutex);
    auto current_timeline = last_frame;
    if (!_recorded.has_cmd || !_recorded.allocator) {
        if (_recorded.submit.has_value() &&
            (!_recorded.submit->callbacks.empty() || !_recorded.submit->signal_events.empty())) {
            current_timeline = ++last_frame;
            queue.Signal(timeline, current_timeline, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            queue.SubmitEmpty();
            Array<WaitEvent> completion_waits{};
            completion_waits.emplace_back(uint64(timeline.Get()), current_timeline);
            Array<IOSignalEvt> signal_events{};
            signal_events.reserve(_recorded.submit->signal_events.size());
            for (const auto& evt : _recorded.submit->signal_events) {
                signal_events.emplace_back(IOSignalEvt{evt.timeline_handle, evt.value});
            }
            VulkanSubmissionExecutor::EnqueueCopyQueueCompletion(
                current_timeline,
                std::move(completion_waits),
                this,
                current_timeline,
                UniquePtr<VulkanAllocator>{},
                std::move(_recorded.submit->callbacks),
                std::move(signal_events)
            );
        }
        return {uint64(timeline.Get()), current_timeline};
    }

    auto& vk_allocator = *_recorded.allocator;
    current_timeline   = ++last_frame;
    queue.Signal(timeline, current_timeline, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    if (current_timeline > 1) {
        queue.Wait(timeline, current_timeline - 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }
    for (auto& evt : _recorded.submit->wait_events) {
        queue.Wait(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value);
    }
    for (auto& evt : _recorded.submit->signal_events) {
        queue.Signal(
            reinterpret_cast<VulkanFence*>(evt.timeline_handle),
            evt.value,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
        );
    }
    queue.Submit(vk_allocator.GetCmdList());
    Array<WaitEvent> completion_waits{};
    completion_waits.emplace_back(uint64(timeline.Get()), current_timeline);
    Array<IOSignalEvt> signal_events{};
    signal_events.reserve(_recorded.submit->signal_events.size());
    for (const auto& evt : _recorded.submit->signal_events) {
        signal_events.emplace_back(IOSignalEvt{evt.timeline_handle, evt.value});
    }
    VulkanSubmissionExecutor::EnqueueCopyQueueCompletion(
        current_timeline,
        std::move(completion_waits),
        this,
        current_timeline,
        std::move(_recorded.allocator),
        std::move(_recorded.submit->callbacks),
        std::move(signal_events)
    );
    return {uint64(timeline.Get()), current_timeline};
}

IOWaitEvt VkCopyQueue::Execute(CmdSubmit&& _evt) {
    VulkanRecordedSubmit recorded = Translate(std::move(_evt));
    return SubmitRecorded(std::move(recorded));
}

void VkCopyQueue::Sync(uint64 _timeline) {
    Complete(_timeline);
}

FenceRef VkCopyQueue::GetFenceHandle() {
    return FenceRef(timeline);
}

UniquePtr<VulkanAllocator> VkCopyQueue::GetAllocator() {
    std::lock_guard<std::mutex> alloc_lock(alloc_mtx);
    if (last_frame >= VulkanDevice::cmd_alloc_limits) {
        Complete(last_frame - VulkanDevice::cmd_alloc_limits + 1);
    }
    auto allocator = std::move(UniquePtr<VulkanAllocator>(allocators.Pop()));
    if (allocator) {
        // allocator->ResetCmdList();
        return std::move(allocator);
    }
    return MakeUnique<VulkanAllocator>(&device, EQueueType::Copy);
}

void VkCopyQueue::Complete(uint64 _timeline) {
    uint64 spin_count = 0;
    while (executed_frame < _timeline) {
        timeline->HostWait(_timeline);
        std::this_thread::yield();
        ++spin_count;
        if (spin_count == 10'000'000) {
            LOG_ERROR(
                "[CopyQueue] Complete: STILL WAITING after 10M spins! "
                "_timeline={}, executed_frame={}",
                _timeline,
                executed_frame.load()
            );
        }
        if (spin_count % 50'000'000 == 0) {
            LOG_ERROR(
                "[CopyQueue] Complete: STUCK! spins={}, _timeline={}, executed_frame={}",
                spin_count,
                _timeline,
                executed_frame.load()
            );
        }
    }
}

void VkCopyQueue::MarkExecutionComplete(uint64 _timeline) {
    timeline->Notify(_timeline);
    uint64 prev_timeline = executed_frame.load(std::memory_order_acquire);
    while (prev_timeline < _timeline &&
           !executed_frame.compare_exchange_weak(prev_timeline, _timeline, std::memory_order_acq_rel)
    ) {
        std::this_thread::yield();
    }
}

void VkCopyQueue::ResolveAllocatorCompletion(UniquePtr<VulkanAllocator>&& _allocator, uint64 _timeline) {
    if (_allocator) {
        _allocator->Complete(timeline.Get(), _timeline);
        _allocator->Reset();
        allocators.Push(_allocator.release());
    }
}
#pragma endregion
} // namespace Moer::Render
