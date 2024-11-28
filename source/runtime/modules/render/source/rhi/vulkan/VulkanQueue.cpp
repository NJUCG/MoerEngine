#include "VulkanQueue.h"
#include "RHICmdReorderer.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanRHIResource.h"
#include "io/IOCommon.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"
#include <memory>
#include <mutex>
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
        attachment_info.storeOp     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(_attachment.action));

        // std::memcpy(attachment_info.clearValue.color.float32, color.float32, sizeof(color.float32));
        attachment_info.clearValue.color = {
            _attachment.clear_color.x,
            _attachment.clear_color.y,
            _attachment.clear_color.z,
            _attachment.clear_color.w};

        return attachment_info;
    }

    VkRenderingAttachmentInfo FromDepthAttachmentInfo(const DepthAttachment& _attachment) {
        VkRenderingAttachmentInfo attachment_info{};
        attachment_info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment_info.pNext = nullptr;

        VulkanTexture* vk_texture = reinterpret_cast<VulkanTexture*>(_attachment.target);
        attachment_info.imageView = vk_texture->GetView();

        attachment_info.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachment_info.loadOp      = VulkanEnumTranslator::METoVKAttachmentLoadOp(GetLoadOp(GetDepthAction(_attachment.action)));
        attachment_info.storeOp     = VulkanEnumTranslator::METoVKAttachmentStoreOp(GetStoreOp(GetDepthAction(_attachment.action)));
        // std::memcpy(attachment_info.clearValue.color.float32, color.float32, sizeof(color.float32));
        attachment_info.clearValue.depthStencil = {_attachment.clear_depth, _attachment.clear_stencil};

        return attachment_info;
    }

    static bool IsBufferTextureWrite(uint64 _flags) {
        VulkanShaderResourceState state(_flags);
        switch (state.resource_type) {
            case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_UAV:
                return true;
            default: return false;
        }
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
            case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SRV:
                return true;
            default: return false;
        }
    }

#pragma endregion

