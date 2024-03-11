#include "HiZBuilder.h"
#include "PixelFormat.h"
#include "math/Base.h"
#include "math/Function.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderResourceManager.h"

namespace Moer {
    static constexpr uint32_t max_mip_levels = 10;
    IMPLEMENT_SHADER_TYPE(BuildHiZShader, "utils/BuildHiZ.hlsl", "main", ST_COMPUTE);

    void HiZBuffer::InitFromDepthExtent(Vector2i extent) {
        Vector2i target_extent = Vector2i(RoundDownToPowerOf2(uint32_t(extent.x)), RoundDownToPowerOf2(uint32_t(extent.y)));
        if (texture) {
            //check if result extent is the same
            if (Vector2i(texture->GetExtent3D()) == target_extent) {
                return;
            }
        }
        texture = g_rhi->RHICreateTexture(
            RHITextureCreateInfo::Create2D(
                "Hi-Z Buffer")
                .SetExtent(target_extent)
                .SetNumMips(std::min(uint32_t(std::log2(std::min(extent.x, extent.y))), max_mip_levels))
                .SetFormat(PF_R16_SFLOAT)
                .SetClearAttachment(RHIClearAttachment(EClearAttachment::COLOR))
                .SetUsageFlags(ETextureUsageFlags::SHADER_RESOURCE | ETextureUsageFlags::UNORDERED_ACCESS));

        srv = g_rhi->RHICreateTextureSRV(texture, PF_R16_SFLOAT, 0, texture->GetNumMips());
        uavs.resize(texture->GetNumMips());
        for (uint32_t i = 0; i < texture->GetNumMips(); ++i) {
            uavs[i] = g_rhi->RHICreateTextureUAV(texture, PF_R16_SFLOAT, i);
        }
        if (sampler == nullptr) {
            RHISamplerCreateInfo create_info(SF_NEAREST, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            create_info.SetCompareOp(SCF_NEVER);
            sampler = g_rhi->RHICreateSampler(create_info);
        }
    }

    struct HiZBuilder::Impl {
        Impl() {
            builder_shader = ShaderResourceManager::GetInstance().GetShader<BuildHiZShader>();
#if _DEBUG
            assert(builder_shader && "Failed to load HiZ builder shader");
#endif
            pso = g_rhi->RHICreateComputePipelineState(builder_shader);
            RHISamplerCreateInfo create_info(SF_NEAREST, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            create_info.SetCompareOp(SCF_NEVER);
            depth_sampler = g_rhi->RHICreateSampler(create_info);
        }

        void Dispatch(RHICommandListBase* cmd_list, RHISRVRef depth_buffer, HiZBuffer& hiz_buffer) {
            //currently just support graphics cmd list
            RHIGraphicsCommandList* graphics_cmd_list = static_cast<RHIGraphicsCommandList*>(cmd_list);
            graphics_cmd_list->SetPipelineState(pso);

            Vector2i depth_size = Vector2i(depth_buffer->GetTexture()->GetExtent3D());
            //calculate power of two
            Vector2i                   mip0_size = Vector2i(RoundDownToPowerOf2(uint32_t(depth_size.x)), RoundDownToPowerOf2(uint32_t(depth_size.y)));
            BuildHiZShader::Parameters params;

            HiZConfig& config   = params.config;
            config.b_mip0       = true;
            config.size         = mip0_size;
            config.target_level = 0;

            uint32_t mip_count   = std::min(uint32_t(std::log2(std::min(depth_size.x, depth_size.y))), max_mip_levels);
            params.depth_buffer  = depth_buffer;
            params.depth_sampler = depth_sampler;

            RHIBatchedShaderParameters batched_params;

            RHISubresourceRange range(ETextureAspectFlags::COLOR);
            range.num_mips  = 1;
            range.mip_index = 0;
            RHIBarrierDependencyInfo depth_barrier_info;
            depth_barrier_info.texture_barriers.resize(2);
            auto& depth_barrier = depth_barrier_info.texture_barriers[0];
            depth_barrier.SetTexture(depth_buffer->GetTexture())
                .SetSrcTextureLayout(TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE)
                .SetDstTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                .SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE))
                .SetSrcStage(PS_LATE_FRAGMENT_TESTS)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::DEPTH_STENCIL_WRITE | ERHIAccessFlags::DEPTH_STENCIL_READ);

            auto& hiz_barrier = depth_barrier_info.texture_barriers[1];
            hiz_barrier.SetTexture(hiz_buffer.texture)
                .SetSubResourceRange(range)
                .SetSrcStage(PS_COMPUTE_SHADER)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);

            graphics_cmd_list->SetPipelineBarrier(depth_barrier_info);//set depth buffer to shader read only

            RHIBarrierDependencyInfo barrier_info;
            barrier_info.texture_barriers.resize(2);
            auto& barrier = barrier_info.texture_barriers[0];

            barrier.SetTexture(hiz_buffer.texture)
                // .SetSrcTextureLayout(TEXTURE_LAYOUT_WRITE)
                // .SetDstTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                .SetSubResourceRange(range)
                .SetSrcStage(PS_COMPUTE_SHADER)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE);

            auto& barrier2 = barrier_info.texture_barriers[1];
            barrier2.SetTexture(hiz_buffer.texture)
                // .SetSrcTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                // .SetDstTextureLayout(TEXTURE_LAYOUT_WRITE)
                .SetSubResourceRange(range)
                .SetSrcStage(PS_COMPUTE_SHADER)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);

            for (uint32_t i = 0; i < mip_count; ++i) {
                params.target = hiz_buffer.uavs[i];
                if (i == 0) {

                } else {
                    //set target mip
                    config.size         = Vector2i(std::max(1, mip0_size.x >> (i - 1)), std::max(1, mip0_size.y >> (i - 1)));
                    config.b_mip0       = false;
                    config.target_level = i;

                    params.depth_buffer = hiz_buffer.srv;
                }
                barrier.sub_resource_range.mip_index  = i;
                barrier2.sub_resource_range.mip_index = i + 1;
                batched_params.SetParameters(builder_shader, params);

                g_rhi->RHISetBatchedShaderParameters(pso, batched_params);
                Vector3i group_count = Vector3i((config.size.t.x + 7) >> 3u, (config.size.t.y + 7) >> 3u, 1);
                graphics_cmd_list->Dispatch(group_count);
                if (i == mip_count - 1) {
                    barrier_info.texture_barriers.resize(1);
                }
                graphics_cmd_list->SetPipelineBarrier(barrier_info);//set current mip level to shader read only
            }
        }

        RHIShaderRef builder_shader;

        RHIComputePipelineStateRef pso;
        RHISamplerRef              depth_sampler;
    };
    HiZBuilder::HiZBuilder() {
        impl = UniquePtr<Impl>(MoerNew(Impl)());
    }

    HiZBuilder::~HiZBuilder() {
        impl.reset();
    }
    HiZBuilder& HiZBuilder::GetInstance() {
        static HiZBuilder instance;
        return instance;
    }
    void HiZBuilder::DispatchBuildHiZ(RHIGraphicsCommandList* cmd_list, RHISRVRef depth_buffer, HiZBuffer& hiz_buffer) {
        impl->Dispatch(cmd_list, depth_buffer, hiz_buffer);
    }
}// namespace Moer