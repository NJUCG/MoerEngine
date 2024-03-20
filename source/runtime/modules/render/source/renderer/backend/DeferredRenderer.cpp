#include "DeferredRenderer.h"
#include "PixelFormat.h"
#include "RenderThread.h"
#include "log/LogSystem.h"
#include "math/Base.h"
#include "math/Function.h"
#include "math/Matrix.h"
#include "misc/MMemory.h"
#include "misc/STL.h"
#include "resources/AsyncResources.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/RHICommand.h"
#include "scene/CameraManager.h"
#include "scene/RenderableManager.h"
#include "scene/TransformManager.h"
#include "shader/Shader.h"
#include "shader/ShaderMutation.h"
#include "shader/ShaderParameterMacros.h"
#include "shader/ShaderResourceManager.h"
#include "scene/Scene.h"
#include "deferred/BasePass.h"
#include "utils/HiZBuilder.h"

#include <algorithm>
#include "Cull.h"
#include "rendergraph/RenderGraph.h"
#include "resources/GpuScene.h"
#include "scene/Material.h"
IMPLEMENT_SHADER_TYPE(CullInstancePrePassShader, "meshdebug/CullInstance.hlsl", "prepass", ST_COMPUTE)
IMPLEMENT_SHADER_TYPE(CullMeshletPrepassShader, "meshdebug/CullMeshlet.hlsl", "prepass", ST_COMPUTE)

IMPLEMENT_SHADER_TYPE(CullInstanceRecheckShader, "meshdebug/CullInstance.hlsl", "recheck_pass", ST_COMPUTE)
IMPLEMENT_SHADER_TYPE(CullMeshletRecheckShader, "meshdebug/CullMeshlet.hlsl", "recheck_pass", ST_COMPUTE)

IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderVert, "test/TriangleDeferredVert.hlsl", "main", ST_VERTEX);
IMPLEMENT_SHADER_TYPE(TestDeferredTriangleShaderFrag, "test/TriangleDeferredFrag.hlsl", "main", ST_FRAGMENT);

BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(LightingData)
DEFINE_SHADER_PARAM(Moer::Matrix4x4f, inv_view_proj)
DEFINE_SHADER_PARAM(uint32_t, light_count)
DEFINE_SHADER_PARAM(Moer::Vector3ui, padding)
END_SHADER_CONSTANT_STRUCT_DEFINITION()

BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(LightData)
DEFINE_SHADER_PARAM(Moer::Vector4f, color)
DEFINE_SHADER_PARAM(Moer::Vector4f, position)
DEFINE_SHADER_PARAM(Moer::Vector4f, direction)
DEFINE_SHADER_PARAM(Moer::Vector4f, info)
END_SHADER_CONSTANT_STRUCT_DEFINITION()

