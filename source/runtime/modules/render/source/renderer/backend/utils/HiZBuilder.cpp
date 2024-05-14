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
#include <utility>
#include <variant>
namespace Moer {
    IMPLEMENT_SHADER_TYPE(BuildHiZShader, "utils/BuildHiZ.hlsl", "main", ST_COMPUTE);

    static constexpr uint32_t max_mip_levels = 12;

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
                .SetPreferredLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED)
                .SetUsageFlags(ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS));

        srv = g_rhi->RHICreateTextureSRV(texture, PF_R16_SFLOAT, 0, texture->GetNumMips());
        uavs.resize(texture->GetNumMips());
        srvs.resize(texture->GetNumMips() - 1);
        for (uint32_t i = 0; i < texture->GetNumMips(); ++i) {
            uavs[i] = g_rhi->RHICreateTextureUAV(texture, PF_R16_SFLOAT, i);
            if (i < texture->GetNumMips() - 1) {
                srvs[i] = g_rhi->RHICreateTextureSRV(texture, PF_R16_SFLOAT, i, 1);
            }
        }
        // auto*                    cmd_list = g_rhi->RHICreateCopyCommandList(g_rhi->RHIGetCurrentCommandAllocator());
        // RHIBarrierDependencyInfo barrier_info;
        // barrier_info.texture_barriers.resize(1);
        // auto& barrier = barrier_info.texture_barriers[0];
        // barrier.SetTexture(texture)
        //     .SetSrcTextureLayout(TEXTURE_LAYOUT_UNDEFINED)
        //     .SetDstTextureLayout(TEXTURE_LAYOUT_COMMON)
        //     .SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::COLOR));

        // cmd_list->BeginRecording();
        // cmd_list->SetPipelineBarrier(barrier_info);
        // cmd_list->EndRecording();
        // RHIFenceRef fence = g_rhi->RHICreateFence(RHIFenceCreateInfo{EFenceUsageFlags::TIMELINE});

        // RHISubmitInfo submit_info;
        // submit_info.Signal(fence, 1);

        // RHICommandQueue* queue = g_rhi->RHICreateCommandQueue(ECommandQueueType::COPY);
        // queue->SubmitCommands(1, cmd_list, &submit_info);
        // fence->Wait(1);
        // MoerDelete(queue);
        if (sampler == nullptr) {
            RHISamplerCreateInfo create_info(SF_NEAREST, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            create_info.SetCompareOp(SCF_NEVER)
                .SetAddressMode(ESamplerAddressMode::SAM_CLAMP_TO_EDGE);
            sampler = g_rhi->RHICreateSampler(create_info);
        }
    }
    template<uint N>
    struct Num {
        static const constexpr auto value = N;
    };
    template<class F, uint... Is>
    void For(F _func, std::integer_sequence<uint, Is...>) {
        (_func(Num<Is>{}), ...);
    }
    struct HiZBuilder::Impl {
        static constexpr uint max_mip_batch_cnt = 4;
        Impl() {

            For([this](auto _i) {
                constexpr uint               idx = _i.value;
                BuildHiZShader::TMutationSet set;
                set.SetMutation<BuildHiZShader::HIZ_BATCH_CNT>(idx + 1);
                builder_shaders[idx] = ShaderResourceManager::GetInstance().GetShader<BuildHiZShader>(set.GetMutationID());
                assert(builder_shaders[idx] && "Failed to load HiZ builder shader");

                psos[idx] = g_rhi->RHICreateComputePipelineState(builder_shaders[idx]);
            },
                std::make_integer_sequence<uint, max_mip_batch_cnt>{});
            // builder_shader = ShaderResourceManager::GetInstance().GetShader<BuildHiZShader>();
            // assert(builder_shader && "Failed to load HiZ builder shader");
            // pso = g_rhi->RHICreateComputePipelineState(builder_shader);
            RHISamplerCreateInfo create_info(SF_NEAREST, TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            create_info.SetCompareOp(SCF_NEVER);
            depth_sampler = g_rhi->RHICreateSampler(create_info);
        }

        // void Dispatch(RHICommandListBase* _cmd_list, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {
        //     //currently just support graphics cmd list
        //     RHIGraphicsCommandList* graphics_cmd_list = static_cast<RHIGraphicsCommandList*>(_cmd_list);
        //     graphics_cmd_list->SetPipelineState(pso);

        //     Vector2i depth_size = Vector2i(_depth_buffer->GetTexture()->GetExtent3D());
        //     //calculate power of two
        //     Vector2i                   mip0_size = Vector2i(_hiz_buffer.texture->GetExtent3D());
        //     BuildHiZShader::Parameters params;

        //     HiZConfig& config   = params.config;
        //     config.b_mip0       = true;
        //     config.size         = mip0_size;
        //     config.target_level = 0;

        //     uint32_t mip_count   = std::min(uint32_t(std::log2(std::min(mip0_size.x, mip0_size.y))), max_mip_levels);
        //     params.depth_buffer  = _depth_buffer;
        //     params.depth_sampler = depth_sampler;

        //     RHIBatchedShaderParameters batched_params;

        //     RHISubresourceRange range(ETextureAspectFlags::COLOR);
        //     range.num_mips  = 1;
        //     range.mip_index = 0;
        //     RHIBarrierDependencyInfo depth_barrier_info;
        //     depth_barrier_info.texture_barriers.resize(2);
        //     auto& depth_barrier = depth_barrier_info.texture_barriers[0];
        //     depth_barrier.SetTexture(_depth_buffer->GetTexture())
        //         .SetSrcTextureLayout(TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE)
        //         .SetDstTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        //         .SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE))
        //         .SetSrcStage(PS_ALL_GRAPHICS)
        //         .SetDstStage(PS_COMPUTE_SHADER)
        //         .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
        //         .SetSrcAccessFlags(ERHIAccessFlags::DEPTH_STENCIL_WRITE | ERHIAccessFlags::DEPTH_STENCIL_READ);

        //     auto& hiz_barrier = depth_barrier_info.texture_barriers[1];
        //     hiz_barrier.SetTexture(_hiz_buffer.texture)
        //         .SetSubResourceRange(range)
        //         .SetDstTextureLayout(TEXTURE_LAYOUT_COMMON)
        //         .SetSrcStage(PS_COMPUTE_SHADER)
        //         .SetDstStage(PS_COMPUTE_SHADER)
        //         .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE);
        //     // .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);

        //     graphics_cmd_list->SetPipelineBarrier(depth_barrier_info);//set depth buffer to shader read only

        //     RHIBarrierDependencyInfo barrier_info;
        //     barrier_info.texture_barriers.resize(2);
        //     auto& barrier = barrier_info.texture_barriers[0];

        //     barrier.SetTexture(_hiz_buffer.texture)
        //         .SetSubResourceRange(range)
        //         .SetSrcStage(PS_COMPUTE_SHADER)
        //         .SetDstStage(PS_COMPUTE_SHADER)
        //         .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
        //         .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE);

        //     auto& barrier2 = barrier_info.texture_barriers[1];
        //     barrier2.SetTexture(_hiz_buffer.texture)
        //         .SetSubResourceRange(range)
        //         .SetSrcStage(PS_COMPUTE_SHADER)
        //         .SetDstStage(PS_COMPUTE_SHADER)
        //         .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
        //         .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);

        //     for (uint32_t i = 0; i < mip_count; ++i) {
        //         params.target = _hiz_buffer.uavs[i];
        //         if (i == 0) {

        //         } else {
        //             //set target mip
        //             config.size         = Vector2i(std::max(1, mip0_size.x >> (i)), std::max(1, mip0_size.y >> (i)));
        //             config.b_mip0       = false;
        //             config.target_level = i;

        //             params.depth_buffer = _hiz_buffer.srv;
        //         }
        //         barrier.sub_resource_range.mip_index  = i;
        //         barrier2.sub_resource_range.mip_index = i + 1;
        //         batched_params.SetParameters(builder_shader, params);

        //         g_rhi->RHISetBatchedShaderParameters(pso, batched_params);
        //         Vector3i group_count = Vector3i((config.size.t.x + 7) >> 3u, (config.size.t.y + 7) >> 3u, 1);
        //         graphics_cmd_list->Dispatch(group_count);
        //         if (i == mip_count - 1) {
        //             barrier_info
        //                 .texture_barriers[1]
        //                 .SetTexture(_depth_buffer->GetTexture())
        //                 .SetDstTextureLayout(TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE)
        //                 .SetSrcTextureLayout(TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        //                 .SetSubResourceRange(RHISubresourceRange(ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE))
        //                 .SetSrcStage(PS_COMPUTE_SHADER)
        //                 .SetDstStage(PS_EARLY_FRAGMENT_TESTS)
        //                 .SetDstAccessFlags(ERHIAccessFlags::DEPTH_STENCIL_WRITE | ERHIAccessFlags::DEPTH_STENCIL_READ)
        //                 .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ);
        //         }
        //         graphics_cmd_list->SetPipelineBarrier(barrier_info);//set current mip level to shader read only
        //     }
        // }

        void Dispatch(RenderContext& _context, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {

            auto& rg      = _context.GetRenderGraph();
            auto  mip_cnt = std::min(uint32_t(std::log2(std::min(_hiz_buffer.texture->GetExtent3D().x, _hiz_buffer.texture->GetExtent3D().y))), max_mip_levels);

            rg.AddComputePass(
                "Build HiZ", [&, depth_buffer(_depth_buffer), mip_cnt](RenderGraph::Builder& _builder) {
                auto& black_board = rg.GetBlackBoard();
                auto depth       = black_board.GetHandle("depth");

                auto hiz = black_board.GetHandle("HiZ Buffer");

                _builder.ReadTexture(depth, TS_SAMPLED);
                _builder.WriteTexture(hiz, TS_UNORDERED_WRITE, 0, mip_cnt); }, [this, mip_cnt, &_hiz_buffer, depth_buffer(_depth_buffer), &_context](RenderPassContext& _pass_context) {
                    BuildHiZShader::Parameters params{};
                    auto&                      hiz_tex   = _hiz_buffer.texture;
                    Vector2i                   mip0_size = Vector2i(hiz_tex->GetExtent3D());

                    HiZConfig& config    = params.config;
                    config.b_mip0        = true;
                    config.size          = mip0_size;
                    config.target_level  = 0;
                    params.target0       = _hiz_buffer.uavs[0];
                    params.target1       = _hiz_buffer.uavs[0];
                    params.target2       = _hiz_buffer.uavs[0];
                    params.target3       = _hiz_buffer.uavs[0];
                    params.depth_buffer  = depth_buffer;
                    params.depth_sampler = depth_sampler;
                    RHIBatchedShaderParameters batched_params;
                    auto&                      cmd_list = _context.GetCommandList();
                    cmd_list.SetPipelineState(psos[0]);

                    batched_params.SetParameters(builder_shaders[0], params);
                    g_rhi->RHISetBatchedShaderParameters(psos[0], batched_params);
                    cmd_list.Dispatch(Vector3i((config.size.t.x + 15) >> 4u, (config.size.t.y + 15) >> 4u, 1));

                    uint current_mip = 1;
                    cmd_list.SetPipelineState(psos[max_mip_batch_cnt - 1]);

                    for (; current_mip < mip_cnt - 4; current_mip += max_mip_batch_cnt) {
                        config.b_mip0       = false;
                        config.size         = Vector2i(std::max(1, mip0_size.x >> (current_mip - 1)), std::max(1, mip0_size.y >> (current_mip - 1)));
                        config.target_level = current_mip;
                        params.target0      = _hiz_buffer.uavs[current_mip];
                        params.target1      = _hiz_buffer.uavs[current_mip + 1];
                        params.target2      = _hiz_buffer.uavs[current_mip + 2];
                        params.target3      = _hiz_buffer.uavs[current_mip + 3];
                        params.depth_buffer = _hiz_buffer.srvs[current_mip - 1];

                        cmd_list.TransitionTexture(hiz_tex, TS_UNORDERED_READ, EPassType::Compute, current_mip - 1);
                        cmd_list.TransitionTexture(hiz_tex, TS_UNORDERED_WRITE, EPassType::Compute, current_mip, 4);
                        cmd_list.ExecuteTransition();

                        batched_params.SetParameters(builder_shaders[max_mip_batch_cnt - 1], params);
                        g_rhi->RHISetBatchedShaderParameters(psos[max_mip_batch_cnt - 1], batched_params);
                        cmd_list.Dispatch(Vector3i((config.size.t.x + 15) >> 4u, (config.size.t.y + 15) >> 4u, 1));
                    }

                    if (current_mip < mip_cnt) {
                        uint start_mip = current_mip;
                        uint batch_cnt = mip_cnt - start_mip;
                        cmd_list.SetPipelineState(psos[batch_cnt - 1]);
                        config.b_mip0       = false;
                        config.size         = Vector2i(std::max(1, mip0_size.x >> (start_mip - 1)), std::max(1, mip0_size.y >> (start_mip - 1)));
                        config.target_level = start_mip;
                        params.depth_buffer = _hiz_buffer.srvs[start_mip - 1];
                        params.target0      = _hiz_buffer.uavs[start_mip];
                        params.target1      = _hiz_buffer.uavs[std::min(mip_cnt - 1, start_mip + 1)];
                        params.target2      = _hiz_buffer.uavs[std::min(mip_cnt - 1, start_mip + 2)];

                        cmd_list.TransitionTexture(hiz_tex, TS_UNORDERED_READ, EPassType::Compute, start_mip - 1);
                        cmd_list.TransitionTexture(hiz_tex, TS_UNORDERED_WRITE, EPassType::Compute, start_mip, batch_cnt);
                        cmd_list.ExecuteTransition();
                        batched_params.SetParameters(builder_shaders[batch_cnt - 1], params);
                        g_rhi->RHISetBatchedShaderParameters(psos[batch_cnt - 1], batched_params);

                        cmd_list.Dispatch(Vector3i((config.size.t.x + 15) >> 4u, (config.size.t.y + 15) >> 4u, 1));
                    } });
        }

        RHIShaderRef                             builder_shader;
        StaticArray<RHIShaderRef, max_mip_batch_cnt> builder_shaders;

        // RHIComputePipelineStateRef                             pso;
        StaticArray<RHIComputePsoRef, max_mip_batch_cnt>       psos;
        RHISamplerRef                                          depth_sampler;
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
    // void HiZBuilder::DispatchBuildHiZ(RHIGraphicsCommandList* _cmd_list, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {
    //     impl->Dispatch(_cmd_list, _depth_buffer, _hiz_buffer);
    // }

    void HiZBuilder::DispatchBuildHiZ(RenderContext& _context, RHISRVRef _depth_buffer, HiZBuffer& _hiz_buffer) {
        impl->Dispatch(_context, _depth_buffer, _hiz_buffer);
    }
}// namespace Moer