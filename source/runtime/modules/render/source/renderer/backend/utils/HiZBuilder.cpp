#include "HiZBuilder.h"
#include "PixelFormat.h"
#include "math/Base.h"
#include "math/Function.h"
#include "misc/STL.h"
#include "rendergraph/RenderGraphPass.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderResourceManager.h"
#include "../deferred/RenderResourceDeferred.h"
namespace Moer {
    static constexpr uint32_t max_mip_levels = 12;
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
                .SetNumMips(std::min(uint32_t(std::log2(std::min(target_extent.x, target_extent.y))), max_mip_levels))
                .SetFormat(PF_R16_SFLOAT)
                .SetClearAttachment(RHIClearAttachment(EClearAttachment::COLOR))
                .SetPreferredLayout(ETextureLayout::TEXTURE_LAYOUT_COMMON)
                .SetUsageFlags(ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS));

        srv = g_rhi->RHICreateTextureSRV(texture, PF_R16_SFLOAT, 0, texture->GetNumMips());
        uavs.resize(texture->GetNumMips());
        for (uint32_t i = 0; i < texture->GetNumMips(); ++i) {
            uavs[i] = g_rhi->RHICreateTextureUAV(texture, PF_R16_SFLOAT, i);
        }
        auto*                    cmd_list = g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());
        RHIBarrierDependencyInfo barrier_info;
        barrier_info.texture_barriers.resize(1);
        auto& barrier = barrier_info.texture_barriers[0];
        barrier.SetTexture(texture)
            .SetSrcTextureLayout(TEXTURE_LAYOUT_UNDEFINED)
            .SetDstTextureLayout(TEXTURE_LAYOUT_COMMON)
            .SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::COLOR));

        cmd_list->BeginRecording();
        cmd_list->SetPipelineBarrier(barrier_info);
        cmd_list->EndRecording();
        RHIFenceRef fence = g_rhi->RHICreateFence(RHIFenceCreateInfo{EFenceUsageFlags::TIMELINE});

        RHISubmitInfo submit_info;
        submit_info.Signal(fence, 1);

        RHICommandQueue* queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
        queue->SubmitCommands(1, cmd_list, &submit_info);
        fence->Wait(1);
        MoerDelete(queue);
        if (sampler == nullptr) {
            RHISamplerCreateInfo create_info(SF_NEAREST, TEXTURE_LAYOUT_COMMON);
            create_info.SetCompareOp(SCF_NEVER)
                .SetAddressMode(ESamplerAddressMode::SAM_CLAMP_TO_EDGE);
            sampler = g_rhi->RHICreateSampler(create_info);
        }
    }

    struct HiZBuilder::Impl {
        Impl() {
            builder_shader = ShaderResourceManager::GetInstance().GetShader<BuildHiZShader>();
            assert(builder_shader && "Failed to load HiZ builder shader");
            pso = g_rhi->RHICreateComputePipelineState(builder_shader);
            RHISamplerCreateInfo create_info(SF_NEAREST, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            create_info.SetCompareOp(SCF_NEVER);
            depth_sampler = g_rhi->RHICreateSampler(create_info);
        }

        void Dispatch(RHICommandListBase* _cmd_list, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {
            //currently just support graphics cmd list
            RHIGraphicsCommandList* graphics_cmd_list = static_cast<RHIGraphicsCommandList*>(_cmd_list);
            graphics_cmd_list->SetPipelineState(pso);

            Vector2i depth_size = Vector2i(_depth_buffer->GetTexture()->GetExtent3D());
            //calculate power of two
            Vector2i                   mip0_size = Vector2i(_hiz_buffer.texture->GetExtent3D());
            BuildHiZShader::Parameters params;

            HiZConfig& config   = params.config;
            config.b_mip0       = true;
            config.size         = mip0_size;
            config.target_level = 0;

            uint32_t mip_count   = std::min(uint32_t(std::log2(std::min(mip0_size.x, mip0_size.y))), max_mip_levels);
            params.depth_buffer  = _depth_buffer;
            params.depth_sampler = depth_sampler;

            RHIBatchedShaderParameters batched_params;

            RHISubresourceRange range(ETextureAspectFlags::COLOR);
            range.num_mips  = 1;
            range.mip_index = 0;
            RHIBarrierDependencyInfo depth_barrier_info;
            depth_barrier_info.texture_barriers.resize(2);
            auto& depth_barrier = depth_barrier_info.texture_barriers[0];
            depth_barrier.SetTexture(_depth_buffer->GetTexture())
                .SetSrcTextureLayout(TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE)
                .SetDstTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                .SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE))
                .SetSrcStage(PS_ALL_GRAPHICS)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::DEPTH_STENCIL_WRITE | ERHIAccessFlags::DEPTH_STENCIL_READ);

            auto& hiz_barrier = depth_barrier_info.texture_barriers[1];
            hiz_barrier.SetTexture(_hiz_buffer.texture)
                .SetSubResourceRange(range)
                .SetDstTextureLayout(TEXTURE_LAYOUT_COMMON)
                .SetSrcStage(PS_COMPUTE_SHADER)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE);
            // .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);

            graphics_cmd_list->SetPipelineBarrier(depth_barrier_info);//set depth buffer to shader read only

            RHIBarrierDependencyInfo barrier_info;
            barrier_info.texture_barriers.resize(2);
            auto& barrier = barrier_info.texture_barriers[0];

            barrier.SetTexture(_hiz_buffer.texture)
                .SetSubResourceRange(range)
                .SetSrcStage(PS_COMPUTE_SHADER)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE);

            auto& barrier2 = barrier_info.texture_barriers[1];
            barrier2.SetTexture(_hiz_buffer.texture)
                .SetSubResourceRange(range)
                .SetSrcStage(PS_COMPUTE_SHADER)
                .SetDstStage(PS_COMPUTE_SHADER)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);

            for (uint32_t i = 0; i < mip_count; ++i) {
                params.target = _hiz_buffer.uavs[i];
                if (i == 0) {

                } else {
                    //set target mip
                    config.size         = Vector2i(std::max(1, mip0_size.x >> (i)), std::max(1, mip0_size.y >> (i)));
                    config.b_mip0       = false;
                    config.target_level = i;

                    params.depth_buffer = _hiz_buffer.srv;
                }
                barrier.sub_resource_range.mip_index  = i;
                barrier2.sub_resource_range.mip_index = i + 1;
                batched_params.SetParameters(builder_shader, params);

                g_rhi->RHISetBatchedShaderParameters(pso, batched_params);
                Vector3i group_count = Vector3i((config.size.t.x + 7) >> 3u, (config.size.t.y + 7) >> 3u, 1);
                graphics_cmd_list->Dispatch(group_count);
                if (i == mip_count - 1) {
                    barrier_info
                        .texture_barriers[1]
                        .SetTexture(_depth_buffer->GetTexture())
                        .SetDstTextureLayout(TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE)
                        .SetSrcTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        .SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE))
                        .SetSrcStage(PS_COMPUTE_SHADER)
                        .SetDstStage(PS_EARLY_FRAGMENT_TESTS)
                        .SetDstAccessFlags(ERHIAccessFlags::DEPTH_STENCIL_WRITE | ERHIAccessFlags::DEPTH_STENCIL_READ)
                        .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);
                }
                graphics_cmd_list->SetPipelineBarrier(barrier_info);//set current mip level to shader read only
            }
        }

        void Dispatch(RenderContext& _context, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {

            auto& rg      = _context.GetRenderGraph();
            auto  mip_cnt = std::min(uint32_t(std::log2(std::min(_hiz_buffer.texture->GetExtent3D().x, _hiz_buffer.texture->GetExtent3D().y))), max_mip_levels);

            rg.AddComputePass("Build HiZ", [&, depth_buffer(_depth_buffer)](RenderGraph::Builder& _builder) {
                auto depth = rg.ImportTexture("Depth Buffer", depth_buffer->GetTexture());

                 auto hiz = rg.ImportTexture("HiZ Buffer", _hiz_buffer.texture);
                _builder.ReadTexture(depth);
                rg.GetTexture(hiz)->GetUAV(0);
                _builder.WriteTexture(hiz); }, [&, mip_cnt](RenderPassContext& _pass_context) {
                BuildHiZShader::Parameters params;
                Vector2i                   mip0_size = Vector2i(_hiz_buffer.texture->GetExtent3D());

                HiZConfig& config   = params.config;
                config.b_mip0       = true;
                config.size         = mip0_size;
                config.target_level = 0;
                params.target       = _hiz_buffer.uavs[0];
                params.depth_buffer = _depth_buffer;
                RHIBatchedShaderParameters batched_params;
                auto&                      cmd_list = _context.GetCommandList();
                cmd_list.SetPipelineState(pso);
                
                batched_params.SetParameters(builder_shader, params); });

            for (uint i = 1; i < mip_cnt; ++i) {
                rg.AddComputePass("Build HiZ", [&, i](RenderGraph::Builder& _builder) mutable {
                        auto hiz = rg.ImportTexture("HiZ Buffer", _hiz_buffer.texture);
                        rg.GetTexture(hiz)->GetUAV(i);
                        _builder.ReadTexture(hiz);
                        _builder.WriteTexture(hiz); }, [&, i](RenderPassContext& _pass_context) {
                                      BuildHiZShader::Parameters params;
                                      Vector2i                   mip0_size = Vector2i(_hiz_buffer.texture->GetExtent3D());

                                      HiZConfig& config   = params.config;
                                      config.b_mip0       = false;
                                      config.size         = Vector2i(std::max(1, mip0_size.x >> i), std::max(1, mip0_size.y >> i));
                                      config.target_level = i;
                                      params.target       = _hiz_buffer.uavs[i];
                                      params.depth_buffer = _hiz_buffer.srv;
                                      RHIBatchedShaderParameters batched_params;
                                      auto&                      cmd_list = _context.GetCommandList();
                                      batched_params.SetParameters(builder_shader, params);

                                      cmd_list.Dispatch(Vector3i((config.size.t.x + 7) >> 3u, (config.size.t.y + 7) >> 3u, 1)); });
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
    void HiZBuilder::DispatchBuildHiZ(RHIGraphicsCommandList* _cmd_list, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {
        impl->Dispatch(_cmd_list, _depth_buffer, _hiz_buffer);
    }

    void HiZBuilder::DispatchBuildHiZ(RenderContext& _context, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {
        impl->Dispatch(_context, _depth_buffer, _hiz_buffer);
    }
}// namespace Moer