class TestGBufferShaderVert : public Shader {
    DEFINE_SHADER_TYPE(TestGBufferShaderVert, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

class TestGBufferShaderFrag : public Shader {
    DEFINE_SHADER_TYPE(TestGBufferShaderFrag, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
    // DEFINE_SHADER_PARAM_SAMPLER(SamplerState, defaultSampler)
    // DEFINE_SHADER_PARAM_SRV(Texture2D, baseColorMap)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(TestGBufferShaderVert, "deferedshading/GBufferVert.hlsl", "main", ST_VERTEX);
IMPLEMENT_SHADER_TYPE(TestGBufferShaderFrag, "deferedshading/GBufferFrag.hlsl", "main", ST_FRAGMENT);

class LightingShaderVert : public Shader {
    DEFINE_SHADER_TYPE(LightingShaderVert, Global, RENDER_API, ...)
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

class LightingShaderFrag : public Shader {
    DEFINE_SHADER_TYPE(LightingShaderFrag, Global, RENDER_API, ...)
public:
    BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
    DEFINE_SHADER_PARAM_STRUCT(LightingData, lighting_data)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MaterialData>, material_data)
    DEFINE_SHADER_PARAM_SRV(StructuredBuffer<Light>, light_data)
    DEFINE_SHADER_PARAM_SRV_ARRAY(Texture2D, scene_textures, 25)
    DEFINE_SHADER_PARAM_SRV(Texture2D, mat_attach)
    DEFINE_SHADER_PARAM_SRV(Texture2D, normal_attach)
    DEFINE_SHADER_PARAM_SRV(Texture2D, gbuffer_uv)
    DEFINE_SHADER_PARAM_SRV(Texture2D, depth_attach)
    DEFINE_SHADER_PARAM_SAMPLER(SamplerState, default_sampler)
    END_ROOT_PARAMETER_DEFINITION(Parameters)
};

IMPLEMENT_SHADER_TYPE(LightingShaderVert, "deferedshading/LightingVert.hlsl", "main", ST_VERTEX);
IMPLEMENT_SHADER_TYPE(LightingShaderFrag, "deferedshading/LightingFrag.hlsl", "main", ST_FRAGMENT);

namespace Moer {
    class DeferredRenderer::Impl {
    public:
        void Init(const BackendRendererInitInfo& _init_info);
        void ShutDown();
        void DrawFrame();
        void Present();
        void SetOriginResolution(uint32_t _width, uint32_t _height);
        void SetPresentResolution(uint32_t _width, uint32_t _height);

        RHISRVRef GetRendererOutput();

    private:
        void BasePass();
        void CreateDepthBuffer();
        void OnResizeVSwapChain();

        void PrePass();
        void RecheckPass();
        void LightingPass();

        void DispatchDrawScene(const CameraData& _camera_data, bool _first_pass = true);

    private:
        VirtualViewport* virtual_viewport;
        uint64_t         frame_counter = 0;

        Moer::Array<RHIGraphicsCommandList*> render_cmd_lists;
        RHICommandQueue*                     render_queue;
        RHIFenceRef                          render_fence;

        // UniquePtr<BasePass> base_pass;

        //test triangle data
        // RHIBufferRef vertex_buffer;
        // RHIBufferRef index_buffer;

        RHIGraphicsPipelineStateRef pipeline_state;
        RHIGraphicsPipelineStateRef gbuffer_pipeline_state;
        RHIGraphicsPipelineStateRef lighting_pipeline_state;

        RHIBufferRef           light_buffer;
        RHISRVRef              light_buffer_view;
        Moer::Array<LightData> lights;

        RHIComputePipelineStateRef cull_instance_recheck_pso;
        RHIComputePipelineStateRef cull_meshlet_recheck_pso;

        RHIShaderRef cull_instance_recheck_shader;
        RHIShaderRef cull_meshlet_recheck_shader;

        RHIComputePipelineStateRef cull_instance_prepass_pso;
        RHIComputePipelineStateRef cull_meshlet_prepass_pso;

        RHIShaderRef cull_instance_prepass_shader;
        RHIShaderRef cull_meshlet_prepass_shader;

        Moer::Array<RHITextureRef> depth_buffer;
        Moer::Array<RHIUAVRef>     depth_buffer_uav;
        Moer::Array<RHISRVRef>     depth_buffer_srv;

        HiZBuffer hiz_buffer;

        RHIBufferRef draw_indirect_buffer;
        RHIBufferRef draw_count_buffer;
        RHIBufferRef zero_buffer;
        RHIBufferRef instance_meshlet_cull_info_buffer;
        RHIBufferRef recheck_cull_info_buffer;
        RHIBufferRef recheck_instance_id_buffer;
        RHIBufferRef uniform_buffer;

        Array<RHICBVRef> uniform_buffer_view;

        RHISRVRef meshlet_descs_buffer_view;
        RHISRVRef meshlet_bounds_buffer_view;
        RHISRVRef instance_buffer_view;
        RHISRVRef instance_meshlet_info_view;
        RHISRVRef instance_meshlet_cull_info_view;
        RHISRVRef recheck_cull_info_view;
        RHISRVRef recheck_instance_id_srv;

        RHIUAVRef instance_meshlet_cull_info_uav;
        RHIUAVRef recheck_cull_info_uav;
        RHIUAVRef recheck_instance_id_uav;

        RHIUAVRef draw_indirect_view;
        RHIUAVRef draw_count_view;

        static constexpr uint32_t meshlet_count_offset      = 0;
        static constexpr uint32_t draw_count_offset         = 4;
        static constexpr uint32_t recheck_draw_count_offset = 8;
        // static constexpr uint32_t meshlet_dispatch_indirect_offset  = 8;
        static constexpr uint32_t check_instance_count_offset       = 12;
        static constexpr uint32_t check_meshlet_count_offset        = 16;
        static constexpr uint32_t instance_dispatch_indirect_offset = 20;
        static constexpr uint32_t meshlet_dispatch_offset           = 24;
        static constexpr uint32_t recheck_meshlet_dispatch_offset   = 36;
        static constexpr uint32_t max_meshlet_count                 = 1024 * 1024 * 16;
        static constexpr uint32_t thread_group_count                = 64;

        Vector2i source_resolution;
    };
    void DeferredRenderer::Init(const BackendRendererInitInfo& _init_info) {
        impl = MoerNew(Impl);
        impl->Init(_init_info);
    }

    void DeferredRenderer::ShutDown() {
        impl->ShutDown();
    }

    void DeferredRenderer::DrawFrame() {
        impl->DrawFrame();
    }

    void DeferredRenderer::Present() {
        impl->Present();
    }

    void DeferredRenderer::SetOriginResolution(uint32_t _width, uint32_t _height) {
        impl->SetOriginResolution(_width, _height);
    }

    void DeferredRenderer::SetPresentResolution(uint32_t _width, uint32_t _height) {
        impl->SetPresentResolution(_width, _height);
    }

    void* DeferredRenderer::GetRendererOutput() {
        return impl->GetRendererOutput();
    }

    void DeferredRenderer::Impl::CreateDepthBuffer() {
        //depth buffer
        auto back_buffer_cnt   = virtual_viewport->GetInfo().back_buffer_count;
        auto depth_create_info = RHITextureCreateInfo::Create("deferred_depth", ETextureDimension::TEX_2D)
                                     .SetExtent(source_resolution)
                                     .SetDepth(1)
                                     .SetFormat(EPixelFormat::PF_D32_SFLOAT_S8_UINT)
                                     .SetClearAttachment(RHIClearAttachment(EClearAttachment::DEPTH_STENCIL))
                                     .SetUsageFlags(ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT | ETextureUsageFlags::SAMPLED);
        depth_buffer.resize(back_buffer_cnt);
        depth_buffer_srv.resize(back_buffer_cnt);
        depth_buffer_uav.resize(back_buffer_cnt);
        for (uint32_t i = 0; i < back_buffer_cnt; ++i) {

            depth_buffer[i]     = g_rhi->RHICreateTexture(depth_create_info);
            depth_buffer_uav[i] = g_rhi->RHICreateTextureUAV(depth_buffer[i]);
            depth_buffer_srv[i] = g_rhi->RHICreateTextureSRV(depth_buffer[i]);
        }

        hiz_buffer.InitFromDepthExtent(depth_buffer[0]->GetExtent2D());

        RHIBarrierDependencyInfo barrier_info{};
        barrier_info.texture_barriers.resize(back_buffer_cnt);

        RHISubresourceRange range(ETextureAspectFlags::DEPTH_SLICE | ETextureAspectFlags::STENCIL_SLICE);

        for (uint32_t i = 0; i < back_buffer_cnt; ++i) {
            auto& barrier = barrier_info.texture_barriers[i];
            barrier
                .SetTexture(depth_buffer[i])
                .SetDstTextureLayout(ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE)
                .SetSrcTextureLayout(ETextureLayout::TEXTURE_LAYOUT_UNDEFINED)
                .SetSubResourceRange(range)
                .SetDstStage(PS_EARLY_FRAGMENT_TESTS)
                .SetSrcAccessFlags(ERHIAccessFlags::UNDEFINED)
                .SetDstAccessFlags(ERHIAccessFlags::DEPTH_STENCIL_WRITE);
        }
        auto* cmd_list = render_cmd_lists[0];
        cmd_list->BeginRecording();
        cmd_list->SetPipelineBarrier(barrier_info);
        cmd_list->EndRecording();
        RHISubmitInfo submit_info{};
        RHIFenceRef   fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::TIMELINE});
        submit_info.Signal(fence, 1);
        render_queue->SubmitCommands(1, render_cmd_lists[0], &submit_info);
        fence->Wait(1);
    }

    void DeferredRenderer::Impl::OnResizeVSwapChain() {
        EnqueueRenderTask([res(this->source_resolution),
                           view_port(this->virtual_viewport),
                           &depth_buffers(this->depth_buffer),
                           &depth_buffer_views(this->depth_buffer_uav),
                           this]() {
            view_port->OnResize(Extent2D(res.x, res.y));

            CreateDepthBuffer();
        });
    }

    void DeferredRenderer::Impl::Init(const BackendRendererInitInfo& _init_info) {
        //wait for g_scene
        RenderThreadFence render_thread_fence;
        render_thread_fence.BeginFence();
        render_thread_fence.Wait();
        VirtualViewportCreateInfo create_info;
        source_resolution             = Vector2i(_init_info.width, _init_info.height);
        create_info.name              = "DeferredRendererViewport";
        create_info.extent            = source_resolution;
        create_info.format            = _init_info.format;
        create_info.back_buffer_count = 3;
        virtual_viewport              = MoerNew(VirtualViewport)(create_info);
        render_queue                  = g_rhi->RHICreateCommandQueue(ECommandQueueType::GRAPHICS);

        render_cmd_lists.resize(create_info.back_buffer_count);
        for (uint32_t i = 0; i < create_info.back_buffer_count; ++i) {
            render_cmd_lists[i] = g_rhi->RHICreateGraphicsCommandList(g_rhi->RHIGetCurrentCommandAllocator());
        }
        render_fence = g_rhi->RHICreateFence({.usage = EFenceUsageFlags::TIMELINE});

        RHIVertexInputInfo vertex_input_info(

            VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 9 * sizeof(float), PF_R32G32_SFLOAT, 3, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(1, 0 * sizeof(float), PF_R32_UINT, 4, sizeof(uint32_t), EVertexInputRate::VIR_INSTANCE));
        // RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);

        auto& shader_resource_manager = ShaderResourceManager::GetInstance();

        RHIShaderRef vertex_shader   = shader_resource_manager.GetShader<TestDeferredTriangleShaderVert>();
        RHIShaderRef fragment_shader = shader_resource_manager.GetShader<TestDeferredTriangleShaderFrag>();
        // RHIShaderBoundStateInput& shader_stage_input = init.shader_stage;

        RHIGraphicsShaderInputInfo shader_input_info =
            RHIGraphicsShaderInputInfo::Create()
                .SetVertexWorkFlow(std::move(vertex_input_info),
                                   vertex_shader,
                                   fragment_shader);

        RHIGraphicsPSOCreateInfo pso_create_info =
            RHIGraphicsPSOCreateInfo::Create()
                .SetShaderStage(
                    std::move(shader_input_info))
                .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<RHIConfig::DepthStencil::DEPTH_WRITE_GREATER>())
                .SetColorAttachmentInfo(
                    {std::move(RHIColorAttachmentInfo::Preset<RHIConfig::Blend::ALPHA_BLEND>(EPixelFormat::PF_R8G8B8A8_SRGB))})
                .SetDepthStencilFormat(PF_D32_SFLOAT_S8_UINT)
                .Finalize();

        pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));

