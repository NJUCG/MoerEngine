// #include "BasePass.h"
// #include "PixelFormat.h"
// #include "rhi/RHI.h"
// #include "rhi/RHICommand.h"
// #include "rhi/RHICommon.h"
// #include "rhi/RHIResource.h"
// #include "../Common.h"
// #include "RenderResourceDeferred.h"
// #include "rhi/RHIResourceInitilizer.h"
// #include "shader/ShaderResourceManager.h"

// namespace Moer {
//     BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CameraData)
//     DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view)
//     DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view_proj)
//     END_SHADER_CONSTANT_STRUCT_DEFINITION()

//     BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CameraCullData)
//     DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view)
//     DEFINE_SHADER_PARAM(Moer::Matrix4x4f, view_proj)
//     DEFINE_SHADER_PARAM(Moer::Vector4f[6], frustum_planes)
//     DEFINE_SHADER_PARAM(Moer::Vector3f, camera_pos)
//     END_SHADER_CONSTANT_STRUCT_DEFINITION()
//     class BasePassDeferredVert : public Shader {
//         DEFINE_SHADER_TYPE(BasePassDeferredVert, Global, RENDER_API, ...)
//     public:
//         BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//         DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//         END_ROOT_PARAMETER_DEFINITION(Parameters)
//     };

//     class BasePassDeferredFrag : public Shader {
//         DEFINE_SHADER_TYPE(BasePassDeferredFrag, Global, RENDER_API, ...)
//     public:
//         BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//         // DEFINE_SHADER_PARAM_SAMPLER(SamplerState, defaultSampler)
//         // DEFINE_SHADER_PARAM_SRV(Texture2D, baseColorMap)
//         END_ROOT_PARAMETER_DEFINITION(Parameters)
//     };

//     IMPLEMENT_SHADER_TYPE(BasePassDeferredVert, "test/TriangleDeferredVert.hlsl", "main", ST_VERTEX);
//     IMPLEMENT_SHADER_TYPE(BasePassDeferredFrag, "test/TriangleDeferredFrag.hlsl", "main", ST_FRAGMENT);

//     BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CullInstanceInput)
//     DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
//     DEFINE_SHADER_PARAM(uint32_t, instance_count)
//     DEFINE_SHADER_PARAM(uint32_t, meshlet_count_offset)
//     END_SHADER_CONSTANT_STRUCT_DEFINITION(CullInstanceInput)

//     class BasePassCullInstanceShader : public Shader {
//         DEFINE_SHADER_TYPE(BasePassCullInstanceShader, Global, RENDER_API, ...)
//     public:
//         BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//         DEFINE_SHADER_PARAM_STRUCT(CullInstanceInput, input)
//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)

//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceMeshInfo>, instance_meshlet_info)

//         DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<uint64_t>, instance_meshlet_cull_info)
//         DEFINE_SHADER_PARAM_UAV(RWByteAddressBuffer, counters_buffer)
//         END_ROOT_PARAMETER_DEFINITION(Parameters)
//     };

//     BEGIN_SHADER_CONSTANT_STRUCT_DEFINITION(CullMeshletInput)
//     DEFINE_SHADER_PARAM_STRUCT(CameraData, camera_data)
//     DEFINE_SHADER_PARAM(uint32_t, meshlet_count_offset)
//     DEFINE_SHADER_PARAM(uint32_t, draw_count_offset)
//     END_SHADER_CONSTANT_STRUCT_DEFINITION(CullMeshletInput)

//     class BasePassCullMeshletShader : public Shader {
//         DEFINE_SHADER_TYPE(BasePassCullMeshletShader, Global, RENDER_API, ...)
//     public:
//         BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//         DEFINE_SHADER_PARAM_STRUCT(CullMeshletInput, input)
//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletDesc>, meshlet_info_buffer)
//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<MeshletBound>, meshlet_bound_buffer)
//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceData>, instance_data)
//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<InstanceMeshInfo>, instance_meshlet_info)
//         DEFINE_SHADER_PARAM_SRV(StructuredBuffer<uint64_t>, instance_meshlet_cull_info)

//         DEFINE_SHADER_PARAM_UAV(RWByteAddressBuffer, counters_buffer)
//         DEFINE_SHADER_PARAM_UAV(RWStructuredBuffer<DrawCommand>, command_buffer)

//         END_ROOT_PARAMETER_DEFINITION(Parameters)
//     };

//     IMPLEMENT_SHADER_TYPE(BasePassCullInstanceShader, "meshdebug/CullInstance.hlsl", "main", ST_COMPUTE);
//     IMPLEMENT_SHADER_TYPE(BasePassCullMeshletShader, "meshdebug/CullMeshlet.hlsl", "main", ST_COMPUTE);

