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

namespace Moer {
    struct BasePass::Impl {

    public:
        Impl();
        ~Impl();

    private:
        void InitResources(RenderContext& _context);
        void DrawFrame(RenderContext& _input);
        void PrePass(RenderContext& _context);
        void BuildHZB();
        void PostPass();
        void OnResizeViewport(Vector2i _extent);

    private:
        RHIGraphicsPipelineStateRef pipeline_state;
        RHIComputePipelineStateRef  cull_instance_pso;
        RHIComputePipelineStateRef  cull_meshlet_pso;

        RHITextureRef          hzb;
        RHISRVRef              hzb_srv;
        uint32_t               hzb_mip_count;
        Moer::Array<RHIUAVRef> hzb_uavs;

        RHIBufferRef draw_indirect_buffer;
        RHIBufferRef counter_buffer;
        RHIBufferRef zero_buffer;
        RHIBufferRef instance_meshlet_cull_info_buffer;
        RHIBufferRef uniform_buffer;

        RHISRVRef meshlet_descs_buffer_view;
        RHISRVRef meshlet_bounds_buffer_view;
        RHISRVRef instance_buffer_view;
        RHISRVRef instance_meshlet_info_view;
        RHISRVRef instance_meshlet_cull_info_view;

        RHIUAVRef instance_meshlet_cull_info_uav;
        RHIUAVRef draw_indirect_view;
        RHIUAVRef draw_count_view;

        static constexpr uint32_t meshlet_count_offset = 0;
        static constexpr uint32_t draw_count_offset    = 4;
        static constexpr uint32_t counter_buffer_size  = 32 * sizeof(uint32_t);
        static constexpr uint32_t max_meshlet_count    = 1024 * 1024 * 16;
        static constexpr uint32_t thread_group_count   = 64;

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

        RHIShaderRef cull_instance_shader = ShaderResourceManager::GetInstance().GetShader<CullInstancePrePassShader>();
        RHIShaderRef cull_meshlet_shader  = ShaderResourceManager::GetInstance().GetShader<CullMeshletPrepassShader>();

        cull_instance_pso = g_rhi->RHICreateComputePipelineState(cull_instance_shader);
        cull_meshlet_pso  = g_rhi->RHICreateComputePipelineState(cull_meshlet_shader);

        auto meshlet_info_buffer       = g_scene->GetBuffer("meshlet_info_buffer");
        auto meshlet_bound_buffer      = g_scene->GetBuffer("meshlet_bounds_buffer");
        auto instance_buffer           = g_scene->GetBuffer("instance_buffer");
        auto instance_mesh_info_buffer = g_scene->GetBuffer("instance_mesh_info_buffer");

        meshlet_descs_buffer_view  = g_rhi->RHICreateBufferSRV(meshlet_info_buffer);
        meshlet_bounds_buffer_view = g_rhi->RHICreateBufferSRV(meshlet_bound_buffer);
        instance_buffer_view       = g_rhi->RHICreateBufferSRV(instance_buffer);
        instance_meshlet_info_view = g_rhi->RHICreateBufferSRV(instance_mesh_info_buffer);

        {
            //self resources
            instance_meshlet_cull_info_buffer = g_rhi->RHICreateBuffer<uint64_t>(max_meshlet_count, EBufferUsageFlags::STORAGE_BUFFER);
            draw_indirect_buffer              = g_rhi->RHICreateBuffer<DrawInstanceCmd>(max_meshlet_count, EBufferUsageFlags::STORAGE_BUFFER);
            counter_buffer                    = g_rhi->RHICreateBuffer<uint32_t>(counter_buffer_size, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST);
            zero_buffer                       = g_rhi->RHICreateBuffer<uint32_t>(counter_buffer_size, EBufferUsageFlags::CPU_VISIBLE);

            instance_meshlet_cull_info_view = g_rhi->RHICreateBufferSRV(instance_meshlet_cull_info_buffer);

            instance_meshlet_cull_info_uav = g_rhi->RHICreateBufferUAV(instance_meshlet_cull_info_buffer);
            draw_indirect_view             = g_rhi->RHICreateBufferUAV(draw_indirect_buffer);
        }
    }

    void BasePass::Impl::DrawFrame(RenderContext& _input) {
        auto& cmd_list = _input.GetCommandList();
        PrePass(_input);
    }

    void BasePass::Impl::PrePass(RenderContext& _context) {
    }

    void BasePass::Impl::BuildHZB() {
    }

    void BasePass::Impl::PostPass() {
    }

    void BasePass::Impl::OnResizeViewport(Vector2i extent) {
    }

    BasePass::BasePass() : impl(MoerNew(Impl)) {
    }

    BasePass::~BasePass() {
    }

}// namespace Moer