#include "BasePass.h"
#include "PixelFormat.h"
#include "math/Base.h"
#include "rendergraph/RenderGraphPass.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "../Common.h"
#include "../Cull.h"
#include "RenderResourceDeferred.h"
#include "rhi/RHIResourceInitilizer.h"
#include "scene/CameraManager.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "../utils/HiZBuilder.h"
#include "../utils/CopyDispatchArgs.h"
#include <string_view>
#include "../DeferredRenderer.h"
#include "shaderheaders/shared/Geometry.h"

constexpr std::string_view instance_meshlet_cull_info_name = "BasePass::instance_meshlet_cull_info_buffer";
constexpr std::string_view recheck_instance_id_name        = "BasePass::recheck_instance_id_buffer";
constexpr std::string_view recheck_cull_info_name          = "BasePass::recheck_cull_info_buffer";
constexpr std::string_view counter_buffer_name             = "BasePass::counter_buffer";
constexpr std::string_view zero_buffer_name                = "BasePass::zero_buffer";
constexpr std::string_view uniform_buffer_name             = "BasePass::uniform_buffer";
constexpr std::string_view hzb_name                        = "BasePass::hzb_buffer";
constexpr std::string_view draw_indirect_name              = "BasePass::draw_indirect_buffer";
constexpr std::string_view indirect_args_name              = "BasePass::indirect_args_buffer";

namespace Moer {
    struct BasePass::Impl {
        friend class BasePass;

    public:
        Impl();
        ~Impl();

    private:
        void Init(RenderContext& _context);
        void InitSceneResources(RenderContext& _context);
        void DrawFrame(RenderContext& _input);
        void ResetCounter(RenderContext& _context);
        void PrePass(RenderContext& _context);
        template<bool is_prepass>
        void DrawScene(RenderContext& _context);
        void BuildHZB(RenderContext& _context);
        void PostPass(RenderContext& _context);
        void OnResizeViewport(Vector2i _extent);

    private:
        RHIComputePsoRef cull_instance_recheck_pso;
        RHIComputePsoRef cull_meshlet_recheck_pso;

        RHIShaderRef cull_instance_recheck_shader;
        RHIShaderRef cull_meshlet_recheck_shader;

        RHIComputePsoRef cull_instance_prepass_pso;
        RHIComputePsoRef cull_meshlet_prepass_pso;

        RHIGfxPsoRef gbuffer_pso;

        RHIShaderRef cull_instance_prepass_shader;
        RHIShaderRef cull_meshlet_prepass_shader;

        RHIShaderRef gbuffer_vert_shader;
        RHIShaderRef gbuffer_frag_shader;

        RHITextureRef          hzb;
        RHISRVRef              hzb_srv;
        uint32_t               hzb_mip_count;
        Moer::Array<RHIUAVRef> hzb_uavs;

        HiZBuffer hiz_buffer;

        RHIBufferRef draw_indirect_buffer;
        RHIBufferRef indirect_args_buffer;
        RHIBufferRef counter_buffer;
        RHIBufferRef zero_buffer;
        RHIBufferRef instance_meshlet_cull_info_buffer;
        RHIBufferRef recheck_cull_info_buffer;
        RHIBufferRef recheck_instance_id_buffer;
        RHIBufferRef view_buffer;

        RHISRVRef meshlet_descs_buffer_view;
        RHISRVRef meshlet_bounds_buffer_view;
        RHISRVRef instance_buffer_view;
        RHISRVRef instance_meshlet_info_view;
        RHISRVRef instance_meshlet_cull_info_view;
        RHISRVRef recheck_instance_id_srv;
        RHISRVRef recheck_cull_info_view;

        RHICBVRef view_buffer_view;

        RHIUAVRef instance_meshlet_cull_info_uav;
        RHIUAVRef draw_indirect_view;
        RHIUAVRef counter_view;
        RHISRVRef counter_srv;
        RHIUAVRef indirect_args_view;