#pragma region[ preprocessor ]
    struct VkCmdPreprocessor {
        VkTracker&       tracker;
        FunctionTable    m_funcs;
        VulkanAllocator& allocator;
        VulkanDevice&    device;
        EQueueType       current_queue;

        VkCmdPreprocessor(VulkanDevice& _device, VkTracker& _tracker, VulkanAllocator& _allocator, FunctionTable _funcs, EQueueType _current_queue = EQueueType::Graphics) : device(_device), tracker(_tracker), allocator(_allocator), m_funcs(_funcs), current_queue(_current_queue) {}
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

            Moer::Array<VulkanTexture*> write_map_textures;
            for (auto&& i : tracker.GetWritedStateTextures()) {
                if (vk_bindless_array->IsResourceAllocated(uint64(i))) {
                    write_map_textures.push_back(i);
                }
            }
            if (!write_map_textures.empty()) {
                for (auto&& i : write_map_textures) {
                    tracker.RecordState(i, tracker.ReadTexture(i, ETextureState::SAMPLE, pass_type));
                }
            }
            Moer::Array<VulkanBuffer*> write_map_buffers;
            for (auto&& i : tracker.GetWritedStateBuffers()) {
                if (vk_bindless_array->IsResourceAllocated(uint64(i))) {
                    write_map_buffers.push_back(i);
                }
            }
            if (!write_map_buffers.empty()) {
                for (auto&& i : write_map_buffers) {
                    tracker.RecordState(i, VK_ACCESS_2_SHADER_READ_BIT, _pipeline_stages);
                }
            }
            tracker.RecordState(vk_bindless_array->bindless_texture_descs, VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT, _pipeline_stages);
            tracker.RecordState(vk_bindless_array->bindless_buffer_descs, VK_ACCESS_2_DESCRIPTOR_BUFFER_READ_BIT_EXT, _pipeline_stages);
            tracker.RecordState(vk_bindless_array->bindless_array_buffer, VK_ACCESS_2_SHADER_READ_BIT, _pipeline_stages);
        }

        static VkAccessFlagBits2 GetBufferAccess(VulkanShaderResourceState _flag) {
            switch (_flag.resource_type) {
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_CBV:
                    return VK_ACCESS_2_SHADER_READ_BIT;
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SRV:
                    return VK_ACCESS_2_SHADER_READ_BIT;
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_UAV:
                    return VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
                default:
                    return VK_ACCESS_2_SHADER_READ_BIT;
            }
        }

        static VkAccessFlagBits2 GetTextureAccess(VulkanShaderResourceState _flag) {
            switch (_flag.resource_type) {
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SAMPLER:
                    return VK_ACCESS_2_SHADER_READ_BIT;
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SRV:
                    return VK_ACCESS_2_SHADER_READ_BIT;
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_UAV:
                    return VK_ACCESS_2_SHADER_WRITE_BIT;
                default:
                    assert(false && "Invalid texture resource type");
                    return VK_ACCESS_2_SHADER_READ_BIT;
            }
        }

        static VkImageLayout GetTextureLayout(VulkanShaderResourceState _flag) {
            switch (_flag.resource_type) {
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SAMPLER:
                    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_SRV: {
                    if (_flag.b_sampled)
                        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                    return VK_IMAGE_LAYOUT_GENERAL;
                }

                case SpvReflectResourceType::SPV_REFLECT_RESOURCE_FLAG_UAV:
                    return VK_IMAGE_LAYOUT_GENERAL;
                default:
                    assert(false && "Invalid texture resource type");
                    return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }

        void VisitArgs(const TArg& _arg, VulkanShaderResourceState _flag, VkPipelineStageFlagBits2 _pipelines) {
            if (_pipelines == VK_PIPELINE_STAGE_2_NONE) return;
            std::visit([&](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, BufferView>) {
                    if (_flag.resource_type == SPV_REFLECT_RESOURCE_FLAG_UNDEFINED) return;
                    auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_arg.GetBuffer());
                    tracker.RecordState(vk_buffer, GetBufferAccess(_flag), _pipelines);
                } else if constexpr (std::is_same_v<T, TextureView>) {
                    if (_flag.resource_type == SPV_REFLECT_RESOURCE_FLAG_UNDEFINED) return;
                    auto* vk_texture = reinterpret_cast<VulkanTexture*>(_arg.GetTexture());
                    tracker.RecordState(vk_texture, GetTextureAccess(_flag), GetTextureLayout(_flag), _pipelines, _arg.mip_level, _arg.num_mips);
                } else if constexpr (std::is_same_v<T, BindlessArrayRef>) {
                    HandleBindless(_arg, _pipelines);
                } else if constexpr (std::is_same_v<T, RaytracingSceneRef>) {
                    VulkanRaytracingScene* vk_as = ResourceCast(_arg.Get());
                    tracker.RecordState(vk_as->tlas->underlying_buffer, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, _pipelines);
                }
            },
                       _arg);
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
                case Command::EType::ShaderDispatch:
                    Visit(static_cast<const DispatchCmd*>(_cmd));
                    break;
                case Command::EType::SetDrawState:
                    Visit(static_cast<const SetDrawStateCmd*>(_cmd));
                    break;
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
                case Command::EType::Custom:
                    break;
                case Command::EType::UpdateBindlessArray:
                    Visit(static_cast<const UpdateBindlessArrayCmd*>(_cmd));
                    break;
                default:
                    assert(false && "Invalid command type");
            }
            return false;
        }

        void Visit(const UploadBufferCmd* _cmd) {
            auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->Handle());
            tracker.RecordState(vk_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        }
        void Visit(const UploadTextureCmd* _cmd) {
            auto* vk_texture = reinterpret_cast<VulkanTexture*>(_cmd->Handle());
            tracker.RecordState(vk_texture, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->MipLevel());
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
            tracker.RecordState(vk_src_texture, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->SrcMipLevel());
            tracker.RecordState(vk_dst_texture, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->DstMipLevel());
        }
        void Visit(const CopyBufferToTextureCmd* _cmd) {
            auto* vk_src_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->SrcHandle());
            auto* vk_dst_texture = reinterpret_cast<VulkanTexture*>(_cmd->DstHandle());
            tracker.RecordState(vk_src_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            tracker.RecordState(vk_dst_texture, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->MipLevel());
        }

        void Visit(const CopyTextureToBufferCmd* _cmd) {
            auto* vk_src_texture = reinterpret_cast<VulkanTexture*>(_cmd->SrcHandle());
            auto* vk_dst_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->DstHandle());
            tracker.RecordState(vk_src_texture, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, _cmd->MipLevel());
            tracker.RecordState(vk_dst_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        }

        void Visit(const CopyBackBufferCmd* _cmd) {
            auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->Handle());
            tracker.RecordState(vk_buffer, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
        }

        void Visit(const DispatchCmd* _cmd) {
            std::visit([this](auto&& _arg) {
                using T = std::decay_t<decltype(_arg)>;
                if constexpr (std::is_same_v<T, uint3>) {
                    return;
                } else if constexpr (std::is_same_v<T, BufferView>) {
                    tracker.RecordState(reinterpret_cast<VulkanBuffer*>(_arg.GetBuffer()), VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
                }
            },
                       _cmd->Param());
            // _cmd->Pipeline().binding_infos;
            auto func = [&](const TArg& _arg, ParamInfoFlags _flag) {
                VisitArgs(_arg, _flag.state_flags, _flag.pipeline_flags);
            };
            _cmd->IterateArgs(func);
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
                    scratch_size = (scratch_size + scratch_alignment - 1) & ~(scratch_alignment - 1);
                    scratch_size += param.mode == ERaytracingBuildMode::BUILD ? vk_geo->build_sizes_info.buildScratchSize : vk_geo->build_sizes_info.updateScratchSize;
                }
                scratch_size += scratch_alignment;

                scratch = allocator.AllocateScratch(scratch_size);
            }
            tracker.RecordState(ResourceCast(scratch.GetBuffer()), {VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});
            for (const AccelerationStructureBuildParam& param : params) {
                auto* vk_geo    = ResourceCast(param.geometry.Get());
                auto* vk_buffer = vk_geo->GetUnderlyingBuffer();

                auto* vtx_buffer = ResourceCast(vk_geo->GetInfo().vertex_buffer.Get());
                auto* idx_buffer = ResourceCast(vk_geo->GetInfo().index_buffer.Get());

                tracker.RecordState(vk_buffer, {VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});
                tracker.RecordState(vtx_buffer, {VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});
                tracker.RecordState(idx_buffer, {VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});

                tracker.EmplaceWriteBLAS(uint64(vk_geo));
            }
        }

        void Visit(const UpdateRaytracingSceneCmd* _cmd) {
            if (_cmd->InstancesToUpdate().empty()) {
                return;
            }
            //instance buffer
            VulkanBuffer*                instance_buffer = reinterpret_cast<VulkanBuffer*>(_cmd->InstanceBufferHandle());
            VulkanBuffer*                scratch_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd->ScratchBufferHandle());
            VulkanAccelerationStructure* tlas            = reinterpret_cast<VulkanAccelerationStructure*>(_cmd->TlasHandle());

            tracker.RecordState(instance_buffer, {VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT});
            tracker.RecordState(scratch_buffer, {VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});
            tracker.RecordState(tlas->underlying_buffer, {VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});

            for (const uint64& handle : tracker.GetWriteBLASStates()) {
                if (_cmd->HasGeometry(handle)) {
                    VulkanRaytracingGeometry* vk_geo = reinterpret_cast<VulkanRaytracingGeometry*>(handle);

                    tracker.RecordState(vk_geo->GetUnderlyingBuffer(), {VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});
                }
            }
        }

        void Visit(const BarrierCmd* _cmd) {
            if (!_cmd->IsQueueTransition()) {

                for (auto& barrier : _cmd->ReadBuffers()) {
                    auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
                    tracker.RecordState(vk_buffer, tracker.ReadBuffer(vk_buffer, barrier.state, barrier.pass_type));
                }
                for (auto& barrier : _cmd->WriteBuffers()) {
                    auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
                    tracker.RecordState(vk_buffer, tracker.WriteBuffer(vk_buffer, barrier.state, barrier.pass_type));
                }

                for (auto& barrier : _cmd->ReadTextures()) {
                    auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                    tracker.RecordState(vk_texture, tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type));
                }
                for (auto& barrier : _cmd->WriteTextures()) {
                    auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                    tracker.RecordState(vk_texture, tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type));
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
                tracker.RecordState(vk_buffer, tracker.ReadBuffer(vk_buffer, barrier.state, barrier.pass_type), src_queue_family, dst_queue_family);
            }
            for (auto& barrier : _cmd->WriteBuffers()) {
                auto* vk_buffer = reinterpret_cast<VulkanBuffer*>(barrier.handle);
                tracker.RecordState(vk_buffer, tracker.WriteBuffer(vk_buffer, barrier.state, barrier.pass_type), src_queue_family, dst_queue_family);
            }

            for (auto& barrier : _cmd->ReadTextures()) {
                auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                tracker.RecordState(vk_texture, tracker.ReadTexture(vk_texture, barrier.state, barrier.pass_type), src_queue_family, dst_queue_family);
            }
            for (auto& barrier : _cmd->WriteTextures()) {
                auto* vk_texture = reinterpret_cast<VulkanTexture*>(barrier.handle);
                tracker.RecordState(vk_texture, tracker.WriteTexture(vk_texture, barrier.state, barrier.pass_type), src_queue_family, dst_queue_family);
            }

            //queue transition
        }

        void Visit(const QueueTransferCmd* _cmd) {
            // EQueueType current_queue = this->allocator.
            EQueueType temp_queue = this->current_queue;
            if (_cmd->IsImport()) {
                _cmd->dst_queue = temp_queue;

                for (auto& barrier : _cmd->ImportTextures()) {
                    auto*         vk_texture = ResourceCast(barrier.texture.GetTexture());
                    auto          access     = tracker.ReadTexture(vk_texture, barrier.state);
                    VkImageLayout src_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    device.GetComputeQueue();
                    tracker.QueueTransferAcquireResource(
                        vk_texture,
                        device.GetQueueFamilyIndex(_cmd->src_queue),
                        device.GetQueueFamilyIndex(_cmd->dst_queue),
                        vk_texture->GetQueuePreferredLayout(_cmd->src_queue),
                        std::get<1>(access),
                        VK_ACCESS_2_NONE,
                        VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
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
                        std::get<1>(access));
                }
            }
        }
        void Visit(const SetDrawStateCmd* _cmd) {

            const auto& vbs = _cmd->VertexBuffers();
            for (const auto& vb : vbs) {
                auto* vk_buffer = ResourceCast(vb.first);
                tracker.RecordState(vk_buffer, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
            }
            const auto& ibs = _cmd->IndexBuffers();
            for (const auto& ib : ibs) {
                auto* vk_buffer = ResourceCast(ib.first);
                tracker.RecordState(vk_buffer, VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
            }

            auto func = [&](const TArg& _arg, ParamInfoFlags _flag) {
                VisitArgs(_arg, (VulkanShaderResourceState)_flag.state_flags, _flag.pipeline_flags);
            };
            _cmd->IterateArgs(func);

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
                    1);
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
                    1);
            }
        }

        void Visit(const UpdateBindlessArrayCmd* _cmd) {
            //use dispatch in the future
            // auto* vk_bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd->Handle());
            // tracker.RecordState(vk_bindless_array, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);

            if (_cmd->TextureUpdates().empty() && _cmd->BufferUpdates().empty()) {
                return;
            }

            VulkanBindlessArray* vk_bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd->Handle());
            tracker.RecordState(vk_bindless_array->bindless_array_buffer, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            if (!_cmd->TextureUpdates().empty()) {
                tracker.RecordState(vk_bindless_array->bindless_texture_descs, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            }

            if (!_cmd->BufferUpdates().empty()) {
                tracker.RecordState(vk_bindless_array->bindless_buffer_descs, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
            }
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
        VulkanCmdList&   cmd_list;
        VulkanAllocator& allocator;
        VkTracker&       tracker;

    public:
        VkCmdVisitor(VulkanDevice& _device, VulkanAllocator& _allocator, VkTracker& _tracker, VulkanCmdList& _cmd_list) : VulkanDeviceObject(&_device),
                                                                                                                          allocator(_allocator), tracker(_tracker),
                                                                                                                          cmd_list(_cmd_list) {}

        void VisitCmd(const Command* _cmd) {
            switch (_cmd->Type()) {
                case Command::EType::UploadBuffer:
                    Visit(static_cast<const UploadBufferCmd&>(*_cmd));
                    break;
                case Command::EType::CopyBackBuffer:
                    Visit(static_cast<const CopyBackBufferCmd&>(*_cmd));
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
                case Command::EType::TraceRay: {
                    assert(false && "TraceRay not implemented");
                    break;
                }
                case Command::EType::Custom: break;
                case Command::EType::UpdateBindlessArray: {
                    Visit(static_cast<const UpdateBindlessArrayCmd&>(*_cmd));
                    break;
                };
            }
        };
        void Visit(const UploadBufferCmd& _cmd) {
            auto data_span  = _cmd.Data();
            auto tmp_buffer = allocator.AllocateUploadBuffer(_cmd.ByteSize(), 16);
            cmd_list.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
            VulkanBuffer* buffer = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
            cmd_list.CopyBuffer(reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
                                buffer,
                                _cmd.ByteSize(),
                                tmp_buffer.GetByteOffset(),
                                _cmd.Offset());

            // LOG_INFO("upload temp buffer handle {} offset {} size {}", (uint64)ResourceCast(tmp_buffer.GetBuffer())->GetHandle(), tmp_buffer.GetByteOffset(), tmp_buffer.GetByteSize());
        }

        void Visit(const UploadTextureCmd& _cmd) {
            auto data_span  = _cmd.Data();
            auto tmp_buffer = allocator.AllocateUploadBuffer(data_span.size_bytes(), 16);
            cmd_list.CopyData(tmp_buffer, data_span.data(), data_span.size_bytes());
            VulkanTexture* texture = reinterpret_cast<VulkanTexture*>(_cmd.Handle());
            cmd_list.CopyBufferToTexture(reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
                                         texture,
                                         data_span.size_bytes(),
                                         tmp_buffer.GetByteOffset(),
                                         _cmd.Offset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const CopyBufferCmd& _cmd) {
            VulkanBuffer* src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
            VulkanBuffer* dst_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

            cmd_list.CopyBuffer(src_buffer,
                                dst_buffer,
                                _cmd.ByteSize(),
                                _cmd.SrcOffset(),
                                _cmd.DstOffset());
        }

        void Visit(const CopyBackBufferCmd& _cmd) {
            VulkanBuffer* src_buffer = reinterpret_cast<VulkanBuffer*>(_cmd.Handle());
            auto          tmp_buffer = allocator.AllocateReadbackBuffer(_cmd.ByteSize(), 16);

            tracker.RegisterFlushBuffer(tmp_buffer, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT);
            tracker.DispatchBarriers(cmd_list);
            cmd_list.CopyBuffer(src_buffer,
                                reinterpret_cast<VulkanBuffer*>(tmp_buffer.GetBuffer()),
                                _cmd.ByteSize(),
                                _cmd.Offset(),
                                tmp_buffer.GetByteOffset());

            // LOG_INFO("copyback temp buffer handle {} offset {} size {}", (uint64)ResourceCast(tmp_buffer.GetBuffer())->GetHandle(), tmp_buffer.GetByteOffset(), tmp_buffer.GetByteSize());

            // tracker.RegisterFlushBuffer(VulkanBuffer *_buffer, VkAccessFlagBits2 _access, VkPipelineStageFlagBits2 _stage)
            allocator.AddOnComplete([tmp_buffer, &cmd_list(cmd_list), src_data(_cmd.Data())]() {
                cmd_list.CopyData(src_data, tmp_buffer, tmp_buffer.GetByteSize());
            });
        }

        void Visit(const CopyTextureCmd& _cmd) {
            VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
            VulkanTexture* dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

            cmd_list.CopyTexture(src_texture,
                                 dst_texture,
                                 _cmd.Size(),
                                 _cmd.SrcOffset(),
                                 _cmd.DstOffset(),
                                 _cmd.SrcMipLevel(),
                                 _cmd.DstMipLevel());
        }

        void Visit(const CopyBufferToTextureCmd& _cmd) {
            VulkanBuffer*  src_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.SrcHandle());
            VulkanTexture* dst_texture = reinterpret_cast<VulkanTexture*>(_cmd.DstHandle());

            cmd_list.CopyBufferToTexture(src_buffer,
                                         dst_texture,
                                         _cmd.ByteSize(),
                                         _cmd.SrcOffset(),
                                         _cmd.DstOffset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const CopyTextureToBufferCmd& _cmd) {
            VulkanTexture* src_texture = reinterpret_cast<VulkanTexture*>(_cmd.SrcHandle());
            VulkanBuffer*  dst_buffer  = reinterpret_cast<VulkanBuffer*>(_cmd.DstHandle());

            cmd_list.CopyTextureToBuffer(src_texture,
                                         dst_buffer,
                                         _cmd.ByteSize(),
                                         _cmd.SrcOffset(),
                                         _cmd.DstOffset(),
                                         _cmd.Size(),
                                         _cmd.MipLevel());
        }

        void Visit(const DispatchCmd& _cmd) {
            static float4 dispatch_color = {0.0f, 0.0f, 1.0f, 1.0f};
            cmd_list.BeginLabel(_cmd.name, dispatch_color);
            const auto& param = _cmd.Param();

            PipelineHandle& pso = _cmd.Pipeline();
            cmd_list.SetPso(_cmd.Pipeline());
            const auto& args = _cmd.Args();

            cmd_list.BindDescriptors(pso, args);

            if (args.constants.size() > 0) {
                cmd_list.UploadPushConstants(
                    pso,
                    std::span<const uint>(args.constants.data(), args.constants.size()));
            }
            std::visit(
                [&](auto&& _param) {
                    using TParam = std::decay_t<decltype(_param)>;
                    if constexpr (std::is_same_v<TParam, uint3>) {
                        cmd_list.Dispatch(_param.x, _param.y, _param.z);
                    } else if constexpr (std::is_same_v<TParam, DispatchIndirectParam>) {
                        cmd_list.DispatchIndirect(
                            reinterpret_cast<VulkanBuffer*>(_param.indirect.GetBuffer()), _param.indirect.GetByteOffset());
                    }
                },
                param);

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
                .sType      = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .pNext      = nullptr,
                .flags      = 0,
                .renderArea = {
                    .offset = {pass_info.render_area.offset.x, pass_info.render_area.offset.y},
                    .extent = {pass_info.render_area.extent.width, pass_info.render_area.extent.height}},
                .layerCount           = 1,
                .colorAttachmentCount = uint(pass_info.color_attachments.size()),
                .pColorAttachments    = color_attachments.data(),
                .pDepthAttachment     = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr,
                .pStencilAttachment   = depth_stencil_attachment.has_value() ? &depth_stencil_attachment.value() : nullptr};

            cmd_list.BeginRendering(std::move(dynamic_rendering_info));

            cmd_list.SetPso(_cmd.Pipeline());

            cmd_list.BindDescriptors(pso, args);

            if (args.constants.size() > 0) {
                cmd_list.UploadPushConstants(
                    pso,
                    std::span<const uint>(args.constants.data(), args.constants.size()));
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
                 .maxDepth = 1.0f};

            viewport.y += viewport.height;
            viewport.height = -viewport.height;

            cmd_list.SetViewPort(viewport);
            cmd_list.SetScissor({rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height});
            for (const auto& draw_data : draw_datas) {
                if (draw_data.vtx_cnt > 0) {
                    StaticArray<VkBuffer, 4>     vertex_buffers{};
                    StaticArray<VkDeviceSize, 4> vtx_offsets{};

                    for (size_t i = 0; i < draw_data.vtx_cnt; ++i) {
                        vertex_buffers[i] = ResourceCast(draw_data.vtx_views[i].buffer)->GetHandle();
                        vtx_offsets[i]    = draw_data.vtx_views[i].offset;
                    }
                    cmd_list.SetVertexBuffers(0,
                                              draw_data.vtx_cnt,
                                              std::span<VkBuffer>(vertex_buffers.data(),
                                                                  draw_data.vtx_cnt),
                                              std::span<VkDeviceSize>(vtx_offsets.data(),
                                                                      draw_data.vtx_cnt));
                }

                // uint vtx_offset = draw_data.vtx_cnt != 0 ? draw_data.vtx_views[0].offset / draw_data.vtx_views[0].buffer->GetStride() : 0;

                std::visit(
                    [&](auto&& _idx_input) {
                        using IdxType = std::decay_t<decltype(_idx_input)>;
                        if constexpr (std::is_same_v<IdxType, IndexBuffer>) {
                            const auto& index_buffer = _idx_input.buffer;
                            uint64      offset       = index_buffer.GetByteOffset();

                            cmd_list.SetIndexBuffer(
                                reinterpret_cast<VulkanBuffer*>(index_buffer.GetBuffer()),
                                index_buffer.GetByteOffset(),
                                VulkanEnumTranslator::METoVKIndexType(_idx_input.stride));

                            for (size_t i = 0; i < draw_data.draw_params.size(); ++i) {
                                const SingleDrawParam& draw_param = draw_data.draw_params[i];
                                cmd_list.DrawIndexedInstanced(draw_param.index_cnt,
                                                              draw_param.instance_cnt,
                                                              draw_param.first_index,
                                                              draw_param.vertex_offset,
                                                              draw_param.first_instance);
                            }
                        } else if constexpr (std::is_same_v<IdxType, uint>) {
                            for (size_t i = 0; i < draw_data.draw_params.size(); ++i) {
                                const SingleDrawParam& draw_param = draw_data.draw_params[i];
                                cmd_list.DrawInstanced(draw_param.index_cnt,
                                                       draw_param.instance_cnt,
                                                       draw_param.vertex_offset,
                                                       draw_param.first_instance);
                            }
                        }
                    },
                    draw_data.idx_view);
            }
            cmd_list.EndRendering();
            cmd_list.EndLabel();
        }

        void Visit(const UpdateBindlessArrayCmd& _cmd) {
            VulkanBindlessArray* bindless_array = reinterpret_cast<VulkanBindlessArray*>(_cmd.Handle());
            auto                 texture_slots  = _cmd.StealTextureUpdates();
            auto                 buffer_slots   = _cmd.StealBufferUpdates();

            {
                bindless_array->Lock();
                for (const VulkanBindlessArray::TextureUpdateInfo& texture : texture_slots) {
                    uint indirect_handle                       = (m_device->GetSamplerIdx(texture.sampler) & 0xff) | (texture.slot & 0xffffff) << 8;
                    bindless_array->handles[texture.array_idx] = {texture.slot, 1, VulkanBindlessArray::Texture};
                }
                for (const auto& buffer : buffer_slots) {
                    uint indirect_handle                      = buffer.slot;
                    bindless_array->handles[buffer.array_idx] = {buffer.slot, 0, VulkanBindlessArray::Buffer};
                }
                bindless_array->Unlock();
            }
            Array<uint>                  to_update_texture_handle_indices(texture_slots.size());
            Array<std::pair<uint, uint>> to_update_textures_indices(texture_slots.size());
            Array<std::pair<uint, uint>> to_update_array_indices(buffer_slots.size() + texture_slots.size());
            Array<uint>                  slots_to_update(texture_slots.size() + buffer_slots.size());

            VulkanDescriptorHeap& heap = m_device->GetGlobalDescriptorHeap();

            //shuffle copy shader
            auto& shuffle_sd = m_device->internal_shaders->sd_component_shuffle;

            BufferView texture_staging{};
            BufferView buffer_staging{};
            uint64     texture_handle_stride = m_device->GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize;
            uint64     buffer_handle_stride  = m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize;

            byte* mapped_image_descs  = nullptr;
            byte* mapped_buffer_descs = nullptr;

            uint array_idx = 0;
            if (!texture_slots.empty()) {
                texture_staging = allocator.AllocateShaderBuffer(texture_slots.size() * texture_handle_stride);
                vmaMapMemory(m_device->GetVmaAllocator(), ResourceCast(texture_staging.GetBuffer())->GetAllocation(), (void**)&mapped_image_descs);

                //copy texture handles
                for (size_t i = 0; i < texture_slots.size(); ++i) {
                    const auto&    texture    = texture_slots[i];
                    VulkanTexture* vk_texture = ResourceCast(texture_slots[i].texture);
                    TextureView    view(vk_texture, texture.format, texture.mip_level, texture.num_mips);
                    uint           src_idx;
                    if (uint(vk_texture->GetAspectFlags() & ETextureAspectFlags::DEPTH_SLICE) != 0) {
                        // view.aspect_flags = ETextureAspectFlags::COLOR;
                        src_idx = heap.GetImageDescIdx(&view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

                    } else {
                        src_idx = heap.GetImageDescIdx(&view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    }
                    to_update_texture_handle_indices[i] = src_idx;
                    to_update_textures_indices[i]       = {i, texture_slots[i].slot};
                    to_update_array_indices[array_idx]  = {array_idx, texture_slots[i].array_idx};

                    uint indirect_handle = (m_device->GetSamplerIdx(texture.sampler) & 0xff) | (texture.slot & 0xffffff) << 8;

                    slots_to_update[array_idx] = indirect_handle;
                    array_idx++;
                }
                //warning: descriptor heap copy memory is not thread safe
                for (size_t i = 0; i < texture_slots.size(); ++i) {
                    memcpy(mapped_image_descs + i * texture_handle_stride, &heap.image_desc_data[to_update_texture_handle_indices[i]], texture_handle_stride);
                }
                vmaUnmapMemory(m_device->GetVmaAllocator(), ResourceCast(texture_staging.GetBuffer())->GetAllocation());

                //copy texture handles
            }

            Array<std::pair<uint, uint>> to_update_buffers_indices(buffer_slots.size());

            if (!buffer_slots.empty()) {
                buffer_staging = allocator.AllocateShaderBuffer(buffer_slots.size() * buffer_handle_stride);
                vmaMapMemory(m_device->GetVmaAllocator(), ResourceCast(buffer_staging.GetBuffer())->GetAllocation(), (void**)&mapped_buffer_descs);

                uint buffer_dst_slot_offset = bindless_array->buffers_offset_in_set / buffer_handle_stride;
                for (size_t i = 0; i < buffer_slots.size(); ++i) {
                    VulkanBuffer* vk_buffer = ResourceCast(buffer_slots[i].buffer);
                    uint          src_idx   = heap.GetBufferDescIdx(vk_buffer->GetView());
                    memcpy(mapped_buffer_descs + i * buffer_handle_stride, &heap.buffer_desc_data[src_idx], buffer_handle_stride);
                    to_update_buffers_indices[i] = {i, buffer_slots[i].slot + buffer_dst_slot_offset};

                    to_update_array_indices[array_idx] = {array_idx, buffer_slots[i].array_idx};
                    slots_to_update[array_idx]         = buffer_slots[i].slot;
                    array_idx++;
                }
                vmaUnmapMemory(m_device->GetVmaAllocator(), ResourceCast(buffer_staging.GetBuffer())->GetAllocation());
            }

            if (!texture_slots.empty() || !buffer_slots.empty()) {
                cmd_list.BeginLabel("UpdateBindlessArray", {0.0f, 1.0f, 0.0f, 1.0f});
                cmd_list.SetPso(shuffle_sd.handle);
                ComponentShuffleShader::Arg arg;

                {
                    //bindless array update
                    arg.component_cnt  = texture_slots.size() + buffer_slots.size();
                    arg.stride         = sizeof(uint) >> 2;
                    BufferView indices = allocator.AllocateShaderBuffer((texture_slots.size() + buffer_slots.size()) * sizeof(uint) * 2);
                    cmd_list.CopyData(indices, to_update_array_indices.data(), (texture_slots.size() + buffer_slots.size()) * sizeof(uint) * 2);
                    BufferView slots = allocator.AllocateShaderBuffer((texture_slots.size() + buffer_slots.size()) * sizeof(uint));
                    cmd_list.CopyData(slots, slots_to_update.data(), (texture_slots.size() + buffer_slots.size()) * sizeof(uint));
                    // BufferView test_temp = allocator.AllocateShaderBuffer(10000);

                    cmd_list.BindDescriptors(shuffle_sd.handle, shuffle_sd.SetArgs(arg, indices, slots, bindless_array->bindless_array_buffer->GetView()));
                    cmd_list.Dispatch((texture_slots.size() + buffer_slots.size() + 63) / 64, 1, 1);
                }

                if (!texture_slots.empty()) {
                    arg.component_cnt = texture_slots.size();
                    arg.stride        = m_device->GetOptionalProperties().descriptor_buffer_properties.sampledImageDescriptorSize >> 2;

                    BufferView indices = allocator.AllocateShaderBuffer(texture_slots.size() * sizeof(uint) * 2);
                    cmd_list.CopyData(indices, to_update_textures_indices.data(), texture_slots.size() * sizeof(uint) * 2);

                    cmd_list.BindDescriptors(shuffle_sd.handle, shuffle_sd.SetArgs(arg, indices, texture_staging, bindless_array->bindless_texture_descs->GetView(bindless_array->texture_offset_in_buffer)));
                    cmd_list.Dispatch((texture_slots.size() + 63) / 64, 1, 1);
                }

                if (!buffer_slots.empty()) {
                    arg.component_cnt = buffer_slots.size();
                    arg.stride        = m_device->GetOptionalProperties().descriptor_buffer_properties.storageBufferDescriptorSize >> 2;

                    BufferView buffer_indices = allocator.AllocateShaderBuffer(buffer_slots.size() * sizeof(uint) * 2);
                    cmd_list.CopyData(buffer_indices, to_update_buffers_indices.data(), buffer_slots.size() * sizeof(uint) * 2);

                    cmd_list.BindDescriptors(shuffle_sd.handle, shuffle_sd.SetArgs(arg, buffer_indices, buffer_staging, bindless_array->bindless_buffer_descs->GetView()));
                    cmd_list.Dispatch((buffer_slots.size() + 63) / 64, 1, 1);
                }

                cmd_list.EndLabel();
            }

            allocator.AddOnComplete([bindless_array,
                                     free_slots(_cmd.StealFreeSlots()),
                                     free_buffers(_cmd.StealFreeBuffers()),
                                     free_textures(std::move(_cmd.StealFreeTextures()))]() {
                bindless_array->OnFree(std::move(free_slots), std::move(free_textures), free_buffers);
            });
        }

        void Visit(const BuildAccelerationStructuresCmd& _cmd) {
            const Array<AccelerationStructureBuildParam>& build_params = _cmd.Params();

            Array<VkAccelerationStructureBuildGeometryInfoKHR> build_infos;
            Array<VkAccelerationStructureBuildRangeInfoKHR*>   build_ranges;

            uint64 scratch_alignment = m_device->GetOptionalProperties().acceleration_structure_properties.minAccelerationStructureScratchOffsetAlignment;

            BufferView    scratch_view    = _cmd.Scratch();
            VulkanBuffer* scratch_buf     = ResourceCast(scratch_view.GetBuffer());
            uint64        scratch_address = scratch_buf->DeviceAddress();
            //align scratch address
            scratch_address = (scratch_address + scratch_alignment - 1) & ~(scratch_alignment - 1);

            build_infos.reserve(build_params.size());
            build_ranges.reserve(build_params.size());

            uint64 scratch_offset = 0;

            for (const auto& build_param : build_params) {
                VulkanRaytracingGeometry* geometry = ResourceCast(build_param.geometry.Get());
                build_ranges.emplace_back(geometry->build_ranges.data());

                VkAccelerationStructureBuildGeometryInfoKHR build_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};

                build_info.dstAccelerationStructure = geometry->GetHandle();
                if (build_param.mode == ERaytracingBuildMode::UPDATE) {
                    build_info.srcAccelerationStructure = geometry->GetHandle();
                }
                build_info.type                      = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
                build_info.geometryCount             = geometry->build_geometries.size();
                build_info.pGeometries               = geometry->build_geometries.data();
                build_info.mode                      = VulkanEnumTranslator::METoVKBuildAccelerationStructureMode(build_param.mode);
                build_info.flags                     = VulkanEnumTranslator::METoVKAccelerationStructureBuildType(geometry->GetInfo().build_flags);
                build_info.scratchData.deviceAddress = scratch_address + scratch_offset;
                build_infos.emplace_back(build_info);

                scratch_offset = (scratch_offset + scratch_alignment - 1) & ~(scratch_alignment - 1);
                scratch_offset += build_param.mode == ERaytracingBuildMode::BUILD ? geometry->build_sizes_info.buildScratchSize : geometry->build_sizes_info.updateScratchSize;
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

            if (to_update.size() == 0) {
                return;
            }
            // VulkanRaytracingScene* scene   = reinterpret_cast<VulkanRaytracingScene*>(_cmd.Handle());
            BufferView staging = allocator.AllocateShaderBuffer(to_update.size() * sizeof(VkAccelerationStructureInstanceKHR));
            BufferView indices = allocator.AllocateShaderBuffer(to_update.size() * sizeof(uint32) * 2);

            Array<std::pair<uint, uint>> to_update_indices(to_update.size());

            for (size_t i = 0; i < to_update.size(); ++i) {
                const auto& id       = to_update[i];
                to_update_indices[i] = {i, id};
            }

            cmd_list.CopyData(staging, _cmd.InstanceData().data(), to_update.size() * sizeof(VkAccelerationStructureInstanceKHR));
            cmd_list.CopyData(indices, to_update_indices.data(), to_update.size() * sizeof(uint32) * 2);

            auto& shuffle_sd = m_device->internal_shaders->sd_component_shuffle;
            cmd_list.SetPso(shuffle_sd.handle);

            ComponentShuffleShader::Arg arg;
            arg.component_cnt = to_update.size();
            arg.stride        = sizeof(VkAccelerationStructureInstanceKHR) >> 2;

            cmd_list.BindDescriptors(shuffle_sd.handle, shuffle_sd.SetArgs(arg, indices, staging, instance_buffer->GetView()));

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
            tracker.FlushSrcState(instance_buffer, VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
            // tracker.RecordState(instance_buffer, {VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR});

            VkAccelerationStructureBuildGeometryInfoKHR build_info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
            build_info.dstAccelerationStructure = tlas->handle;
            build_info.type                     = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            build_info.geometryCount            = 1;
            build_info.mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;//now force build each frame

            VkAccelerationStructureGeometryKHR geometry{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geometry.geometryType                          = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            geometry.flags                                 = 0;
            geometry.geometry.instances.sType              = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
            geometry.geometry.instances.arrayOfPointers    = VK_FALSE;
            geometry.geometry.instances.data.deviceAddress = instance_buffer->DeviceAddress();

            VkAccelerationStructureBuildRangeInfoKHR build_range[] =
                {{}};
            build_range[0].primitiveCount                   = _cmd.InstanceCount();
            VkAccelerationStructureBuildRangeInfoKHR* range = build_range;
            build_info.pGeometries                          = &geometry;

            VkAccelerationStructureBuildSizesInfoKHR size_infos{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};

            uint instance_count = _cmd.InstanceCount();
            vkGetAccelerationStructureBuildSizesKHR(m_device->GetDevice(),
                                                    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
                                                    &build_info,
                                                    &instance_count,
                                                    &size_infos);

            assert(size_infos.accelerationStructureSize > 0 && "Invalid acceleration structure size!");
            assert(size_infos.buildScratchSize <= scratch_buffer->GetByteSize() && "Invalid scratch buffer size!");

            build_info.scratchData.deviceAddress = scratch_buffer->DeviceAddress();

            cmd_list.BeginLabel(std::format("UpdateTLAS with {} instances", _cmd.InstanceCount()), {});
            vkCmdBuildAccelerationStructuresKHR(cmd_list.GetHandle(), 1, &build_info, &range);
            cmd_list.EndLabel();
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

    VkNativeQueue::~VkNativeQueue() {
    }

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

        VkCommandBufferSubmitInfo cmd_info{.sType         = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                                           .pNext         = nullptr,
                                           .commandBuffer = cmd};
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
        wait_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage});
    }

    void VkNativeQueue::Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
        wait_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage});
    }
    void VkNativeQueue::Signal(VulkanFence* _fence, uint64 _fence_val, VkPipelineStageFlags2 _stage) {
        VkSemaphore sem = _fence->GetUnderlyingHandle();
        signal_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = sem,
            .value     = _fence_val,
            .stageMask = _stage});
    }
    void VkNativeQueue::Signal(VkSemaphore _sem, VkPipelineStageFlags2 _stage) {
        signal_infos.push_back(VkSemaphoreSubmitInfo{
            .sType     = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .pNext     = nullptr,
            .semaphore = _sem,
            .value     = 0,
            .stageMask = _stage});
    }
    void VkCommandQueue::Wait(WaitEvent _evt) {
        auto* fence = reinterpret_cast<VulkanFence*>(_evt.timeline_handle);
        {
            {
                // queue.Wait(fence, _evt.value, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
                // queue.SubmitEmpty();
            }

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

#pragma region[ VkCommandQueue ]
    WaitEvent VkCommandQueue::Execute(CmdSubmit&& _submit) {
        Timer timer{};
        timer.Start();
        auto  allocator_ptr = std::move(GetAllocator());
        auto& vk_allocator  = *allocator_ptr;
        auto& tracker       = vk_allocator.GetTracker();

        VkCmdVisitor  visitor(vk_device, vk_allocator, tracker, vk_allocator.GetCmdList());
        FunctionTable function_table{
            .is_resource_write       = &IsBufferTextureWrite,
            .is_resource_read        = &IsBufferTextureRead,
            .is_texture_sampled      = &IsTextureSampled,
            .is_resource_in_bindless = &IsResourceInBindlessArray};
        CmdReorderer reorderer{function_table};

        VkCmdPreprocessor preprocessor(vk_device, tracker, vk_allocator, function_table);

        for (const auto& cmd : _submit.cmds) {
            reorderer.AcceptCmd(cmd.get());
        }

        timer.Stop();
        // LOG_INFO("Reorderer time {}", timer.ElapsedMilliseconds());
        timer.Start();
        const auto& cmd_lists = reorderer.m_cmd_lists;
        bool        has_cmd   = !reorderer.m_cmd_lists.empty();
        uint64      last_time = last_frame;

        if (has_cmd) {
            vk_allocator.GetCmdList().Begin();
            if (queue.GetType() != EQueueType::Copy) {
                vk_device.GetGlobalDescriptorHeap().BeginPushDescriptors(last_time + 1);
            }

            std::string_view queue_label = queue.GetType() == EQueueType::Graphics ? "Graphics Exec" : queue.GetType() == EQueueType::Compute ? "Compute Exec" :
                                                                                                                                                "Copy Exec";
            vk_allocator.GetCmdList().BeginLabel(queue_label, {1.0f, 0.0f, 0.0f, 1.0f});
        }

        for (const CmdReorderer::LinkedCommandList& cmd_list : cmd_lists) {
            if (cmd_list.head == nullptr) {
                continue;
            }
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                preprocessor.VisitCmd(cmdnode->cmd);
            }
            tracker.ResolveBarriers();
            tracker.DispatchBarriers(vk_allocator.GetCmdList());
            for (const auto* cmdnode = cmd_list.head; cmdnode != nullptr; cmdnode = cmdnode->next) {
                const auto* cmd = cmdnode->cmd;
                visitor.VisitCmd(cmd);
            }
        }

        if (has_cmd) {
            tracker.RestoreState();
            tracker.DispatchBarriers(vk_allocator.GetCmdList());
            vk_allocator.GetCmdList().EndLabel();
            vk_allocator.GetCmdList().End();
            if (queue.GetType() != EQueueType::Copy) {
                vk_device.GetGlobalDescriptorHeap().EndPushDescriptors(last_time + 1);
            }
            tracker.Reset();
        }

        Array<RHIResource*> deleted_resources;
        vk_device.deferred_release_queue.PopAll(deleted_resources);
        _submit.callbacks.emplace_back([deleted_resources(std::move(deleted_resources))]() {
            for (auto* resource : deleted_resources) {
                MoerDelete(resource);
            }
        });
        timer.Stop();
        // LOG_INFO("Command Recording time {}", timer.ElapsedMilliseconds());
        if (_submit.cmds.empty()) {
            allocators.Push(allocator_ptr.release());
            std::unique_lock<std::mutex> lock(event_mutex);
            bool                         b_wake_up = false;

            b_wake_up = _submit.callbacks.size() != 0 || _submit.wait_events.size() != 0 || _submit.signal_events.size() != 0;
            if (b_wake_up) {
                auto end_tag = queue.GetType() == EQueueType::Graphics ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

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
            timer.Start();
            auto current_timeline = ++last_frame;
            // LOG_INFO("signal timeline {}", current_timeline);

            auto end_tag = queue.GetType() == EQueueType::Graphics ? VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            queue.Signal(timeline, current_timeline, end_tag);
            for (auto& evt : _submit.wait_events) {
                queue.Wait(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
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
            timer.Stop();
            // LOG_INFO("Submit time {}", timer.ElapsedMilliseconds());
            return {uint64(timeline), current_timeline};
        }
        return {uint64(timeline), 0ull};
    }

    void VkCommandQueue::Present(SwapchainRef _sc, TextureView _view) {
        VkSwapchain* sc           = ResourceCast(_sc.Get());
        auto         allocator    = std::move(GetAllocator());
        auto&        vk_allocator = *allocator;
        auto&        vk_cmd_list  = vk_allocator.GetCmdList();
        auto&        vk_tracker   = vk_allocator.GetTracker();
        sc->WaitFrameInFlight();
        auto [fence, idx, present_timeline] = sc->AquireNextImage();
        if (idx == UINT32_MAX) {
            //present null
            allocator->Reset();
            allocators.Push(allocator.release());
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
            vk_tracker.RecordState(swaphchain_tex, vk_tracker.WriteTexture(swaphchain_tex, ETextureState::TRANSFER));
            vk_tracker.ResolveBarriers();
            vk_tracker.DispatchBarriers(vk_cmd_list);
            //copy
            //todo: need transaction
            vk_cmd_list.InsertLabel("Copy Present Image", {0.0f, 0.0f, 0.0f, 1.0f});
            vk_cmd_list.CopyTexture(vk_src_tex, swaphchain_tex, _view.extent, {0, 0, 0}, {0, 0, 0}, 0, 0);
            vk_tracker.RecordState(swaphchain_tex,
                                   VK_ACCESS_2_NONE,
                                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                                   VK_PIPELINE_STAGE_2_COPY_BIT);
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
        queue.Wait(sc->GetImageReadyFence(idx), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
        queue.Signal(sc->GetRenderFinishedFence(), VK_PIPELINE_STAGE_2_COPY_BIT);
        queue.Submit(vk_allocator.GetCmdList(), sc->GetInFlightFence(present_timeline));
        sc->Present(queue.GetHandle(), idx);
        {
            std::unique_lock<std::mutex> lock(event_mutex);
            event_queue.emplace_back(std::move(allocator), current_timeline, true);
            queue_cv.notify_one();
        }
    }

    void VkCommandQueue::Sync() {
        Complete(last_frame);
    }

    UniquePtr<VulkanAllocator> VkCommandQueue::GetAllocator() {
        if (last_frame >= vk_device.cmd_alloc_limits) {
            Complete(last_frame - vk_device.cmd_alloc_limits + 1);
        }
        auto allocator = std::move(UniquePtr<VulkanAllocator>(allocators.Pop()));
        if (allocator) {
            // allocator->ResetCmdList();
            return std::move(allocator);
        }
        return MakeUnique<VulkanAllocator>(&vk_device, queue.GetType());
    }

    void VkCommandQueue::ExecuteThread() {
        while (enabled) {
            uint64 timeline;
            bool   b_wake_up = false;

            auto wait_util_reach_timeline = [&timeline, &b_wake_up, this]() {
                if (!b_wake_up) { return; }
                uint64 prev_timeline = executed_frame;
                while (prev_timeline < timeline && !executed_frame.compare_exchange_weak(prev_timeline, timeline)) {
                    std::this_thread::yield();
                }
            };

            auto visit_allocator = [&, &allocators(this->allocators), fence(this->timeline)](UniquePtr<VulkanAllocator>& _allocator) {
                _allocator->Complete(fence, timeline);
                // LOG_INFO("timeline {} complete", timeline);
                _allocator->Reset();
                allocators.Push(_allocator.release());
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
                auto*  fence    = reinterpret_cast<VulkanFence*>(_evt.timeline_handle);
                uint64 test_val = fence->GetValue();
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
                    [&](auto& _evt) {
                        using TEvent = std::decay_t<decltype(_evt)>;
                        if constexpr (std::is_same_v<TEvent, UniquePtr<VulkanAllocator>>) {
                            visit_allocator(_evt);
                        } else if constexpr (std::is_same_v<TEvent, FencePlaceHoler>) {
                            visit_fence(_evt);
                        } else if constexpr (std::is_same_v<TEvent, Array<std::function<void()>>>) {
                            visit_funcs(_evt);
                        } else if constexpr (std::is_same_v<TEvent, SignalEvent>) {
                            visit_signal_event(_evt);
                        }
                    },
                    evt->event);
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

    VkCopyQueue::VkCopyQueue(VulkanDevice& _device) : CopyQueue(),
                                                      device(_device),
                                                      queue(EQueueType::Copy, _device) {
        timeline = MoerNew(VulkanFence)(_device);
        thread   = std::jthread([this]() { ExecuteThread(); });
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

    IOWaitEvt VkCopyQueue::Execute(IOSubmission&& _submission) {
        assert(_submission.cmds.size() > 0 && "Empty submission");
        return {};
    }

    IOWaitEvt VkCopyQueue::Execute(CmdSubmit&& _evt) {
        Array<UniquePtr<Command>> cmds = std::move(_evt.cmds);

        auto current_timeline = last_frame;
        if (!cmds.empty()) {
            auto          allocator    = GetAllocator();
            auto&         vk_allocator = *allocator;
            auto&         vk_cmd_list  = vk_allocator.GetCmdList();
            auto&         vk_tracker   = vk_allocator.GetTracker();
            FunctionTable function_table{
                .is_resource_write       = &IsBufferTextureWrite,
                .is_resource_read        = &IsBufferTextureRead,
                .is_texture_sampled      = &IsTextureSampled,
                .is_resource_in_bindless = &IsResourceInBindlessArray};
            CmdReorderer reorderer{function_table};

            std::unique_lock<std::mutex> lk(exec_mutex);

            VkCmdPreprocessor preprocessor(device, vk_tracker, vk_allocator, {}, EQueueType::Copy);
            VkCmdVisitor      visitor(device, vk_allocator, vk_tracker, vk_cmd_list);

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
                queue.Wait(timeline, current_timeline - 1, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
            }
            for (auto& evt : _evt.wait_events) {
                queue.Wait(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value);
            }
            for (auto& evt : _evt.signal_events) {
                queue.Signal(reinterpret_cast<VulkanFence*>(evt.timeline_handle), evt.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
            }
            queue.Submit(vk_allocator.GetCmdList());
            {
                std::unique_lock<std::mutex> lock(event_mutex);
                event_queue.emplace_back(std::move(allocator), current_timeline, true);
                for (auto& evt : _evt.signal_events) {
                    event_queue.emplace_back(IOSignalEvt(evt.timeline_handle, evt.value), current_timeline, false);
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
                if (!b_wake_up) { return; }
                uint64 prev_timeline = executed_frame;
                while (prev_timeline < timeline && !executed_frame.compare_exchange_weak(prev_timeline, timeline)) {
                    std::this_thread::yield();
                }
            };

            auto visit_allocator = [&, &allocators(this->allocators), fence(this->timeline)](UniquePtr<VulkanAllocator>& _allocator) {
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
                    [&](auto& _evt) {
                        using TEvent = std::decay_t<decltype(_evt)>;
                        if constexpr (std::is_same_v<TEvent, UniquePtr<VulkanAllocator>>) {
                            visit_allocator(_evt);
                        } else if constexpr (std::is_same_v<TEvent, Placeholder>) {
                            visit_fence(_evt);
                        } else if constexpr (std::is_same_v<TEvent, Array<std::function<void()>>>) {
                            visit_funcs(_evt);
                        } else if constexpr (std::is_same_v<TEvent, IOSignalEvt>) {
                            visit_signal_event(_evt);
                        }
                    },
                    evt->event);
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
}// namespace Moer::Render