        cull_instance_recheck_shader = shader_resource_manager.GetShader<CullInstanceRecheckShader>();
        cull_meshlet_recheck_shader  = shader_resource_manager.GetShader<CullMeshletRecheckShader>();

        cull_instance_recheck_pso = g_rhi->RHICreateComputePipelineState(cull_instance_recheck_shader);
        cull_meshlet_recheck_pso  = g_rhi->RHICreateComputePipelineState(cull_meshlet_recheck_shader);

        cull_instance_prepass_shader = shader_resource_manager.GetShader<CullInstancePrePassShader>();
        cull_meshlet_prepass_shader  = shader_resource_manager.GetShader<CullMeshletPrepassShader>();
        cull_instance_prepass_pso    = g_rhi->RHICreateComputePipelineState(cull_instance_prepass_shader);
        cull_meshlet_prepass_pso     = g_rhi->RHICreateComputePipelineState(cull_meshlet_prepass_shader);
        //why not implement a counter buffer?
        {
            RHIBufferCreateInfo buffer_create_info;
            uniform_buffer = g_rhi->RHICreateBuffer<float>(sizeof(CameraCullData) * 3, EBufferUsageFlags::UNIFORM_BUFFER | EBufferUsageFlags::CPU_VISIBLE);

            for (int i = 0; i < 3; i++) {
                uniform_buffer_view.push_back(
                    g_rhi->RHICreateCBV(uniform_buffer, sizeof(CameraCullData), sizeof(CameraCullData) * i));
            }

            draw_indirect_buffer = g_rhi->RHICreateBuffer<DrawInstanceCmd>(
                1024 * 1024,
                EBufferUsageFlags::INDIRECT_BUFFER | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::STORAGE_BUFFER);

            draw_count_buffer = g_rhi->RHICreateBuffer<uint32_t>(64 * sizeof(int),
                                                                 EBufferUsageFlags::TRANSFER_DST |
                                                                     EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::INDIRECT_BUFFER);

            zero_buffer = g_rhi->RHICreateBuffer<uint32_t>(64 * sizeof(int),
                                                           EBufferUsageFlags::CPU_VISIBLE | EBufferUsageFlags::TRANSFER_SRC);

            void* mapped = g_rhi->RHIMapBuffer(zero_buffer, 0, sizeof(uint32_t));

            std::array<uint32_t, 32> zero_data{};
            std::copy(zero_data.begin(), zero_data.end(), static_cast<uint32_t*>(mapped));

            g_rhi->RHIUnmapBuffer(zero_buffer);

            draw_indirect_view =
                g_rhi->RHICreateBufferUAV(draw_indirect_buffer);

            draw_count_view =
                g_rhi->RHICreateBufferUAV(draw_count_buffer);

            auto meshlet_descs = g_scene->GetBuffer("meshlet_descs");

            meshlet_descs_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("meshlet_descs"));