//     class BuildHZBShader : public Shader {
//         DEFINE_SHADER_TYPE(BuildHZBShader, Global, RENDER_API, ...)
//     public:
//         BEGIN_ROOT_PARAMETER_DEFINITION(Parameters)
//         DEFINE_SHADER_PARAM_SRV(Texture2D, depth)
//         DEFINE_SHADER_PARAM_UAV(RWTexture2D, hzb)
//         END_ROOT_PARAMETER_DEFINITION(Parameters)
//     };

// }// namespace Moer

// namespace Moer {
//     struct BasePass::Impl {

//     public:
//         Impl();
//         ~Impl();

//     private:
//         void InitResources(RenderResourceDeferred& resources);
//         void DrawFrame(PassInput& input);
//         void PrePass(RHIGraphicsCommandList* cmd_list);
//         void BuildHZB();
//         void PostPass();
//         void OnResizeViewport(Vector2i extent);

//     private:
//         RHIGraphicsPipelineStateRef pipeline_state;
//         RHIComputePipelineStateRef  cull_instance_pso;
//         RHIComputePipelineStateRef  cull_meshlet_pso;

//         RHITextureRef          hzb;
//         RHISRVRef              hzb_srv;
//         uint32_t               hzb_mip_count;
//         Moer::Array<RHIUAVRef> hzb_uavs;

//         RHIBufferRef draw_indirect_buffer;
//         RHIBufferRef draw_count_buffer;
//         RHIBufferRef zero_buffer;
//         RHIBufferRef instance_meshlet_cull_info_buffer;
//         RHIBufferRef uniform_buffer;

//         RHISRVRef meshlet_descs_buffer_view;
//         RHISRVRef meshlet_bounds_buffer_view;
//         RHISRVRef instance_buffer_view;
//         RHISRVRef instance_meshlet_info_view;
//         RHISRVRef instance_meshlet_cull_info_view;

//         RHIUAVRef instance_meshlet_cull_info_uav;
//         RHIUAVRef draw_indirect_view;
//         RHIUAVRef draw_count_view;

//         static constexpr uint32_t meshlet_count_offset = 0;
//         static constexpr uint32_t draw_count_offset    = 4;
//         static constexpr uint32_t counter_buffer_size  = 32 * sizeof(uint32_t);
//         static constexpr uint32_t max_meshlet_count    = 1024 * 1024 * 16;
//         static constexpr uint32_t thread_group_count   = 64;

//         RenderResourceDeferred* render_resources;
//     };

//     BasePass::Impl::Impl() {
//     }

//     BasePass::Impl::~Impl() {
//     }

//     void BasePass::Impl::InitResources(RenderResourceDeferred& resources) {
//         render_resources = &resources;

//         RHIVertexInputInfo vertex_input_info(

//             VertexElement(0, 0, PF_R32G32B32_SFLOAT, 0, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 3 * sizeof(float), PF_R32G32B32_SFLOAT, 1, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 6 * sizeof(float), PF_R32G32B32_SFLOAT, 2, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 9 * sizeof(float), PF_R32G32B32_SFLOAT, 3, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(0, 12 * sizeof(float), PF_R32G32_SFLOAT, 4, sizeof(float) * 14, EVertexInputRate::VIR_VERTEX),
//             VertexElement(1, 14 * sizeof(float), PF_R32_UINT, 5, sizeof(uint32_t), EVertexInputRate::VIR_INSTANCE));

//         RHIShaderRef vertex_shader = ShaderResourceManager::GetInstance().GetShader<BasePassDeferredVert>();
//         RHIShaderRef frag_shader   = ShaderResourceManager::GetInstance().GetShader<BasePassDeferredFrag>();

//         RHIGraphicsPSOCreateInfo pso_info = RHIGraphicsPSOCreateInfo::Create()
//                                                 .SetColorAttachmentInfo(RHIColorAttachmentInfo::Preset<RHIConfig::Blend::ALPHA_BLEND>(PF_R8G8B8A8_SRGB))
//                                                 .SetDepthStencilInfo(RHIDepthStencilStateInfo::Preset<RHIConfig::DepthStencil::DEPTH_WRITE_GREATER>())
//                                                 .SetDepthStencilFormat(EPixelFormat::PF_D32_SFLOAT_S8_UINT)
//                                                 .SetShaderStage(RHIGraphicsShaderInputInfo::Create().SetVertexWorkFlow(vertex_input_info, vertex_shader, frag_shader))
//                                                 .Finalize();
//         pipeline_state = g_rhi->RHICreateGraphicsPSO(std::move(pso_info));

//         RHIShaderRef cull_instance_shader = ShaderResourceManager::GetInstance().GetShader<BasePassCullInstanceShader>();
//         RHIShaderRef cull_meshlet_shader  = ShaderResourceManager::GetInstance().GetShader<BasePassCullMeshletShader>();
//         cull_instance_pso                 = g_rhi->RHICreateComputePipelineState(cull_instance_shader);
//         cull_meshlet_pso                  = g_rhi->RHICreateComputePipelineState(cull_meshlet_shader);