        RHIUAVRef recheck_cull_info_uav;
        RHIUAVRef recheck_instance_id_uav;

        static constexpr uint32_t counter_buffer_size               = 32 * sizeof(uint32_t);
        static constexpr uint32_t meshlet_count_offset              = 0;
        static constexpr uint32_t draw_count_offset                 = 4;
        static constexpr uint32_t recheck_draw_count_offset         = 8;
        static constexpr uint32_t check_instance_count_offset       = 12;
        static constexpr uint32_t check_meshlet_count_offset        = 16;
        static constexpr uint32_t instance_dispatch_indirect_offset = 0;
        static constexpr uint32_t meshlet_dispatch_offset           = 12;
        static constexpr uint32_t recheck_meshlet_dispatch_offset   = 24;
        static constexpr uint32_t max_meshlet_count                 = 1024 * 512;
        static constexpr uint32_t thread_group_count                = 64;
        static constexpr uint32_t uniform_buffer_size               = sizeof(VirtualView);
        RenderResourceDeferred*   render_resources;
    };

    BasePass::Impl::Impl() {
    }

    BasePass::Impl::~Impl() {
    }

    void BasePass::Impl::Init(RenderContext& _context) {

        gbuffer_vert_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderVert>();
        gbuffer_frag_shader = ShaderResourceManager::GetInstance().GetShader<TestGBufferShaderFrag>();

        RHIVertexInputInfo vertex_input_info(

            VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(0, 9 * sizeof(float), PF_R32G32_SFLOAT, 3, sizeof(float) * 11, EVertexInputRate::VIR_VERTEX),
            VertexElement(1, 11 * sizeof(float), PF_R32_UINT, 4, sizeof(uint32_t), EVertexInputRate::VIR_INSTANCE));
        // RHIVertexInputStateRef vertex_input_state = g_rhi->RHICreateVertexInputState(vertex_input_state_init_list);

        RHIGraphicsShaderInputInfo gbuffer_shader_input_info = RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(vertex_input_info, gbuffer_vert_shader, gbuffer_frag_shader);
        RHIGraphicsPSOCreateInfo   pso_create_info =
            RHIGraphicsPSOCreateInfo::Create()
                .SetShaderStage(
                    std::move(gbuffer_shader_input_info))
                .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<Moer::Render::DepthStencil::DEPTH_WRITE_GREATER>())
                .SetColorAttachmentInfo(
                    {RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R32_UINT),
                     RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R8G8B8A8_UNORM),
                     RHIColorAttachmentInfo::Preset(EPixelFormat::PF_R16G16_SFLOAT)})
                .SetDepthStencilFormat(PF_D32_SFLOAT_S8_UINT)
                .Finalize();

        //ToDo: this may be can read and cached from render pass
        gbuffer_pso = g_rhi->RHICreateGraphicsPSO(std::move(pso_create_info));

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
            instance_meshlet_cull_info_buffer = g_rhi->RHICreateBuffer<uint>(max_meshlet_count * sizeof(int64), EBufferUsageFlags::UNORDERED_ACCESS);
            draw_indirect_buffer              = g_rhi->RHICreateBuffer<DrawInstanceCmd>(max_meshlet_count * sizeof(DrawInstanceCmd), EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::INDIRECT_BUFFER);
            indirect_args_buffer              = g_rhi->RHICreateBuffer<uint32_t>(counter_buffer_size, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::INDIRECT_BUFFER);
            counter_buffer                    = g_rhi->RHICreateBuffer<uint>(counter_buffer_size, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST | EBufferUsageFlags::INDIRECT_BUFFER);
            zero_buffer                       = g_rhi->RHICreateBuffer<uint>(counter_buffer_size, EBufferUsageFlags::CPU_VISIBLE);
            recheck_cull_info_buffer          = g_rhi->RHICreateBuffer<uint>(max_meshlet_count * sizeof(int64), EBufferUsageFlags::UNORDERED_ACCESS);
            recheck_instance_id_buffer        = g_rhi->RHICreateBuffer<uint>(max_meshlet_count * sizeof(uint), EBufferUsageFlags::UNORDERED_ACCESS);

            instance_meshlet_cull_info_view = g_rhi->RHICreateBufferSRV(instance_meshlet_cull_info_buffer);

            instance_meshlet_cull_info_uav = g_rhi->RHICreateBufferUAV(instance_meshlet_cull_info_buffer);
            draw_indirect_view             = g_rhi->RHICreateBufferUAV(draw_indirect_buffer);

            counter_view           = g_rhi->RHICreateBufferUAV(counter_buffer);
            counter_srv            = g_rhi->RHICreateBufferSRV(counter_buffer);
            recheck_cull_info_view = g_rhi->RHICreateBufferSRV(recheck_cull_info_buffer);
            recheck_cull_info_uav  = g_rhi->RHICreateBufferUAV(recheck_cull_info_buffer);

            recheck_instance_id_srv = g_rhi->RHICreateBufferSRV(recheck_instance_id_buffer);
            recheck_instance_id_uav = g_rhi->RHICreateBufferUAV(recheck_instance_id_buffer);

            indirect_args_view = g_rhi->RHICreateBufferUAV(indirect_args_buffer);
        }
    }

    void BasePass::Impl::InitSceneResources(RenderContext& _context) {
        auto meshlet_info_buffer       = g_scene->GetBuffer("meshlet_descs");
        auto meshlet_bound_buffer      = g_scene->GetBuffer("meshlet_bounds");
        auto instance_buffer           = g_scene->GetBuffer("instance_data");
        auto instance_mesh_info_buffer = g_scene->GetBuffer("instance_meshlet_info_buffer");

        meshlet_descs_buffer_view  = g_rhi->RHICreateBufferSRV(meshlet_info_buffer);
        meshlet_bounds_buffer_view = g_rhi->RHICreateBufferSRV(meshlet_bound_buffer);
        instance_buffer_view       = g_rhi->RHICreateBufferSRV(instance_buffer);
        instance_meshlet_info_view = g_rhi->RHICreateBufferSRV(instance_mesh_info_buffer);
    }

    void BasePass::Impl::DrawFrame(RenderContext& _input) {
        ResetCounter(_input);
        PrePass(_input);
        DrawScene<true>(_input);
        BuildHZB(_input);
        PostPass(_input);
        DrawScene<false>(_input);
        BuildHZB(_input);
    }

    void BasePass::Impl::ResetCounter(RenderContext& _context) {
        _context.GetRenderGraph().AddCopyPass(
            "Reset Counter",
            [&, this](RenderGraph::Builder& _builder) {
                auto tp_counter_buffer = _context.GetRenderGraph().ImportIfNotExist(counter_buffer_name, counter_buffer);
                auto tp_indirect_args  = _context.GetRenderGraph().ImportIfNotExist(indirect_args_name, indirect_args_buffer);
                auto src_buffer        = _context.GetRenderGraph().ImportIfNotExist(zero_buffer_name, zero_buffer);
                _builder.WriteBuffer(tp_counter_buffer, EBufferRuntimeUsageFlags::TRANSFER_WRITE);
                _builder.WriteBuffer(tp_indirect_args, EBufferRuntimeUsageFlags::TRANSFER_WRITE);
                _builder.ReadBuffer(src_buffer, EBufferRuntimeUsageFlags::TRANSFER_READ);
            },
            [&](RenderPassContext& _context) {
                auto*             cmd_list = _context.cmd_list;
                RHICopyBufferInfo copy_info{};
                copy_info.regions.push_back({0, 0, sizeof(uint32_t) * 32});
                cmd_list->CopyBuffer(copy_info, zero_buffer, counter_buffer);
                cmd_list->CopyBuffer(copy_info, zero_buffer, indirect_args_buffer);
            });
    }

    void BasePass::Impl::PrePass(RenderContext& _context) {
        uint32_t frame_offset   = _context.GetFrameOffset();
        auto     instance_count = instance_buffer_view->GetBuffer()->GetNumElement() / sizeof(Render::InstanceData);

        CullInstancePrePassShader::Parameters cull_instance_params;
        cull_instance_params.input.meshlet_count_offset          = meshlet_count_offset;
        cull_instance_params.input.instance_count                = instance_count;
        cull_instance_params.input.hiz_factor                    = Vector2f(hiz_buffer.texture->GetExtent2D());
        cull_instance_params.input.hiz_depth                     = hiz_buffer.texture->GetNumMips();
        cull_instance_params.input.recheck_counter_buffer_offset = check_instance_count_offset;

        cull_instance_params.instance_meshlet_info      = instance_meshlet_info_view;
        cull_instance_params.instance_meshlet_cull_info = instance_meshlet_cull_info_uav;
        cull_instance_params.recheck_instance_id        = recheck_instance_id_uav;
        cull_instance_params.counters_buffer            = counter_view;

        if (view_buffer_view == nullptr) {
            auto* view_handle = _context.GetRenderGraph().GetBlackBoard().GetBuffer(view_buffer_name.data());
            view_buffer_view  = g_rhi->RHICreateCBV(view_handle->GetBuffer());
        }
        cull_instance_params.views         = view_buffer_view;
        cull_instance_params.hiz_depth     = hiz_buffer.srv;
        cull_instance_params.depth_sampler = hiz_buffer.sampler;

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
        cull_meshlet_params.counters_buffer                     = counter_view;
        cull_meshlet_params.command_buffer                      = draw_indirect_view;
        cull_meshlet_params.views                               = view_buffer_view;
        cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
        cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

        RHIBatchedShaderParameters cull_instance_batched_params;
        cull_instance_batched_params.SetParameters(cull_instance_prepass_shader, cull_instance_params);

        RHIBatchedShaderParameters cull_meshlet_batched_params;
        cull_meshlet_batched_params.SetParameters(cull_meshlet_prepass_shader, cull_meshlet_params);

        auto prepass_cull_instance = [this,
                                      instance_params(std::move(cull_instance_batched_params)),
                                      instance_count(instance_count)](RenderPassContext& _context) mutable {
            auto& cmd_list = *_context.cmd_list;
            cmd_list.SetPipelineState(cull_instance_prepass_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_instance_prepass_pso, instance_params);
            auto dispatch_count = (instance_count + thread_group_count - 1) / thread_group_count;
            cmd_list.Dispatch(dispatch_count, 1, 1);
        };

        auto prepass_cull_meshlet = [this,
                                     meshlet_params(std::move(cull_meshlet_batched_params))](RenderPassContext& _context) mutable {
            auto& cmd_list = *_context.cmd_list;
            cmd_list.SetPipelineState(cull_meshlet_prepass_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_meshlet_prepass_pso, meshlet_params);
            cmd_list.DispatchIndirect(indirect_args_buffer, meshlet_dispatch_offset);
        };
        _context.GetRenderGraph().AddComputePass(
            "Prepass Cull Instance",
            [&](RenderGraph::Builder& _builder) {
                // auto& rg  = GetCurrentRenderGraph();
                auto hiz = _context.GetRenderGraph().ImportIfNotExist("HiZ Buffer", hiz_buffer.texture);
                // if (!hiz.IsInitialized()) {
                //     hiz = rg.ImportTexture("hiz_buffer", hiz_buffer.texture);
                // }
                // _builder.ReadTexture(hiz, ETextureUsageFlags::SAMPLED);
                auto& black_board = _context.GetRenderGraph().GetBlackBoard();
                _builder.ReadTexture(hiz, TS_SAMPLED, 0, MAX_TEXTURE_MIP_COUNT);
                _builder.WriteBuffer(_context.GetRenderGraph().ImportIfNotExist(instance_meshlet_cull_info_name, instance_meshlet_cull_info_buffer), EBufferRuntimeUsageFlags::WRITE);
                _builder.WriteBuffer(_context.GetRenderGraph().ImportIfNotExist(recheck_instance_id_name, recheck_instance_id_buffer), EBufferRuntimeUsageFlags::WRITE);
                _builder.WriteBuffer(_context.GetRenderGraph().ImportIfNotExist(counter_buffer_name, counter_buffer), EBufferRuntimeUsageFlags::WRITE);
                _builder.WriteBuffer(black_board.GetHandle(indirect_args_name), EBufferRuntimeUsageFlags::WRITE);
                _builder.ReadBuffer(black_board.GetHandle(view_buffer_name));
            },
            std::move(prepass_cull_instance));
        CopyDispatchArgs::Dispatch<64>(_context, counter_buffer_name, counter_srv, indirect_args_name, indirect_args_view, meshlet_count_offset, meshlet_dispatch_offset);
        _context.GetRenderGraph()
            .AddComputePass(
                "Prepass Cull Meshlet",
                [&](RenderGraph::Builder& _builder) {
                    auto hiz = _context.GetRenderGraph().GetBlackBoard().GetHandle("HiZ Buffer");
                    _builder.ReadTexture(hiz, TS_SAMPLED, 0, MAX_TEXTURE_MIP_COUNT);
                    auto& black_board                = _context.GetRenderGraph().GetBlackBoard();
                    auto  instance_meshlet_cull_info = black_board.GetHandle(instance_meshlet_cull_info_name.data());
                    auto  recheck_instance_id_hd     = black_board.GetHandle(recheck_instance_id_name.data());
                    auto  counter_buffer_hd          = black_board.GetHandle(counter_buffer_name.data());
                    auto  indirect_buffer_hd         = black_board.GetHandle(indirect_args_name.data());
                    _builder.ReadBuffer(instance_meshlet_cull_info, EBufferRuntimeUsageFlags::READ);
                    _builder.ReadBuffer(recheck_instance_id_hd, EBufferRuntimeUsageFlags::READ);
                    _builder.WriteBuffer(counter_buffer_hd, EBufferRuntimeUsageFlags::WRITE);
                    _builder.WriteBuffer(_context.GetRenderGraph().ImportIfNotExist(recheck_cull_info_name, recheck_cull_info_buffer), EBufferRuntimeUsageFlags::WRITE);
                },
                std::move(prepass_cull_meshlet));
    }

    void BasePass::Impl::BuildHZB(RenderContext& _context) {
        HiZBuilder::GetInstance().DispatchBuildHiZ(_context, _context.GetMainViewport().GetDepthSRV(), hiz_buffer);
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
        // cull_instance_params.instance_data                       = instance_buffer_view;
        cull_instance_params.instance_meshlet_info      = instance_meshlet_info_view;
        cull_instance_params.instance_meshlet_cull_info = recheck_cull_info_uav;
        cull_instance_params.recheck_instances          = recheck_instance_id_srv;
        cull_instance_params.counters_buffer            = counter_view;
        cull_instance_params.views                      = view_buffer_view;
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
        cull_meshlet_params.counters_buffer                     = counter_view;
        cull_meshlet_params.command_buffer                      = draw_indirect_view;
        cull_meshlet_params.views                               = view_buffer_view;
        cull_meshlet_params.hiz_depth                           = hiz_buffer.srv;
        cull_meshlet_params.depth_sampler                       = hiz_buffer.sampler;

        RHIBatchedShaderParameters cull_instance_batched_params;
        cull_instance_batched_params.SetParameters(cull_instance_recheck_shader, cull_instance_params);

        RHIBatchedShaderParameters cull_meshlet_batched_params;
        cull_meshlet_batched_params.SetParameters(cull_meshlet_recheck_shader, cull_meshlet_params);

        auto recheck_pass_cull_instance = [this,
                                           &_context,
                                           instance_params(std::move(cull_instance_batched_params))](RenderPassContext& _pass_context) mutable {
            auto& cmd_list = _context.GetCommandList();
            cmd_list.SetPipelineState(cull_instance_recheck_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_instance_recheck_pso, instance_params);
            // cmd_list->Dispatch(dispatch_count, 1, 1);
            cmd_list.DispatchIndirect(indirect_args_buffer, instance_dispatch_indirect_offset);
        };

        auto recheck_pass_cull_meshlet = [this, &_context, meshlet_params(std::move(cull_meshlet_batched_params))](RenderPassContext& _pass_context) mutable {
            auto& cmd_list = _context.GetCommandList();
            cmd_list.SetPipelineState(cull_meshlet_recheck_pso);
            g_rhi->RHISetBatchedShaderParameters(cull_meshlet_recheck_pso, meshlet_params);
            // cmd_list->Dispatch((max_meshlet_count + thread_group_count) / thread_group_count, 1, 1);
            cmd_list.DispatchIndirect(indirect_args_buffer, recheck_meshlet_dispatch_offset);
        };

        CopyDispatchArgs::Dispatch<64>(_context, counter_buffer_name, counter_srv, indirect_args_name, indirect_args_view, check_instance_count_offset, instance_dispatch_indirect_offset);

        _context.GetRenderGraph().AddComputePass(
            "Recheck Cull Instance",
            [&](RenderGraph::Builder& _builder) {
                auto& black_board              = _context.GetRenderGraph().GetBlackBoard();
                auto  hiz                      = black_board.GetHandle("HiZ Buffer");
                auto  recheck_cull_instance_hd = black_board.GetHandle(recheck_instance_id_name.data());
                auto  counter_buffer_hd        = black_board.GetHandle(counter_buffer_name.data());
                auto  recheck_cull_info_hd     = black_board.GetHandle(recheck_cull_info_name.data());
                auto  indirect_args_hd         = black_board.GetHandle(indirect_args_name.data());

                _builder.ReadTexture(hiz, TS_SAMPLED, 0, MAX_TEXTURE_MIP_COUNT);
                _builder.ReadBuffer(indirect_args_hd, EBufferRuntimeUsageFlags::INDIRECT_COMMAND_READ);
                _builder.ReadBuffer(recheck_cull_instance_hd, EBufferRuntimeUsageFlags::READ);
                _builder.ReadBuffer(counter_buffer_hd, EBufferRuntimeUsageFlags::WRITE);
                _builder.WriteBuffer(recheck_cull_info_hd, EBufferRuntimeUsageFlags::WRITE);
            },
            std::move(recheck_pass_cull_instance));

        CopyDispatchArgs::Dispatch<64>(_context, counter_buffer_name, counter_srv, indirect_args_name, indirect_args_view, check_meshlet_count_offset, recheck_meshlet_dispatch_offset);

        _context.GetRenderGraph().AddComputePass(
            "Recheck Cull Meshlet",
            [&](RenderGraph::Builder& _builder) {
                auto& black_board          = _context.GetRenderGraph().GetBlackBoard();
                auto  hiz                  = black_board.GetHandle("HiZ Buffer");
                auto  recheck_cull_info_hd = black_board.GetHandle(recheck_cull_info_name.data());
                auto  counter_buffer_hd    = black_board.GetHandle(counter_buffer_name.data());
                auto  draw_indirect_hd     = black_board.GetHandle(draw_indirect_name.data());
                auto  indirect_args_hd     = black_board.GetHandle(indirect_args_name.data());

                _builder.ReadTexture(hiz, TS_SAMPLED, 0, MAX_TEXTURE_MIP_COUNT);
                _builder.ReadBuffer(recheck_cull_info_hd, EBufferRuntimeUsageFlags::READ);
                _builder.ReadBuffer(indirect_args_hd, EBufferRuntimeUsageFlags::INDIRECT_COMMAND_READ);
                _builder.WriteBuffer(counter_buffer_hd, EBufferRuntimeUsageFlags::WRITE);
                _builder.WriteBuffer(draw_indirect_hd, EBufferRuntimeUsageFlags::WRITE);
            },
            std::move(recheck_pass_cull_meshlet));
    }
    template<bool is_prepass>
    void BasePass::Impl::DrawScene(RenderContext& _context) {

        auto& cmd_list = _context.GetCommandList();

        auto&        virtual_viewport = _context.GetMainViewport();
        Extent3D     extent           = virtual_viewport.GetBackBufferExtent();
        RenderGraph& rg               = _context.GetRenderGraph();
        rg.AddGraphicPass(
            "GBuffer Pass",
            [&](RenderGraph::Builder& _builder) {
                auto normal = rg.CreateIfNotExist("normal", RenderGraphTexture::Descriptor{.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R8G8B8A8_UNORM, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
                auto mat    = rg.CreateIfNotExist("mat", RenderGraphTexture::Descriptor{.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R32_UINT, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
                auto uv     = rg.CreateIfNotExist("uv", RenderGraphTexture::Descriptor{.extent2D = Extent2D(extent.x, extent.y), .format = EPixelFormat::PF_R16G16_SFLOAT, .usage = ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED});
                auto depth  = rg.ImportIfNotExist("depth", virtual_viewport.GetDepthSRV()->GetTexture());
                if constexpr (!is_prepass) {
                    _builder.ReadWriteTextures({normal, uv, mat}, TS_COLOR_ATTACHMENT);
                    _builder.ReadWriteTexture(depth, TS_DEPTH_STENCIL);
                } else {
                    _builder.WriteTextures({normal, uv, mat}, TS_COLOR_ATTACHMENT);
                    _builder.WriteTexture(depth, TS_DEPTH_STENCIL);
                }
                _builder.ReadBuffer(_context.GetRenderGraph().ImportIfNotExist(draw_indirect_name, draw_indirect_buffer), EBufferRuntimeUsageFlags::INDIRECT_COMMAND_READ);

                _builder.DeclareRenderPass({.color_attachments = {
                                                mat,
                                                normal,
                                                uv},
                                            .depth_stencil_attachment = depth});
            },
            [this](RenderPassContext& _context) {
                auto* cmd_list = _context.cmd_list;
                cmd_list->SetPipelineState(gbuffer_pso);
                // cmd_list->BindVertexBuffers()
                auto* scene = g_scene;
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

                g_rhi->RHISetBatchedShaderParameters(gbuffer_pso, batched_params);

                auto instance_idx    = 0;
                uint draw_cnt_offset = 0;
                if constexpr (is_prepass) {
                    draw_cnt_offset = draw_count_offset;
                } else {
                    draw_cnt_offset = recheck_draw_count_offset;
                }
                cmd_list->DrawIndexedIndirect(draw_indirect_buffer, 0, counter_buffer, draw_cnt_offset, draw_indirect_buffer->GetNumElement(), sizeof(DrawInstanceCmd));
            });
    }

    void
    BasePass::Impl::OnResizeViewport(Vector2i _extent) {
        hiz_buffer.InitFromDepthExtent(_extent);
    }

    BasePass::BasePass() : impl(MoerNew(Impl)) {
    }

    BasePass::~BasePass() {
        impl.reset();
    }

    void BasePass::InitResources(RenderContext& _context) {
        impl->Init(_context);
    }

    void BasePass::UpdateSceneData(RenderContext& _context) {
        impl->InitSceneResources(_context);
    }

    void BasePass::Draw(RenderContext& _input) {
        impl->DrawFrame(_input);
    }

    void BasePass::OnResizeViewport(Vector2i _extent) {
        impl->OnResizeViewport(_extent);
    }

}// namespace Moer