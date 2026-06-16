#include "RTResource.h"

#include "Configs.h"
#include "PixelFormat.h"
#include "PreprocessLightPass.h"
#include "ShaderUtils.h"
#include "config/ConfigManager.h"
#include "math/Function.h"
#include "renderer/common/RuntimeAssets.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include <cstdio>
#include <filesystem>
#include <stb_image.h>
#include <utility>

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/Scene.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "tinyexr.h"

namespace Moer::Render::Raytracing {

RTContext::RTContext(
    ShaderUtils&               _sd_utils,
    ImportanceSamplingContext& _is_ctx,
    BindlessArrayRef           _bdls_array
) :
    max_emissive_meshes(0),
    max_emissive_triangles(0),
    max_prim_lights(0),
    sd_utils(_sd_utils),
    is_ctx(_is_ctx),
    bdls(_bdls_array) {

    RenderDevice& device = RenderDevice::Get();
    neighbor_offset_buf  = device.CreateBuffer<byte>(
        "Raytracing::neighbor_offset_buf",
        is_ctx.GetNeighborOffsetCnt() * 2,
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TEXTURE_BUFFER,
        PF_R8G8_SNORM
    );
    // AllocateAndFreeBdlsIfNeeded(bindless_handles.neighbor_offset, neighbor_offset_buf->GetView());
}

void RTContext::SetBindlessHandles(const GpuScene::Res& gpu_scene_res) {
    bindless_handles.light_buf_hdl          = gpu_scene_res.light_buf.hdl;
    bindless_handles.material_buf_hdl       = gpu_scene_res.material_buf.hdl;
    bindless_handles.primitive_buf_hdl      = gpu_scene_res.primitive_buf.hdl;
    bindless_handles.instance_buf_hdl       = gpu_scene_res.instance_buf.hdl;
    bindless_handles.position_buf_hdl       = gpu_scene_res.position_buf.hdl;
    bindless_handles.packed_normal_buf_hdl  = gpu_scene_res.packed_normal_buf.hdl;
    bindless_handles.packed_tangent_buf_hdl = gpu_scene_res.packed_tangent_buf.hdl;
    bindless_handles.texcoord0_buf_hdl      = gpu_scene_res.texcoord0_buf.hdl;
    bindless_handles.index_buf_hdl          = gpu_scene_res.index_buf.hdl;

    // RT 专用（mesh-level BLAS 方案）
    bindless_handles.rt_instance_buf_hdl        = gpu_scene_res.rt_instance_buf.hdl;
    bindless_handles.rt_primitive_table_buf_hdl = gpu_scene_res.rt_primitive_table_buf.hdl;
}

void RTContext::FillFrameResources(uint2 _resolution) {
    RenderDevice& device = RenderDevice::Get();
    frame_rt.view_depth  = device.CreateTexture(
        "view_depth",
        Extent2D(_resolution),
        PF_R32_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.diffuse_albedo = device.CreateTexture(
        "diffuse_albedo",
        Extent2D(_resolution),
        PF_R32_UINT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.specular_roughness = device.CreateTexture(
        "specular_roughness",
        Extent2D(_resolution),
        PF_R32_UINT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.normal = device.CreateTexture(
        "normal",
        Extent2D(_resolution),
        PF_R32_UINT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.emission = device.CreateTexture(
        "emission",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.motion = device.CreateTexture(
        "motion",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.clip_depth = device.CreateTexture(
        "clip_depth",
        Extent2D(_resolution),
        PF_R32_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );

    frame_rt.prev_view_depth = device.CreateTexture(
        "prev_view_depth",
        Extent2D(_resolution),
        PF_R32_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.prev_diffuse_albedo = device.CreateTexture(
        "prev_diffuse_albedo",
        Extent2D(_resolution),
        PF_R32_UINT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.prev_specular_roughness = device.CreateTexture(
        "prev_specular_roughness",
        Extent2D(_resolution),
        PF_R32_UINT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.prev_normal = device.CreateTexture(
        "prev_normal",
        Extent2D(_resolution),
        PF_R32_UINT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.prev_luminance = device.CreateTexture(
        "prev_luminance",
        Extent2D(_resolution),
        PF_R16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );

    frame_rt.normal_roughness = device.CreateTexture(
        "normal_roughness",
        Extent2D(_resolution),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.diffuse_lighting = device.CreateTexture(
        "diffuse_lighting",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.prev_diffuse_lighting = device.CreateTexture(
        "prev_diffuse_lighting",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.specular_lighting = device.CreateTexture(
        "specular_lighting",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.prev_specular_lighting = device.CreateTexture(
        "prev_specular_lighting",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.temporal_sample_pos = device.CreateTexture(
        "temporal_sample_pos",
        Extent2D(_resolution),
        PF_R16G16_SINT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.gradients = device.CreateTexture(
        "gradients",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED,
        1,
        2
    );
    frame_rt.restir_luminance = device.CreateTexture(
        "restir_luminance",
        Extent2D(_resolution),
        PF_R16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.denoised_diffuse_lighting = device.CreateTexture(
        "denoised_diffuse_lighting",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.denoised_specular_lighting = device.CreateTexture(
        "denoised_specular_lighting",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );

    frame_rt.debug_color = device.CreateTexture(
        "debug_color",
        Extent2D(_resolution),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    // frame_rt.final_color = device.CreateTexture("final_color", Extent2D(_resolution), PF_R8G8B8A8_SRGB, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    frame_rt.ldr_color = device.CreateTexture(
        "ldr_color",
        Extent2D(_resolution),
        PF_R8G8B8A8_UNORM,
        ETextureUsageFlags::COLOR_ATTACHMENT | ETextureUsageFlags::SAMPLED
    );
    frame_rt.hdr_color = device.CreateTexture(
        "hdr_color",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.feedback_color_ping = device.CreateTexture(
        "feedback_color",
        Extent2D(_resolution),
        PF_R16G16B16A16_SNORM,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    frame_rt.feedback_color_pong = device.CreateTexture(
        "rw_feedback_color",
        Extent2D(_resolution),
        PF_R16G16B16A16_SNORM,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );

    frame_rt.resolved_color = device.CreateTexture(
        "resolved_color",
        Extent2D(_resolution),
        PF_R16G16B16A16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED
    );
    Sampler spl{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE};
    AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_depth, frame_rt.view_depth->GetView(), spl);
    AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_normal, frame_rt.normal->GetView(), spl);
    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.gbuffer_diffuse_albedo, frame_rt.diffuse_albedo->GetView(), spl
    );
    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.gbuffer_specular_roughness, frame_rt.specular_roughness->GetView(), spl
    );
    AllocateAndFreeBdlsIfNeeded(bindless_handles.motion, frame_rt.motion->GetView(), spl);

    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.gbuffer_prev_depth, frame_rt.prev_view_depth->GetView(), spl
    );
    AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_prev_normal, frame_rt.prev_normal->GetView(), spl);
    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.gbuffer_prev_diffuse_albedo, frame_rt.prev_diffuse_albedo->GetView(), spl
    );
    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.gbuffer_prev_specular_roughness, frame_rt.prev_specular_roughness->GetView(), spl
    );
    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.restir_prev_luminance, frame_rt.prev_luminance->GetView(), spl
    );
    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.denoiser_normal_roughness, frame_rt.normal_roughness->GetView(), spl
    );
}

void RTContext::SetResolution(uint2 _resolution) {
    FillFrameResources(_resolution);
    b_current_frame = true;
}

void RTContext::FillLowDiscrepancySequence(CommandList& _cmd_list) {
    if (b_has_neighbor_offset) {
        return;
    }
    GenLowDiscrepancySequenceParam param;
    param.num_dimensions = 2;
    param.num_samples    = is_ctx.GetNeighborOffsetCnt();
    sd_utils.GenerateLowDiscrepancySequence(_cmd_list, param, neighbor_offset_buf->GetView());
    b_has_neighbor_offset = true;
    // _cmd_list.Compute(sd_utils.gen_low_discrepancy_pipeline, _param, _output).Dispatch(uint3(DivCeil(_param.num_samples, 256), 1, 1), "GenerateLowDiscrepancySequence");
}

void RTContext::CreateEnvMapResources(TextureWithHandle _env_tex, CommandList& _cmd_list) {

    uint2         extent = _env_tex.tex->GetExtent().xy;
    RenderDevice& device = RenderDevice::Get();

    // env_pdf_tex 需要完整的 mip 链（SamplePdfMip 从最高 mip 向下采样），
    // 不能使用原始 env map 的 mip 数量（通常只有 1 级）
    uint full_mip_count = uint(std::floor(std::log2(float(std::max(extent.x, extent.y))))) + 1;

    env_pdf_mips.clear();
    env_pdf_tex = device.CreateTexture(
        "env_pdf_tex",
        Extent2D(extent.x, extent.y),
        PF_R16_SFLOAT,
        ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED,
        full_mip_count
    );

    for (int i = 0; i < env_pdf_tex->GetNumMips(); ++i) {
        env_pdf_mips.push_back(env_pdf_tex->GetView(i));
    }
    sd_utils.GenerateMipPdf(_cmd_list, _env_tex.tex, env_pdf_mips);

    AllocateAndFreeBdlsIfNeeded(
        bindless_handles.env_pdf,
        env_pdf_tex->GetView(0, env_pdf_tex->GetNumMips()),
        Sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE}
    );
    scene_params.env_map_handle = _env_tex.hdl;
    scene_params.enable_env_map = 1;
    SetEnvMapInfos(1.f, 0.f);
}

void RTContext::CreateBuffersIfNeeded(
    uint _num_emissive_meshes,
    uint _num_emissive_triangles,
    uint _num_prim_lights,
    uint _max_primitives
) {

    RenderDevice& device   = RenderDevice::Get();
    uint          task_num = _num_emissive_meshes + _num_prim_lights;

    if (!task_buf || task_num > task_buf->GetNumElement()) {
        task_buf = device.CreateBuffer<PrepareLightsTask>(
            "Raytracing::task_buf", task_num, EBufferUsageFlags::UNORDERED_ACCESS
        );
    }

    max_prim_lights        = _num_prim_lights;
    max_emissive_meshes    = _num_emissive_meshes;
    max_emissive_triangles = _num_emissive_triangles;

    uint max_local_lights  = max_emissive_triangles + max_prim_lights;
    uint light_buf_element = max_local_lights * 2;
    // light_buf_element 必须大于 0（即使没有光源，也需要至少 1 个元素用于双缓冲）
    assert(light_buf_element > 0 && "light_buf_element must be greater than 0");

    if (!light_mapping_buf || light_buf_element > light_data_buf->GetNumElement()) {
        light_mapping_buf = device.CreateBuffer<uint>(
            "Raytracing::light_mapping_buf", light_buf_element, EBufferUsageFlags::UNORDERED_ACCESS
        );
        light_data_buf = device.CreateBuffer<PolymorphicLightInfo>(
            "Raytracing::light_data_buf", light_buf_element, EBufferUsageFlags::UNORDERED_ACCESS
        );

        AllocateAndFreeBdlsIfNeeded(bindless_handles.light_index, light_mapping_buf->GetView());
        AllocateAndFreeBdlsIfNeeded(bindless_handles.poly_light_data, light_data_buf->GetView());
    }

    if (!prim_light_buf || max_prim_lights > prim_light_buf->GetNumElement()) {
        // max_prim_lights 可以为 0（没有场景光源），但 buffer 大小必须至少为 1
        uint prim_light_buf_size = max_prim_lights > 0 ? max_prim_lights : 1u;
        prim_light_buf           = device.CreateBuffer<PolymorphicLightInfo>(
            "Raytracing::prim_light_buf", prim_light_buf_size, EBufferUsageFlags::UNORDERED_ACCESS
        );
    }

    // 创建 primitive_to_light buffer（primitive_id -> light_offset 映射）
    // _max_primitives 必须大于 0（场景必须至少有一个 primitive）
    assert(_max_primitives > 0 && "_max_primitives must be greater than 0");
    if (!primitive_to_light_buf || _max_primitives > primitive_to_light_buf->GetNumElement()) {
        primitive_to_light_buf = device.CreateBuffer<uint>(
            "Raytracing::primitive_to_light_buf", _max_primitives, EBufferUsageFlags::UNORDERED_ACCESS
        );
        AllocateAndFreeBdlsIfNeeded(bindless_handles.primitive_to_light, primitive_to_light_buf->GetView());
    }
    {
        // 计算 texture 尺寸
        // light_buf_element 已经通过上面的 assert 确保 > 0
        uint texture_width  = RoundUpToPowerOf2(uint(ceil(sqrt(double(light_buf_element)))));
        uint texture_height = RoundUpToPowerOf2(uint(ceil(double(light_buf_element) / texture_width)));
        assert(texture_width > 0 && texture_height > 0 && "Texture dimensions must be greater than 0");
        uint mips = Max(1u, uint(log2(Max(texture_width, texture_height))) + 1u);

        if (!local_light_pdf_tex || texture_height != local_light_pdf_tex->GetExtent().y ||
            texture_width != local_light_pdf_tex->GetExtent().x) {
            local_light_pdf_mips.clear();
            local_light_pdf_tex = device.CreateTexture(
                "local_light_pdf_tex",
                Extent2D(texture_width, texture_height),
                PF_R16_SFLOAT,
                ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED,
                mips
            );

            for (int i = 0; i < local_light_pdf_tex->GetNumMips(); ++i) {
                local_light_pdf_mips.push_back(local_light_pdf_tex->GetView(i));
            }

            AllocateAndFreeBdlsIfNeeded(
                bindless_handles.local_light_pdf,
                local_light_pdf_tex->GetView(0, local_light_pdf_tex->GetNumMips()),
                Sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE}
            );
        }
    }
}

void RTContext::AllocateAndFreeBdlsIfNeeded(uint& _target, const TextureView& _view, Sampler _sampler) {
    if (_target) {
        bdls->UnbindTexture(_target);
        if (allocated_bdls_tex.contains(_target)) {
            allocated_bdls_tex.erase(_target);
        }
    }
    _target = bdls->AllocateTexture(_view, _sampler);
    allocated_bdls_tex.insert(_target);
}

void RTContext::AllocateAndFreeBdlsIfNeeded(uint& _target, const BufferView& _view) {
    if (_target) {
        bdls->UnbindBuffer(_target);
        if (allocated_bdls_buf.contains(_target)) {
            allocated_bdls_buf.erase(_target);
        }
    }
    _target = bdls->AllocateBuffer(_view);
    allocated_bdls_buf.insert(_target);
}

void RTContext::SetEnvMapInfos(float _scale, float _rotation) {
    scene_params.env_map_scale    = _scale;
    scene_params.env_map_rotation = _rotation;
}

void RTContext::Tick(Camera& _camera, float2 _jitter) {
    auto& device = RenderDevice::Get();
    prev_view    = main_view;

    float2 delta = 2.f / float2(frame_rt.ldr_color->GetExtent().xy);

    main_view.view2world = Transpose(_camera.GetToWorldMatrix());
    main_view.world2view = Transpose(_camera.GetViewMatrix());

    float4x4 view = _camera.GetViewMatrix();
    float4x4 proj = _camera.GetProjectionMatrix();

    // float4 test_prev = float4(0, 0, -1, 1);
    // test_prev        = proj * test_prev;
    // //apply jitter
    // proj[0][3] += _jitter.x * delta.x;
    // proj[1][3] += _jitter.y * delta.y;

    // float4 test = float4(0, 0, -1, 1);
    // test        = proj * test;

    float4x4 jitter_matrix = MakeTranslation(_jitter.x * delta.x, -_jitter.y * delta.y, 0.f);
    proj                   = jitter_matrix * proj;

    main_view.view2clip  = Transpose(proj);
    main_view.clip2view  = Transpose(Inverse(proj));
    main_view.clip2world = Transpose(Inverse(proj * view));
    main_view.world2clip = Transpose(proj * view);

    // main_view.world2clip        = Transpose(_camera->GetViewProjectionMatrix());
    // main_view.view2clip         = Transpose(_camera->GetProjectionMatrix());
    // main_view.clip2view         = Transpose(_camera->GetProjectionMatrixInv());
    // main_view.clip2world        = Transpose(_camera->GetViewProjectionMatrixInv());
    main_view.frustum  = _camera.GetFrustum();
    main_view.near_far = float2(_camera.GetNearClip(), _camera.GetFarClip());
    main_view.rect =
        float2(is_ctx.GetReSTIRDIConfig().render_width, is_ctx.GetReSTIRDIConfig().render_height);
    main_view.inv_rect          = float2(1.f / main_view.rect.x, 1.f / main_view.rect.y);
    main_view.dir_or_pos        = float4(_camera.GetPosition(), 1.f);
    main_view.clip2window_scale = float2(0.5f * main_view.rect.x, -0.5f * main_view.rect.y);
    main_view.clip2window_bias  = float2(0.5f * main_view.rect.x, 0.5f * main_view.rect.y);
    main_view.window2clip_scale = float2(2.f / main_view.rect.x, -2.f / main_view.rect.y);
    main_view.window2clip_bias  = float2(-1.f, 1.f);
    main_view.jitter            = _jitter;
    //restir
    {

        if (!light_reservoir_buf ||
            is_ctx.GetReSTIRDIRuntimeConfig().reservoir_buffer_params.block_array_pitch *
                    s_num_restirdi_reservoir_buffer >
                light_reservoir_buf->GetNumElement()) {
            light_reservoir_buf = device.CreateBuffer<DI::PackedReservoir>(
                "Raytracing::light_reservoir_buf",
                is_ctx.GetReSTIRDIRuntimeConfig().reservoir_buffer_params.block_array_pitch *
                    s_num_restirdi_reservoir_buffer,
                EBufferUsageFlags::UNORDERED_ACCESS
            );
        }

        if (!ris_buf ||
            2 * std::max(is_ctx.GetSegmentAllocator().GetTotalSize(), 1u) > ris_buf->GetNumElement()) {
            ris_buf = device.CreateBuffer<uint>(
                "Raytracing::ris_buf",
                2 * std::max(is_ctx.GetSegmentAllocator().GetTotalSize(), 1u),
                EBufferUsageFlags::UNORDERED_ACCESS
            );

            ris_light_data_buf = device.CreateBuffer<uint4>(
                "Raytracing::ris_light_data_buf",
                2 * std::max(is_ctx.GetSegmentAllocator().GetTotalSize(), 1u),
                EBufferUsageFlags::UNORDERED_ACCESS
            );
        }
    }
}

void RTContext::LoadDefaultResources(RuntimeAssets& _rt_res) {
    TextureRef white = _rt_res.GetTexture("white.png");
    TextureRef black = _rt_res.GetTexture("black.png");

    default_res.white_tex = white;
    default_res.black_tex = black;
}

void RTContext::AdvanceFrame() {
    b_current_frame = !b_current_frame;
    std::swap(bindless_handles.gbuffer_normal, bindless_handles.gbuffer_prev_normal);
    std::swap(bindless_handles.gbuffer_depth, bindless_handles.gbuffer_prev_depth);
    std::swap(bindless_handles.gbuffer_diffuse_albedo, bindless_handles.gbuffer_prev_diffuse_albedo);
    std::swap(bindless_handles.gbuffer_specular_roughness, bindless_handles.gbuffer_prev_specular_roughness);
    std::swap(bindless_handles.restir_luminance, bindless_handles.restir_prev_luminance);
}
}; // namespace Moer::Render::Raytracing