//         meshlet_descs_buffer_view  = g_rhi->RHICreateBufferSRV(resources.meshlet_info_buffer);
//         meshlet_bounds_buffer_view = g_rhi->RHICreateBufferSRV(resources.meshlet_bounds_buffer);
//         instance_buffer_view       = g_rhi->RHICreateBufferSRV(resources.instance_buffer);
//         instance_meshlet_info_view = g_rhi->RHICreateBufferSRV(resources.instance_mesh_info_buffer);

//         {
//             //self resources
//             instance_meshlet_cull_info_buffer = g_rhi->RHICreateBuffer<uint64_t>(max_meshlet_count, EBufferUsageFlags::STORAGE_BUFFER);
//             draw_indirect_buffer              = g_rhi->RHICreateBuffer<DrawInstanceCmd>(max_meshlet_count, EBufferUsageFlags::STORAGE_BUFFER);
//             draw_count_buffer                 = g_rhi->RHICreateBuffer<uint32_t>(counter_buffer_size, EBufferUsageFlags::STORAGE_BUFFER | EBufferUsageFlags::TRANSFER_DST);
//             zero_buffer                       = g_rhi->RHICreateBuffer<uint32_t>(counter_buffer_size, EBufferUsageFlags::CPU_VISIBLE);

//             instance_meshlet_cull_info_view = g_rhi->RHICreateBufferSRV(instance_meshlet_cull_info_buffer);

//             instance_meshlet_cull_info_uav = g_rhi->RHICreateBufferUAV(instance_meshlet_cull_info_buffer);
//             draw_indirect_view             = g_rhi->RHICreateBufferUAV(draw_indirect_buffer);
//         }
//     }

//     void BasePass::Impl::DrawFrame(PassInput& input) {
//         //clear counter buffer
//         //do instance culling (frustum + prev_hzb)
//         //do meshlet culling (frustum + prev_hzb)
//         //draw
//         //build hzb
//         //do instance culling (hzb)
//         //do meshlet culling (hzb)

//         RHIGraphicsCommandList* cmd_list = input.cmd_list;
//         PrePass(cmd_list);

//         cmd_list->CopyBuffer(
//             RHICopyBufferInfo({0, 0, 32 * sizeof(uint32_t)}),
//             zero_buffer,
//             draw_count_buffer);

//         {
//             RHIBarrierDependencyInfo buffer_barrier;
//             buffer_barrier.buffer_barriers.resize(1);
//             auto& barrier = buffer_barrier.buffer_barriers[0];
//             barrier.SetBuffer(draw_count_buffer)
//                 .SetSrcStage(PS_TRANSFER)
//                 .SetDstStage(PS_COMPUTE_SHADER)
//                 .SetSrcAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
//                 .SetDstAccessFlags(ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE);

//             cmd_list->SetPipelineBarrier(buffer_barrier);
//         }

//         //draw
//         cmd_list->SetViewPort({0, 0, 1920, 1080});
//         cmd_list->SetScissor({0, 0, 1920, 1080});
//         cmd_list->SetPipelineState(pipeline_state);
//         RHIRenderPassInfo render_pass_info;
//         auto&             out_color       = render_pass_info.color_attachments[0];
//         out_color.color_attachment_action = EAttachmentAction::AC_CLEAR_STORE;
//         out_color.color_attachment_view   = RenderAttachmentView(
//             input.target_attachment,
//             ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT,
//             RHIClearAttachment(EClearAttachment::COLOR));

//         auto& out_depth                         = render_pass_info.depth_stencil_attachment;
//         out_depth.depth_stencil_attachment_view = RenderAttachmentView(
//             hzb_uavs[0],
//             TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE,
//             RHIClearAttachment(EClearAttachment::DEPTH_STENCIL));
//     }

//     void BasePass::Impl::PrePass(RHIGraphicsCommandList* cmd_list) {

//         RHIBarrierDependencyInfo counters_barrier{};
//         counters_barrier.buffer_barriers.resize(1);
//         auto& buffer_barrier_info = counters_barrier.buffer_barriers[0];
//         buffer_barrier_info
//             .SetBuffer(draw_count_buffer)
//             .SetDstAccessFlags(ERHIAccessFlags::TRANSFER_WRITE)
//             .SetSrcAccessFlags(ERHIAccessFlags::INDIRECT_COMMAND_READ)
//             .SetSrcStage(ERHIPipelineStageFlags::PS_DRAW_INDIRECT)
//             .SetDstStage(ERHIPipelineStageFlags::PS_TRANSFER);
//         auto counter_barrier = [cmd_list, barrier = std::move(counters_barrier)]() {
//             cmd_list->SetPipelineBarrier(barrier);
//         };

//         EnqueueRenderTask(std::move(counter_barrier));
//     }

//     void BasePass::Impl::BuildHZB() {
//     }

//     void BasePass::Impl::PostPass() {
//     }

//     void BasePass::Impl::OnResizeViewport(Vector2i extent) {
//     }

//     BasePass::BasePass() : impl(MoerNew(Impl)) {
//     }

//     BasePass::~BasePass() {
//     }

// }// namespace Moer