            meshlet_bounds_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("meshlet_bounds"));

            instance_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("instance_data"));

            instance_meshlet_info_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("instance_meshlet_info_buffer"));

            instance_meshlet_cull_info_buffer = g_rhi->RHICreateBuffer<uint64_t>(
                1024 * 512 * sizeof(uint64_t),
                EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::UNORDERED_ACCESS);

            recheck_cull_info_buffer = g_rhi->RHICreateBuffer<uint64_t>(
                1024 * 512 * sizeof(uint64_t),
                EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::UNORDERED_ACCESS);
            recheck_instance_id_buffer = g_rhi->RHICreateBuffer<uint32_t>(
                64 * 512 * sizeof(uint32_t),
                EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::UNORDERED_ACCESS);
            instance_meshlet_cull_info_view = g_rhi->RHICreateBufferSRV(instance_meshlet_cull_info_buffer);

            instance_meshlet_cull_info_uav = g_rhi->RHICreateBufferUAV(instance_meshlet_cull_info_buffer);

            recheck_cull_info_view = g_rhi->RHICreateBufferSRV(recheck_cull_info_buffer);
            recheck_cull_info_uav  = g_rhi->RHICreateBufferUAV(recheck_cull_info_buffer);

            recheck_instance_id_srv = g_rhi->RHICreateBufferSRV(recheck_instance_id_buffer);
            recheck_instance_id_uav = g_rhi->RHICreateBufferUAV(recheck_instance_id_buffer);
        }
        {
            CreateDepthBuffer();
        }

        {
            RHIShaderRef gbuffer_vert_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderVert>();
            RHIShaderRef gbuffer_frag_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderFrag>();

            RHIVertexInputInfo vertex_input_info(

                VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
                VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
                VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
                VertexElement(0, 9 * sizeof(float), PF_R32G32_SFLOAT, 3, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
                VertexElement(1, 0 * sizeof(float), PF_R32_UINT, 4, sizeof(uint32_t), EVertexInputRate::VIR_INSTANCE));
            // RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);

            RHIGraphicsShaderInputInfo gbuffer_shader_input_info = RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(vertex_input_info, gbuffer_vert_shader, gbuffer_frag_shader);
            RHIGraphicsPSOCreateInfo   pso_create_info =
                RHIGraphicsPSOCreateInfo::Create()
                    .SetShaderStage(
                        std::move(gbuffer_shader_input_info))
                    .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<RHIConfig::DepthStencil::DEPTH_WRITE_GREATER>())
                    .SetColorAttachmentInfo(
                        {std::move(RHIColorAttachmentInfo::Preset<RHIConfig::Blend::ALPHA_BLEND>(EPixelFormat::PF_R8G8B8A8_SRGB))})
                    .SetDepthStencilFormat(PF_D32_SFLOAT_S8_UINT)
                    .Finalize();

            //ToDo: this may be can read and cached from render pass
            pso_create_info.color_attachment_count = 3;
            auto& color_attachments_info           = pso_create_info.color_attachments_info;
            color_attachments_info[0]              = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R32_UINT);
            // color_attachments_info[1]              = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_UNORM);
            color_attachments_info[1] = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_UNORM);
            color_attachments_info[2] = RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R16G16_SFLOAT);

            gbuffer_pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));

            RHIShaderRef               lighting_vert_shader       = ShaderResourceManager::GetInstance().GetShader<LightingShaderVert>();
            RHIShaderRef               lighting_frag_shader       = ShaderResourceManager::GetInstance().GetShader<LightingShaderFrag>();
            RHIVertexInputInfo         lighting_vertex_input_info = {};
            RHIGraphicsShaderInputInfo lighting_shader_input_info = RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(lighting_vertex_input_info, lighting_vert_shader, lighting_frag_shader);
            RHIGraphicsPSOCreateInfo   lighting_pso_create_info =
                RHIGraphicsPSOCreateInfo::Create()
                    .SetShaderStage(
                        std::move(lighting_shader_input_info))
                    .SetRasterizerInfo({
                        .cull_mode = RCM_NONE,
                    })
                    .Finalize();
            lighting_pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(lighting_pso_create_info));

            instance_buffer_view = g_rhi->RHICreateBufferSRV(g_scene->GetBuffer("instance_data"));

            //HarCode lights
            auto light_pos   = Moer::Vector3f(0.0f, 128.0f, -225.0f);
            auto light_color = Moer::Vector3f(1.0, 1.0, 1.0);

            // Magic numbers used to offset lights in the Sponza scene
            for (int i = -4; i < 4; ++i) {
                for (int j = 0; j < 2; ++j) {
                    Moer::Vector3f pos = light_pos;
                    pos.x += i * 400;
                    pos.z += j * (225 + 140);
                    pos.y = 8;

                    for (int k = 0; k < 3; ++k) {
                        pos.y = pos.y + (k * 100);

                        light_color.x = static_cast<float>(rand()) / (RAND_MAX);
                        light_color.y = static_cast<float>(rand()) / (RAND_MAX);
                        light_color.z = static_cast<float>(rand()) / (RAND_MAX);

                        LightData light;
                        light.color     = Moer::Vector4f(light_color, 1.0f);
                        light.position  = Moer::Vector4f(pos, 1.0f);
                        light.direction = Moer::Vector4f(0.0f, 0.0f, 0.0f, 2);
                        lights.push_back(light);
                    }
                }
            }

            light_buffer      = GpuSceneBufferBuilder::CopyFrom(EBufferUsageFlags::STORAGE_BUFFER, lights.data(), lights.size() * sizeof(LightData));
            light_buffer_view = g_rhi->RHICreateBufferSRV(light_buffer);
        }
    }

    void DeferredRenderer::Impl::ShutDown() {
        RenderThreadFence render_thread_fence;
        render_thread_fence.BeginFence();
        render_thread_fence.Wait();
        MoerDelete(virtual_viewport);
    }

    void DeferredRenderer::Impl::DrawFrame() {
        //render and copy to backbuffer
        auto camera_entity = g_scene->GetCameras()[0];
        auto camera        = CameraManager::Get().Get(camera_entity);

        CameraData camera_data;

        // LOG_INFO("camera proj: {} {} {} {}", camera_proj.r0, camera_proj.r1, camera_proj.r2, camera_proj.r3);
        auto vp = camera->GetProjectionMatrix() * camera->GetViewMatrix();

        camera_data.prev_view_proj = vp;
        Matrix4x4f  transform{{-0.126363948, -0.289807469, 0.948684514, -49.9675217},
                              {0.259712189, 0.913335443, 0.313601822, 13.8385649},
                              {-0.957374394, 0.286012888, -0.0401463173, -85.6168518},
                              {-0.00000000, -0.00000000, -0.00000000, 1.f}};
        static bool first = true;
        if (first) {
            camera->SetWorldTransform(transform);
            first = false;
        }
        // camera->SetWorldTransform(Transform to_world)
        camera->Tick();

        camera_data.view       = camera->GetViewMatrix();
        camera_data.view_proj  = camera->GetProjectionMatrix() * camera->GetViewMatrix();
        camera_data.camera_pos = Vector4f(camera->GetPosition(), 1.f);
        CameraCullData cull_data;
        cull_data.camera_data      = camera_data;
        cull_data.proj             = camera->GetProjectionMatrix();
        cull_data.aspect_ratio     = camera->GetAspectRatio();
        cull_data.far_plane        = camera->GetFarClip();
        cull_data.near_plane       = camera->GetNearClip();
        cull_data.inv_tan_half_fov = 1.f / camera->GetTanHalfFov();

        auto& frustum_planes = cull_data.frustum_planes;
        {
            auto target_vp    = vp;
            frustum_planes[0] = target_vp.r3 + target_vp.r0;//left
            frustum_planes[1] = target_vp.r3 - target_vp.r0;//right
            frustum_planes[2] = target_vp.r3 + target_vp.r1;//top
            frustum_planes[3] = target_vp.r3 - target_vp.r1;//bottom
            frustum_planes[4] = target_vp.r2;               //near
            frustum_planes[5] = target_vp.r3 - target_vp.r2;//far
            //normalize
            for (int i = 0; i < 6; i++) {
                auto length       = Length(Vector3f(frustum_planes[i]));
                frustum_planes[i] = Vector4f(Normalizef(Vector3f(frustum_planes[i])), frustum_planes[i].w / length);
            }
        }

        {
            auto fill_uniform_data = [this, data(cull_data)]() {
                auto frame_offset = frame_counter % render_cmd_lists.size();

                auto  c_data = data;
                auto* ptr    = g_rhi->RHIMapBuffer(uniform_buffer, frame_offset * sizeof(CameraCullData), sizeof(CameraCullData));

                std::memcpy(ptr, &data, sizeof(CameraCullData));
                g_rhi->RHIUnmapBuffer(uniform_buffer);
            };
            EnqueueRenderTask(std::move(fill_uniform_data));
        }

        auto instance_count = instance_buffer_view->GetBuffer()->GetNumElement();

        auto* const scene = g_scene;

        auto&& sync_command_list = [this]() {
            int                     i        = render_cmd_lists.size();
            RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
            cmd_list->Reset();
            cmd_list->BeginRecording();
        };
        RHICopyBufferInfo copy_info{};
        copy_info.regions.push_back({0, 0, sizeof(uint32_t) * 32});

        RHIBarrierDependencyInfo counters_barrier{};
        counters_barrier.buffer_barriers.resize(1);
        auto& buffer_barrier_info = counters_barrier.buffer_barriers[0];
        buffer_barrier_info
            .SetBuffer(draw_count_buffer)
            .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
            .SetSrcAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
            .SetSrcStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT)
            .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER);

        auto&& reset_counter_buffer = [this, copy_info = std::move(copy_info), buffer_barrier(std::move(counters_barrier))]() {
            RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
            cmd_list->SetPipelineBarrier(buffer_barrier);
            cmd_list->CopyBuffer(copy_info, zero_buffer, draw_count_buffer);
        };

        EnqueueRenderTask(std::move(sync_command_list));
        EnqueueRenderTask(std::move(reset_counter_buffer));

        PrePass();

        DispatchDrawScene(camera_data);

        //hzb build
        EnqueueRenderTask([this]() {
            auto frame_offset = frame_counter % render_cmd_lists.size();
            HiZBuilder::GetInstance().DispatchBuildHiZ(render_cmd_lists[frame_offset], depth_buffer_srv[frame_offset], hiz_buffer);
        });
        RecheckPass();

        DispatchDrawScene(camera_data, false);
        EnqueueRenderTask([this]() {
            auto frame_offset = frame_counter % render_cmd_lists.size();
            HiZBuilder::GetInstance().DispatchBuildHiZ(render_cmd_lists[frame_offset], depth_buffer_srv[frame_offset], hiz_buffer);
        });

        LightingPass();

        EnqueueRenderTask([this]() {
            RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

            RHIBarrierDependencyInfo barrier_dependency_info{};
            barrier_dependency_info.texture_barriers.resize(1);
            auto& attachment_info = barrier_dependency_info.texture_barriers[0];
            attachment_info
                .SetTexture(virtual_viewport->GetBackBufferInfo().backbuffer_uav->GetTexture())
                .SetDstTextureLayout(TEXTURE_LAYOUT_TRANSFER_SRC)
                .SetSrcTextureLayout(TEXTURE_LAYOUT_COLOR_ATTACHMENT)
                .SetSrcStage(ERHIPipelineStageFlags::PS_COLOR_ATTACHMENT_OUTPUT)
                .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER)
                .SetSrcAccessFlags(ERHIAccessFlags::COLOR_ATTACHMENT_WRITE)
                .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_READ);

            //   cmd_list->SetPipelineBarrier(barrier_dependency_info);
        });
        {

            auto submit_rendering = [this]() {
                auto* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
                cmd_list->EndRecording();

                auto          back_buffer_info = virtual_viewport->GetBackBufferInfo();
                RHISubmitInfo submit_info;
                submit_info.Wait(back_buffer_info.backbuffer_ready_fence, frame_counter);
                submit_info.Wait(render_fence, frame_counter);
                submit_info.Signal(render_fence, ++frame_counter);

                render_queue->SubmitCommands(1, cmd_list, &submit_info);
            };
            EnqueueRenderTask(std::move(submit_rendering));
        }
    }

    void DeferredRenderer::Impl::Present() {
        EnqueueRenderTask([this]() {
            virtual_viewport->Present(render_fence);
        });
    }

    void DeferredRenderer::Impl::SetOriginResolution(uint32_t _width, uint32_t _height) {
    }

    void DeferredRenderer::Impl::SetPresentResolution(uint32_t _width, uint32_t _height) {
        if (_width == source_resolution.x && _height == source_resolution.y) {
            return;
        }
        source_resolution = Vector2i(_width, _height);
        OnResizeVSwapChain();
    }

    RHISRVRef DeferredRenderer::Impl::GetRendererOutput() {
        return virtual_viewport->GetPresentTextureSRV();
    }

    void DeferredRenderer::Impl::PrePass() {

        uint32_t frame_offset   = frame_counter % render_cmd_lists.size();
        auto     instance_count = instance_buffer_view->GetBuffer()->GetNumElement();

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
        cull_instance_params.cull_data                  = uniform_buffer_view[frame_offset];
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
        cull_meshlet_params.cull_data                           = uniform_buffer_view[frame_offset];
        cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
        cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

        RHIBatchedShaderParameters cull_instance_batched_params;
        cull_instance_batched_params.SetParameters(cull_instance_prepass_shader, cull_instance_params);

        RHIBatchedShaderParameters cull_meshlet_batched_params;
        cull_meshlet_batched_params.SetParameters(cull_meshlet_prepass_shader, cull_meshlet_params);
        auto&& prepass_cull_task = [this,
                                    instance_params(std::move(cull_instance_batched_params)),
                                    meshlet_params(std::move(cull_meshlet_batched_params)),
                                    instance_count(instance_count)]() mutable {
            RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

            RHIBarrierDependencyInfo barrier_dependency_info{};
            barrier_dependency_info.buffer_barriers.resize(2);
            auto& buffer_barrier_info = barrier_dependency_info.buffer_barriers[0];
            buffer_barrier_info
                .SetBuffer(draw_count_buffer)
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
                instance_cull_barrier.buffer_barriers.resize(2);
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

                cmd_list->SetPipelineBarrier(instance_cull_barrier);
            }

            cmd_list->SetPipelineState(cull_meshlet_prepass_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_meshlet_prepass_pso, meshlet_params);
            // cmd_list->Dispatch((max_meshlet_count + thread_group_count) / thread_group_count, 1, 1);
            cmd_list->DispatchIndirect(draw_count_buffer, meshlet_dispatch_offset);
            {
                RHIBarrierDependencyInfo post_compute_barrier{};
                post_compute_barrier.buffer_barriers.resize(3);
                auto& buffer_barrier_info_0 = post_compute_barrier.buffer_barriers[0];
                buffer_barrier_info_0
                    .SetBuffer(draw_count_buffer)
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
        EnqueueRenderTask(std::move(prepass_cull_task));
    }

    void DeferredRenderer::Impl::RecheckPass() {
        uint32_t frame_offset   = frame_counter % render_cmd_lists.size();
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
        cull_instance_params.cull_data                  = uniform_buffer_view[frame_offset];
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
        cull_meshlet_params.cull_data                           = uniform_buffer_view[frame_offset];
        cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
        cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

        RHIBatchedShaderParameters cull_instance_batched_params;
        cull_instance_batched_params.SetParameters(cull_instance_recheck_shader, cull_instance_params);

        RHIBatchedShaderParameters cull_meshlet_batched_params;
        cull_meshlet_batched_params.SetParameters(cull_meshlet_recheck_shader, cull_meshlet_params);

        auto recheck_pass_cull_task = [this,
                                       instance_params(std::move(cull_instance_batched_params)),
                                       meshlet_params(std::move(cull_meshlet_batched_params)),
                                       instance_count(instance_count)]() mutable {
            RHIGraphicsCommandList* cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

            RHIBarrierDependencyInfo barrier_dependency_info{};
            barrier_dependency_info.buffer_barriers.resize(3);
            auto& buffer_barrier_info = barrier_dependency_info.buffer_barriers[0];
            buffer_barrier_info
                .SetBuffer(draw_count_buffer)
                .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

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

            cmd_list->SetPipelineBarrier(barrier_dependency_info);

            cmd_list->SetPipelineState(cull_instance_recheck_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_instance_recheck_pso, instance_params);
            auto dispatch_count = (instance_count + thread_group_count - 1) / thread_group_count;
            // cmd_list->Dispatch(dispatch_count, 1, 1);
            cmd_list->DispatchIndirect(draw_count_buffer, instance_dispatch_indirect_offset);
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
                    .SetBuffer(draw_count_buffer)
                    .SetDstAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                    .SetSrcAccessFlags(ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::SHADER_READ)
                    .SetSrcStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER)
                    .SetDstStage(ERHIPipelineStageFlags::PS_COMPUTE_SHADER);

                cmd_list->SetPipelineBarrier(meshlet_cull_barrier);
            }

            cmd_list->SetPipelineState(cull_meshlet_recheck_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_meshlet_recheck_pso, meshlet_params);

            // cmd_list->Dispatch((max_meshlet_count + thread_group_count) / thread_group_count, 1, 1);
            cmd_list->DispatchIndirect(draw_count_buffer, recheck_meshlet_dispatch_offset);
            {
                RHIBarrierDependencyInfo post_compute_barrier{};
                post_compute_barrier.buffer_barriers.resize(2);
                auto& buffer_barrier_info_0 = post_compute_barrier.buffer_barriers[0];
                buffer_barrier_info_0
                    .SetBuffer(draw_count_buffer)
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

                cmd_list->SetPipelineBarrier(post_compute_barrier);
            }
        };
        EnqueueRenderTask(std::move(recheck_pass_cull_task));
    }

    void DeferredRenderer::Impl::DispatchDrawScene(const CameraData& _camera_data, bool _first_pass) {
        auto dispatch_gbuffer_pass = [&, b_first_pass(_first_pass)]() {
            RenderGraph render_graph;
            auto*       cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];

            {
                Extent3D extent = virtual_viewport->GetNextBackBufferExtent();

                render_graph.AddGraphicPass(
                    "GBuffer Pass",
                    [&](RenderGraph::Builder& builder) {
                        auto normal = render_graph.CreateTexture("normal", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R8G8B8A8_UNORM, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
                        auto mat    = render_graph.CreateTexture("mat", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R32_UINT, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
                        auto uv     = render_graph.CreateTexture("uv", {.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R16G16_SFLOAT, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
                        auto depth  = render_graph.ImportTexture("depth", depth_buffer[frame_counter % render_cmd_lists.size()]);
                        if (!b_first_pass) {
                            builder.readTextures({normal, uv, mat}, RenderGraphTexture::Usage::COLOR_ATTACHMENT);
                            builder.readTexture(depth, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT);
                        }
                        builder.writeTextures({normal, uv, mat}, RenderGraphTexture::Usage::COLOR_ATTACHMENT);
                        builder.writeTexture(depth, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT);
                        builder.DeclareRenderPass({.color_attachments = {
                                                       mat,
                                                       normal,
                                                       uv},
                                                   .depth_stencil_attachment = depth});
                    },
                    [this, b_first_pass](RenderPassContext& context) {
                        auto cmd_list = context.cmd_list;
                        cmd_list->SetPipelineState(gbuffer_pipeline_state);
                        // cmd_list->BindVertexBuffers()
                        auto scene = g_scene;
                        cmd_list->BindIndexBuffer(scene->GetBuffer("index_buffer"), 0, IET_UINT32);
                        uint32_t           offset[]           = {0, 0};
                        const RHIBufferRef prim_vertex_buffer = scene->GetBuffer("vertex_buffer");
                        const RHIBufferRef instance_id_buffer = scene->GetBuffer("instance_id_buffer");
                        RHIBufferRef       vbuffers[]         = {prim_vertex_buffer, instance_id_buffer};
                        cmd_list->BindVertexBuffers(0, 2, vbuffers, offset);

                        auto camera_entity = g_scene->GetMainCamera();
                        auto camera        = CameraManager::Get().Get(camera_entity);

                        CameraData camera_data;
                        camera_data.view           = camera->GetViewMatrix();
                        camera_data.view_proj      = camera->GetProjectionMatrix() * camera->GetViewMatrix();
                        camera_data.camera_pos     = Vector4f(camera->GetPosition(), 1.f);
                        auto vp                    = camera->GetProjectionMatrix() * camera->GetViewMatrix();
                        camera_data.prev_view_proj = vp;

                        TestGBufferShaderVert::Parameters vert_params;
                        vert_params.camera_data   = camera_data;
                        vert_params.instance_data = instance_buffer_view;

                        TestGBufferShaderFrag::Parameters frag_params;
                        frag_params.instance_data = instance_buffer_view;

                        RHIBatchedShaderParameters batched_params;
                        batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderVert>(), vert_params);
                        batched_params.SetParameters(ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderFrag>(), frag_params);

                        g_rhi->RHISetBatchedShaderParameters(gbuffer_pipeline_state, batched_params);

                        auto instance_idx = 0;
                        // scene->ForEach([cmd_list, &instance_idx](Entity entity) {
                        //     auto mesh_info = RenderableManager::Get().GetMeshInfo(entity);
                        //     cmd_list->DrawIndexedInstanced(mesh_info.index_count, 1, mesh_info.index_offset, mesh_info.vertex_offset, instance_idx++);
                        // });
                        auto draw_cnt_offset = b_first_pass ? draw_count_offset : recheck_draw_count_offset;
                        cmd_list->DrawIndexedIndirect(draw_indirect_buffer, 0, draw_count_buffer, draw_cnt_offset, draw_indirect_buffer->GetNumElement(), sizeof(DrawInstanceCmd));
                    });

                render_graph.AddGraphicPass(
                    "Lighting Pass", [&](RenderGraph::Builder& builder) {

            auto normal = render_graph.GetBlackBoard().GetHandle("normal");
            auto mat = render_graph.GetBlackBoard().GetHandle("mat");
            auto uv = render_graph.GetBlackBoard().GetHandle("uv");
            auto depth = render_graph.GetBlackBoard().GetHandle("depth");
            // auto position = render_graph.GetBlackBoard().GetHandle("position");
            auto output = render_graph.ImportTexture("swapchain_output",virtual_viewport->GetBackBufferInfo().backbuffer_uav->GetTexture());
            builder.readTextures({normal,uv,mat,depth},RenderGraphTexture::Usage::SAMPLED).writeTexture(output);
            builder.DeclareRenderPass({.color_attachments =  {output}}); }, [&](RenderPassContext& context) {
            auto cmd_list = context.cmd_list;
            cmd_list->SetPipelineState(lighting_pipeline_state);

            //First Get all material instances
            //Material organize this materials 
            //Material bind all resources for it's pass
            //Draw a full screen quad pass for each material type 
            Moer::UnorderedSet<EMaterialType> material_types = {}; 
            Moer::UnorderedMap<EMaterialType,Moer::Array<MaterialInstanceRef>> material_instances;
            g_scene->ForEach([&](Entity entity) {
                if (RenderableManager::Get().Contains(entity)) {
                    
                     auto mi = RenderableManager::Get().GetMaterialInstance(entity);
                    material_types.insert(mi->GetMaterial()->GetType());
                    material_instances[mi->GetMaterial()->GetType()].push_back(mi);
                }
            }); 

             auto mat_srv = render_graph.GetBlackBoard().GetTexture("mat")->GetSRV();
            auto normal_srv = render_graph.GetBlackBoard().GetTexture("normal")->GetSRV();
           auto uv_srv = render_graph.GetBlackBoard().GetTexture("uv")->GetSRV();
           auto depth_srv = render_graph.GetBlackBoard().GetTexture("depth")->GetSRV();

           Camera * camera = CameraManager::Get().Get(g_scene->GetMainCamera());     
           LightingData lighting_data;
           lighting_data.inv_view_proj = Inverse(camera->GetProjectionMatrix() * camera->GetViewMatrix());     
           lighting_data.light_count = lights.size();
               
            for(auto type : material_types){
                RHIBatchedShaderParameters parameters;
                LightingShaderFrag::Parameters frag_params;
                frag_params.lighting_data = lighting_data;
                frag_params.light_data = light_buffer_view;
                frag_params.depth_attach = depth_srv;
                frag_params.gbuffer_uv = uv_srv;
                frag_params.normal_attach = normal_srv;
                frag_params.mat_attach = mat_srv;
                parameters.SetParameters(ShaderResourceManager::GetInstance().GetShader<LightingShaderFrag>(),frag_params);
                
                auto& mat_instances = material_instances[type];
                MaterialRef material = mat_instances[0]->GetMaterial();
                material->OrganizeInstancesAndBind(parameters,mat_instances);
                g_rhi->RHISetBatchedShaderParameters(lighting_pipeline_state,parameters);

                cmd_list->Draw(3, 1,0,0);
                
            } });

                render_graph.SetGraphOutput(render_graph.GetBlackBoard().GetHandle("swapchain_output"));
            }

            render_graph.Execute({cmd_list, virtual_viewport->GetNextBackBufferExtent()});
        };
        EnqueueRenderTask(std::move(dispatch_gbuffer_pass));
    }

    void DeferredRenderer::Impl::BasePass() {
    }
    void DeferredRenderer::Impl::LightingPass() {
        // auto dispatch_rdg = [&]() {
        //     RenderGraph render_graph;
        //     auto*       cmd_list = render_cmd_lists[frame_counter % render_cmd_lists.size()];
        //     render_graph.AddGraphicPass(
        //         "Lighting Pass", [&](RenderGraph::Builder& builder) {
        //     auto normal = render_graph.GetBlackBoard().GetHandle("normal");
        //     auto mat = render_graph.GetBlackBoard().GetHandle("mat");
        //                 auto depth = render_graph.GetBlackBoard().GetHandle("depth");
        //     // auto position = render_graph.GetBlackBoard().GetHandle("position");
        //     auto output = render_graph.ImportTexture("swapchain_output",virtual_viewport->GetBackBufferInfo().backbuffer_uav->GetTexture());
        //     builder.readTextures({normal,mat,depth},RenderGraphTexture::Usage::SAMPLED).writeTexture(output);
        //     builder.DeclareRenderPass({.color_attachments =  {output}}); }, [&](RenderPassContext& context) {
        //     auto* cmd_list = context.cmd_list;
        //     cmd_list->SetPipelineState(lighting_pipeline_state);

        //     //First Get all material instances
        //     //Material organize this materials
        //     //Material bind all resources for it's pass
        //     //Draw a full screen quad pass for each material type
        //     Moer::UnorderedSet<EMaterialType> material_types = {};
        //     Moer::UnorderedMap<EMaterialType,Moer::Array<MaterialInstanceRef>> material_instances;
        //     g_scene->ForEach([&](Entity entity) {
        //         if (RenderableManager::Get().Contains(entity)) {

        //             auto mi = RenderableManager::Get().GetMaterialInstance(entity);
        //             material_types.insert(mi->GetMaterial()->GetType());
        //             material_instances[mi->GetMaterial()->GetType()].push_back(mi);
        //         }
        //     });

        //     auto mat_srv    = render_graph.GetBlackBoard().GetTexture("mat")->GetSRV();
        //     auto normal_srv = render_graph.GetBlackBoard().GetTexture("normal")->GetSRV();
        //     auto depth_srv  = render_graph.GetBlackBoard().GetTexture("depth")->GetSRV();

        //     Camera*      camera = CameraManager::Get().Get(g_scene->GetMainCamera());
        //     LightingData lighting_data;
        //     lighting_data.inv_view_proj = Inverse(camera->GetProjectionMatrix() * camera->GetViewMatrix());
        //     lighting_data.light_count   = lights.size();

        //     for(auto type : material_types){
        //         RHIBatchedShaderParameters parameters;
        //         LightingShaderFrag::Parameters frag_params;
        //         frag_params.lighting_data = lighting_data;
        //         frag_params.light_data = light_buffer_view;
        //         frag_params.depth_attach = depth_srv;
        //         frag_params.normal_attach = normal_srv;
        //         frag_params.mat_attach = mat_srv;
        //         parameters.SetParameters(ShaderResourceManager::GetInstance().GetShader<LightingShaderFrag>(),frag_params);

        //         auto& mat_instances = material_instances[type];
        //         MaterialRef material = mat_instances[0]->GetMaterial();
        //         material->OrganizeInstancesAndBind(parameters,mat_instances);
        //         g_rhi->RHISetBatchedShaderParameters(lighting_pipeline_state,parameters);

        //         cmd_list->Draw(3, 1,0,0);

        //     } });

        //     render_graph.SetGraphOutput(render_graph.GetBlackBoard().GetHandle("swapchain_output"));
        //     render_graph.Execute({cmd_list, virtual_viewport->GetNextBackBufferExtent()});
        // };

        // EnqueueRenderTask(std::move(dispatch_rdg)); //not ready yet
    }

}// namespace Moer