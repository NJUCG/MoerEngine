#include "BasePass.h"
#include "PixelFormat.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "../Common.h"
#include "../Cull.h"
#include "RenderResourceDeferred.h"
#include "rhi/RHIResourceInitilizer.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "../utils/HiZBuilder.h"

namespace Moer {
    struct BasePass::Impl {
        friend class BasePass;

    public:
        Impl();
        ~Impl();

    private:
        void InitResources(RenderContext& _context);
        void UpdateSceneData(RenderContext& _context);
        void DrawFrame(RenderContext& _input);
        void PrePass(RenderContext& _context);
        void BuildHZB(RenderContext& _context);
        void PostPass(RenderContext& _context);
        void OnResizeViewport(Vector2i _extent);

    private:
        RHIGraphicsPipelineStateRef pipeline_state;
        RHIComputePipelineStateRef  cull_instance_recheck_pso;
        RHIComputePipelineStateRef  cull_meshlet_recheck_pso;

        RHIShaderRef cull_instance_recheck_shader;
        RHIShaderRef cull_meshlet_recheck_shader;

        RHIComputePipelineStateRef cull_instance_prepass_pso;
        RHIComputePipelineStateRef cull_meshlet_prepass_pso;

        RHIShaderRef cull_instance_prepass_shader;
        RHIShaderRef cull_meshlet_prepass_shader;

        RHITextureRef          hzb;
        RHISRVRef              hzb_srv;
        uint32_t               hzb_mip_count;
        Moer::Array<RHIUAVRef> hzb_uavs;

        HiZBuffer hiz_buffer;

        RHIBufferRef draw_indirect_buffer;
        RHIBufferRef counter_buffer;
        RHIBufferRef zero_buffer;
        RHIBufferRef instance_meshlet_cull_info_buffer;
        RHIBufferRef recheck_cull_info_buffer;
        RHIBufferRef recheck_instance_id_buffer;
        RHIBufferRef uniform_buffer;

        RHISRVRef meshlet_descs_buffer_view;
        RHISRVRef meshlet_bounds_buffer_view;
        RHISRVRef instance_buffer_view;
        RHISRVRef instance_meshlet_info_view;
        RHISRVRef instance_meshlet_cull_info_view;
        RHISRVRef recheck_instance_id_srv;
        RHISRVRef recheck_cull_info_view;

        Array<RHICBVRef> uniform_buffer_view;

        RHIUAVRef instance_meshlet_cull_info_uav;
        RHIUAVRef draw_indirect_view;
        RHIUAVRef draw_count_view;

        RHIUAVRef recheck_cull_info_uav;
        RHIUAVRef recheck_instance_id_uav;

        static constexpr uint32_t counter_buffer_size  = 32 * sizeof(uint32_t);
        static constexpr uint32_t meshlet_count_offset              = 0;
        static constexpr uint32_t draw_count_offset                 = 4;
        static constexpr uint32_t recheck_draw_count_offset         = 8;
        static constexpr uint32_t check_instance_count_offset       = 12;
        static constexpr uint32_t check_meshlet_count_offset        = 16;
        static constexpr uint32_t instance_dispatch_indirect_offset = 20;
        static constexpr uint32_t meshlet_dispatch_offset           = 24;
        static constexpr uint32_t recheck_meshlet_dispatch_offset   = 36;
        static constexpr uint32_t max_meshlet_count                 = 1024 * 1024 * 16;
        static constexpr uint32_t thread_group_count                = 64;
        static constexpr uint32_t uniform_buffer_size               = sizeof(VirtualView);
        RenderResourceDeferred* render_resources;
    };

    BasePass::Impl::Impl() {
    }

    BasePass::Impl::~Impl() {
    }

    void BasePass::Impl::InitResources(RenderContext& _context) {

        RHIVertexInputInfo vertex_input_info(

            VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 9 * sizeof(float), PF_R32G32B32_SFLOAT, 3, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 12 * sizeof(float), PF_R32G32_SFLOAT, 4, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
            VertexElement(1, 14 * sizeof(float), PF_R32_UINT, 5, sizeof(uint32_t), EVertexInputRate::VIR_INSTANCE));

        RHIShaderRef vertex_shader = ShaderResourceManager::GetInstance().GetShader<TestDeferredTriangleShaderVert>();
        RHIShaderRef frag_shader   = ShaderResourceManager::GetInstance().GetShader<TestDeferredTriangleShaderFrag>();

        RHIGraphicsPSOCreateInfo pso_info = RHIGraphicsPSOCreateInfo::Create()
                                                .SetColorAttachmentInfo(RHIColorAttachmentInfo::Preset<RHIConfig::Blend::ALPHA_BLEND>(PF_R8G8B8A8_SRGB))
                                                .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<RHIConfig::DepthStencil::DEPTH_WRITE_GREATER>())
                                                .SetDepthStencilFormat(EPixelFormat::PF_D32_SFLOAT_S8_UINT)
                                                .SetShaderStage(RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(vertex_input_info, vertex_shader, frag_shader))
                                                .Finalize();
        pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_info));

        cull_instance_prepass_shader = ShaderResourceManager::GetInstance().GetShader<CullInstancePrePassShader>();
        cull_meshlet_prepass_shader  = ShaderResourceManager::GetInstance().GetShader<CullMeshletPrepassShader>();

        cull_instance_prepass_pso = g_rhi->RHICreateComputePipelineState(cull_instance_prepass_shader);
        cull_meshlet_prepass_pso  = g_rhi->RHICreateComputePipelineState(cull_meshlet_prepass_shader);

        cull_instance_recheck_shader = ShaderResourceManager::GetInstance().GetShader<CullInstanceRecheckShader>();
        cull_meshlet_recheck_shader  = ShaderResourceManager::GetInstance().GetShader<CullMeshletRecheckShader>();

        cull_instance_recheck_pso = g_rhi->RHICreateComputePipelineState(cull_instance_recheck_shader);
        cull_meshlet_recheck_pso  = g_rhi->RHICreateComputePipelineState(cull_meshlet_recheck_shader);

        {
            //self resources
            instance_meshlet_cull_info_buffer = g_rhi->RHICreateBuffer<uint64_t>(max_meshlet_count, EBufferUsageFlags::UNORDERED_ACCESS);
            draw_indirect_buffer              = g_rhi->RHICreateBuffer<DrawInstanceCmd>(max_meshlet_count, EBufferUsageFlags::UNORDERED_ACCESS);
            counter_buffer                    = g_rhi->RHICreateBuffer<uint32_t>(counter_buffer_size, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST);
            zero_buffer                       = g_rhi->RHICreateBuffer<uint32_t>(counter_buffer_size, EBufferUsageFlags::CPU_VISIBLE);
            recheck_cull_info_buffer          = g_rhi->RHICreateBuffer<uint32_t>(max_meshlet_count, EBufferUsageFlags::UNORDERED_ACCESS);
            recheck_instance_id_buffer        = g_rhi->RHICreateBuffer<uint32_t>(max_meshlet_count, EBufferUsageFlags::UNORDERED_ACCESS);
            uniform_buffer                    = g_rhi->RHICreateBuffer<float>(uniform_buffer_size * _context.GetMaxFrameInFlight(), EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::CPU_VISIBLE);

            uniform_buffer_view.reserve(_context.GetMaxFrameInFlight());
            for (uint32_t i = 0; i < _context.GetMaxFrameInFlight(); ++i) {
                uniform_buffer_view.push_back(g_rhi->RHICreateCBV(uniform_buffer, uniform_buffer_size, i * uniform_buffer_size));
            }
            instance_meshlet_cull_info_view = g_rhi->RHICreateBufferSRV(instance_meshlet_cull_info_buffer);

            instance_meshlet_cull_info_uav = g_rhi->RHICreateBufferUAV(instance_meshlet_cull_info_buffer);
            draw_indirect_view             = g_rhi->RHICreateBufferUAV(draw_indirect_buffer);

            draw_count_view        = g_rhi->RHICreateBufferUAV(counter_buffer, sizeof(uint32_t), 4);
            recheck_cull_info_view = g_rhi->RHICreateBufferSRV(recheck_cull_info_buffer);
            recheck_cull_info_uav  = g_rhi->RHICreateBufferUAV(recheck_cull_info_buffer);

            recheck_instance_id_srv = g_rhi->RHICreateBufferSRV(recheck_instance_id_buffer);
            recheck_instance_id_uav = g_rhi->RHICreateBufferUAV(recheck_instance_id_buffer);
        }
    }

    void BasePass::Impl::UpdateSceneData(RenderContext& _context) {
        auto meshlet_info_buffer       = g_scene->GetBuffer("meshlet_info_buffer");
        auto meshlet_bound_buffer      = g_scene->GetBuffer("meshlet_bounds_buffer");
        auto instance_buffer           = g_scene->GetBuffer("instance_buffer");
        auto instance_mesh_info_buffer = g_scene->GetBuffer("instance_mesh_info_buffer");

        meshlet_descs_buffer_view  = g_rhi->RHICreateBufferSRV(meshlet_info_buffer);
        meshlet_bounds_buffer_view = g_rhi->RHICreateBufferSRV(meshlet_bound_buffer);
        instance_buffer_view       = g_rhi->RHICreateBufferSRV(instance_buffer);
        instance_meshlet_info_view = g_rhi->RHICreateBufferSRV(instance_mesh_info_buffer);
    }

    void BasePass::Impl::DrawFrame(RenderContext& _input) {
        auto& cmd_list = _input.GetCommandList();
        PrePass(_input);
    }

    void BasePass::Impl::PrePass(RenderContext& _context) {
        uint32_t frame_offset   = _context.GetFrameOffset();
        auto     instance_count = instance_buffer_view->GetBuffer()->GetNumElement() / sizeof(InstanceData);

        CullInstancePrePassShader::Parameters cull_instance_params;
        cull_instance_params.input.meshlet_count_offset          = meshlet_count_offset;
        cull_instance_params.input.instance_count                = instance_count;
        cull_instance_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
        cull_instance_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
        cull_instance_params.input.recheck_counter_buffer_offset = check_instance_count_offset;
        cull_instance_params.input.instance_dispatch_offset      = instance_dispatch_indirect_offset;
        cull_instance_params.input.meshlet_dispatch_offset       = meshlet_dispatch_offset;

        cull_instance_params.instance_meshlet_info      = instance_meshlet_info_view;
        cull_instance_params.instance_meshlet_cull_info = instance_meshlet_cull_info_uav;
        cull_instance_params.recheck_instance_id        = recheck_instance_id_uav;
        cull_instance_params.counters_buffer            = draw_count_view;
        cull_instance_params.views                      = uniform_buffer_view[frame_offset];
        cull_instance_params.hiz_depth                  = hiz_buffer.srv;
        cull_instance_params.depth_sampler              = hiz_buffer.sampler;

        CullMeshletPrepassShader::Parameters cull_meshlet_params;
        cull_meshlet_params.input.meshlet_count_offset          = meshlet_count_offset;
        cull_meshlet_params.input.draw_count_offset             = draw_count_offset;
        cull_meshlet_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
        cull_meshlet_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
        cull_meshlet_params.input.recheck_counter_buffer_offset = check_meshlet_count_offset;
        cull_meshlet_params.meshlet_info_buffer                 = meshlet_descs_buffer_view;
        cull_meshlet_params.meshlet_bound_buffer                = meshlet_bounds_buffer_view;
        cull_meshlet_params.instance_data                       = instance_buffer_view;
        cull_meshlet_params.instance_meshlet_info               = instance_meshlet_info_view;
        cull_meshlet_params.instance_meshlet_cull_info          = instance_meshlet_cull_info_view;
        cull_meshlet_params.recheck_cull_info                   = recheck_cull_info_uav;
        cull_meshlet_params.counters_buffer                     = draw_count_view;
        cull_meshlet_params.command_buffer                      = draw_indirect_view;
        cull_meshlet_params.views                               = uniform_buffer_view[frame_offset];
        cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
        cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

        RHIBatchedShaderParameters cull_instance_batched_params;
        cull_instance_batched_params.SetParameters(cull_instance_prepass_shader, cull_instance_params);

        RHIBatchedShaderParameters cull_meshlet_batched_params;
        cull_meshlet_batched_params.SetParameters(cull_meshlet_prepass_shader, cull_meshlet_params);
        auto prepass_cull_task = [this,
                                  instance_params(std::move(cull_instance_batched_params)),
                                  meshlet_params(std::move(cull_meshlet_batched_params)),
                                  instance_count(instance_count)](
                                     RenderPassContext& _context) mutable {
            RHIGraphicsCommandList* cmd_list = _context.cmd_list;

            RHIBarrierDependencyInfo barrier_dependency_info{};
            barrier_dependency_info.buffer_barriers.resize(2);
            auto& buffer_barrier_info = barrier_dependency_info.buffer_barriers[0];
            buffer_barrier_info
                .SetBuffer(counter_buffer)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
                .SetSrcStage(ERHIPipelineStageFlags::PS_TRANSFER)
                .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

            auto& buffer_barrier_info_1 = barrier_dependency_info.buffer_barriers[1];
            buffer_barrier_info_1
                .SetBuffer(draw_indirect_buffer)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                .SetSrcStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT)
                .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

            cmd_list->SetPipelineBarrier(barrier_dependency_info);

            cmd_list->SetPipelineState(cull_instance_prepass_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_instance_prepass_pso, instance_params);
            auto dispatch_count = (instance_count + thread_group_count - 1) / thread_group_count;
            cmd_list->Dispatch(dispatch_count, 1, 1);
            {
                RHIBarrierDependencyInfo instance_cull_barrier{};
                instance_cull_barrier.buffer_barriers.resize(3);
                auto& buffer_barrier_info = instance_cull_barrier.buffer_barriers[0];
                buffer_barrier_info
                    .SetBuffer(instance_meshlet_cull_info_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                auto& buffer_barrier_info_1 = instance_cull_barrier.buffer_barriers[1];
                buffer_barrier_info_1
                    .SetBuffer(recheck_instance_id_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                auto& buffer_barrier_info_2 = instance_cull_barrier.buffer_barriers[2];
                buffer_barrier_info_2
                    .SetBuffer(counter_buffer)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                    .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                cmd_list->SetPipelineBarrier(instance_cull_barrier);
            }

            cmd_list->SetPipelineState(cull_meshlet_prepass_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_meshlet_prepass_pso, meshlet_params);
            // cmd_list->Dispatch((max_meshlet_count + thread_group_count) / thread_group_count, 1, 1);
            cmd_list->DispatchIndirect(counter_buffer, meshlet_dispatch_offset);
            {
                RHIBarrierDependencyInfo post_compute_barrier{};
                post_compute_barrier.buffer_barriers.resize(3);
                auto& buffer_barrier_info_0 = post_compute_barrier.buffer_barriers[0];
                buffer_barrier_info_0
                    .SetBuffer(counter_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                auto& buffer_barrier_info_1 = post_compute_barrier.buffer_barriers[1];
                buffer_barrier_info_1
                    .SetBuffer(draw_indirect_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                auto& buffer_barrier_info_2 = post_compute_barrier.buffer_barriers[2];
                buffer_barrier_info_2
                    .SetBuffer(recheck_cull_info_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_READ)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                cmd_list->SetPipelineBarrier(post_compute_barrier);
            }
        };

        EnqueueRenderTask([&, prepass_cull(std::move(prepass_cull_task))]() {
            _context.GetRenderGraph().AddComputePass(
                "Prepass Cull Instance",
                [&](RenderGraph::Builder& _builder) {
                    // auto& rg  = GetCurrentRenderGraph();
                    // auto  hiz = rg.GetBlackBoard().GetHandle("hiz_buffer");
                    // if (!hiz.IsInitialized()) {
                    //     hiz = rg.ImportTexture("hiz_buffer", hiz_buffer.texture);
                    // }
                    // _builder.ReadTexture(hiz, ETextureUsageFlags::SAMPLED);
                },
                std::move(prepass_cull));
        });
    }

    void BasePass::Impl::BuildHZB(RenderContext& _context) {
        auto frame_offset = _context.GetFrameOffset();
        // HiZBuilder::GetInstance().DispatchBuildHiZ(&_context.GetCommandList(), depth_buffer_srv[frame_offset], hiz_buffer);
    }

    void BasePass::Impl::PostPass(RenderContext& _context) {

        uint32_t frame_offset   = _context.GetFrameOffset();
        auto     instance_count = instance_buffer_view->GetBuffer()->GetNumElement();

        CullInstanceRecheckShader::Parameters cull_instance_params;
        cull_instance_params.input.meshlet_count_offset          = check_meshlet_count_offset;
        cull_instance_params.input.instance_count                = instance_count;
        cull_instance_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
        cull_instance_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
        cull_instance_params.input.recheck_counter_buffer_offset = check_instance_count_offset;
        cull_instance_params.input.instance_dispatch_offset      = instance_dispatch_indirect_offset;
        cull_instance_params.input.meshlet_dispatch_offset       = recheck_meshlet_dispatch_offset;
        // cull_instance_params.instance_data                       = instance_buffer_view;
        cull_instance_params.instance_meshlet_info      = instance_meshlet_info_view;
        cull_instance_params.instance_meshlet_cull_info = recheck_cull_info_uav;
        cull_instance_params.recheck_instances          = recheck_instance_id_srv;
        cull_instance_params.counters_buffer            = draw_count_view;
        cull_instance_params.views                      = uniform_buffer_view[frame_offset];
        cull_instance_params.hiz_depth                  = hiz_buffer.srv;
        cull_instance_params.depth_sampler              = hiz_buffer.sampler;

        CullMeshletRecheckShader::Parameters cull_meshlet_params;
        cull_meshlet_params.input.meshlet_count_offset          = meshlet_count_offset;
        cull_meshlet_params.input.draw_count_offset             = recheck_draw_count_offset;
        cull_meshlet_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
        cull_meshlet_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
        cull_meshlet_params.input.recheck_counter_buffer_offset = check_meshlet_count_offset;
        cull_meshlet_params.meshlet_info_buffer                 = meshlet_descs_buffer_view;
        cull_meshlet_params.meshlet_bound_buffer                = meshlet_bounds_buffer_view;
        cull_meshlet_params.instance_data                       = instance_buffer_view;
        cull_meshlet_params.instance_meshlet_info               = instance_meshlet_info_view;
        cull_meshlet_params.instance_meshlet_cull_info          = recheck_cull_info_view;
        cull_meshlet_params.counters_buffer                     = draw_count_view;
        cull_meshlet_params.command_buffer                      = draw_indirect_view;
        cull_meshlet_params.views                               = uniform_buffer_view[frame_offset];
        cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
        cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

        RHIBatchedShaderParameters cull_instance_batched_params;
        cull_instance_batched_params.SetParameters(cull_instance_recheck_shader, cull_instance_params);

        RHIBatchedShaderParameters cull_meshlet_batched_params;
        cull_meshlet_batched_params.SetParameters(cull_meshlet_recheck_shader, cull_meshlet_params);

        auto recheck_pass_cull_task = [this,
                                       instance_params(std::move(cull_instance_batched_params)),
                                       meshlet_params(std::move(cull_meshlet_batched_params)),
                                       instance_count(instance_count),
                                       &_context](
                                          RenderPassContext& _pass_context) mutable {
            auto& cmd_list = _context.GetCommandList();

            RHIBarrierDependencyInfo barrier_dependency_info{};
            barrier_dependency_info.buffer_barriers.resize(3);
            auto& buffer_barrier_info = barrier_dependency_info.buffer_barriers[0];
            buffer_barrier_info
                .SetBuffer(counter_buffer)
                .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                .SetSrcStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT)
                .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

            auto& buffer_barrier_info_1 = barrier_dependency_info.buffer_barriers[1];
            buffer_barrier_info_1
                .SetBuffer(draw_indirect_buffer)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                .SetSrcAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                .SetSrcStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT)
                .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

            auto& buffer_barrier_info_2 = barrier_dependency_info.buffer_barriers[2];
            buffer_barrier_info_2
                .SetBuffer(recheck_instance_id_buffer)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

            cmd_list.SetPipelineBarrier(barrier_dependency_info);

            cmd_list.SetPipelineState(cull_instance_recheck_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_instance_recheck_pso, instance_params);
            auto dispatch_count = (instance_count + thread_group_count - 1) / thread_group_count;
            // cmd_list->Dispatch(dispatch_count, 1, 1);
            cmd_list.DispatchIndirect(counter_buffer, instance_dispatch_indirect_offset);
            {
                RHIBarrierDependencyInfo meshlet_cull_barrier{};
                meshlet_cull_barrier.buffer_barriers.resize(3);
                auto& buffer_barrier_info = meshlet_cull_barrier.buffer_barriers[0];
                buffer_barrier_info
                    .SetBuffer(instance_meshlet_cull_info_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)

                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                auto& buffer_barrier_info_1 = meshlet_cull_barrier.buffer_barriers[1];
                buffer_barrier_info_1
                    .SetBuffer(recheck_cull_info_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                auto& buffer_barrier_info_2 = meshlet_cull_barrier.buffer_barriers[2];
                buffer_barrier_info_2
                    .SetBuffer(counter_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                cmd_list.SetPipelineBarrier(meshlet_cull_barrier);
            }

            cmd_list.SetPipelineState(cull_meshlet_recheck_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_meshlet_recheck_pso, meshlet_params);

            // cmd_list->Dispatch((max_meshlet_count + thread_group_count) / thread_group_count, 1, 1);
            cmd_list.DispatchIndirect(counter_buffer, recheck_meshlet_dispatch_offset);
            {
                RHIBarrierDependencyInfo post_compute_barrier{};
                post_compute_barrier.buffer_barriers.resize(2);
                auto& buffer_barrier_info_0 = post_compute_barrier.buffer_barriers[0];
                buffer_barrier_info_0
                    .SetBuffer(counter_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                auto& buffer_barrier_info_1 = post_compute_barrier.buffer_barriers[1];
                buffer_barrier_info_1
                    .SetBuffer(draw_indirect_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT);

                cmd_list.SetPipelineBarrier(post_compute_barrier);
            }
        };

        EnqueueRenderTask([&, recheck_pass(std::move(recheck_pass_cull_task))]() {
            _context.GetRenderGraph().AddComputePass(
                "Recheck Cull Instance",
                [&](RenderGraph::Builder& _builder) {
                },
                std::move(recheck_pass));
        });
    }

    void BasePass::Impl::OnResizeViewport(Vector2i extent) {
    }

    BasePass::BasePass() : impl(MoerNew(Impl)) {
    }

    BasePass::~BasePass() {
        impl.reset();
    }

    void BasePass::InitResources(RenderContext& _context) {
        impl->InitResources(_context);
    }

    void BasePass::UpdateSceneData(RenderContext& _context) {
        impl->UpdateSceneData(_context);
    }

    void BasePass::Draw(RenderContext& _input) {
        impl->DrawFrame(_input);
    }

    void BasePass::OnResizeViewport(Vector2i _extent) {
        impl->OnResizeViewport(_extent);
    }

}// namespace Moer