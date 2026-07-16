#include "PreprocessLightPass.h"

#include "rhi/RHICommand.h"
#include "shader/ShaderResourceManager.h"

#include <algorithm>
#include <cmath>

namespace Moer::Render::Raytracing {

namespace {

uint FloatToUInt(float value, float scale) {
    return static_cast<uint>(std::floor(value * scale + 0.5f));
}

float Saturate(float value) {
    return std::clamp(value, 0.f, 1.f);
}

uint Float3ToR8G8B8Unorm(const float3& value) {
    return (FloatToUInt(Saturate(value.x), 255.f) & 0xffu) |
           ((FloatToUInt(Saturate(value.y), 255.f) & 0xffu) << 8) |
           ((FloatToUInt(Saturate(value.z), 255.f) & 0xffu) << 16);
}

uint16_t Fp32ToFp16(float value) {
    static const union FloatBits {
        uint  ui;
        float f;
    } multiplier = {0x07800000};

    FloatBits biased_float{};
    biased_float.f  = value * multiplier.f;
    const uint bits = biased_float.ui;

    const uint sign = bits & 0x80000000;
    const uint body = bits & 0x0fffffff;
    return static_cast<uint16_t>((sign >> 16 | body >> 13) & 0xffffu);
}

void PackPolyLightColor(const float3& color, PolymorphicLightInfo& info) {
    const float max_radiance = Max(Max(color.x, color.y), color.z);
    if (max_radiance <= 0.f) {
        return;
    }

    const float log_radiance = std::clamp(
        (std::log2f(max_radiance) - g_poly_morphic_light_min_log2_radiance) /
            (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance),
        0.f,
        1.f
    );
    const uint  packed_radiance = std::min(static_cast<uint>(std::ceil(log_radiance * 65534.f)) + 1, 0xffffu);
    const float unpacked_radiance = std::exp2f(
        (static_cast<float>(packed_radiance - 1) / 65534.f) *
            (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance) +
        g_poly_morphic_light_min_log2_radiance
    );

    info.color_type_flags |= Float3ToR8G8B8Unorm(color / unpacked_radiance);
    info.log_radiance = packed_radiance;
}

} // namespace

PrepareLightPass::PrepareLightPass(ShaderManager& manager, BindlessArrayRef bindless_array) :
    bindless_array(std::move(bindless_array)),
    prepare_light_pipeline(manager.Compute<PrepareLightShaderPipeline>(
        "pipelines/raytracing/lighting/precompute/PrepareLights.hlsl"
    )) {}

void PrepareLightPass::Process(
    CommandList&                        cmd_list,
    RTContext&                          rt_ctx,
    const RaytracingSceneFrameSnapshot& scene_snapshot
) {
    Array<PrepareLightsTask>    tasks;
    Array<PolymorphicLightInfo> primitive_light_infos;
    Array<uint>                 primitive_to_light(scene_snapshot.primitive_count, s_invalid_light_idx);

    uint light_buffer_offset          = 0;
    uint num_emissive_triangle_lights = 0;
    for (const auto& primitive : scene_snapshot.emissive_primitives) {
        const auto previous = instance_light_buffer_offsets.find(primitive.stable_key);

        PrepareLightsTask task{};
        task.primitive_id       = primitive.primitive_id;
        task.light_offset       = light_buffer_offset;
        task.num_triangles      = primitive.num_triangles;
        task.prev_light_offset  = previous == instance_light_buffer_offsets.end() ? -1 : previous->second;
        task.index_start_idx    = primitive.index_start_idx;
        task.first_instance_idx = primitive.first_instance_idx;

        if (primitive.primitive_id >= primitive_to_light.size()) {
            primitive_to_light.resize(primitive.primitive_id + 1, s_invalid_light_idx);
        }
        primitive_to_light[primitive.primitive_id]          = light_buffer_offset;
        instance_light_buffer_offsets[primitive.stable_key] = light_buffer_offset;
        light_buffer_offset += task.num_triangles;
        num_emissive_triangle_lights += task.num_triangles;
        tasks.emplace_back(task);
    }

    uint num_finite_primitive_lights   = 0;
    uint num_infinite_primitive_lights = 0;
    uint num_environment_lights        = 0;

    for (const auto& light : scene_snapshot.analytic_lights) {
        const auto previous = primitive_light_buffer_offsets.find(light.stable_key);

        PrepareLightsTask task{};
        task.primitive_id      = static_cast<uint>(primitive_light_infos.size()) | g_task_prim_light_bit;
        task.light_offset      = light_buffer_offset;
        task.num_triangles     = 1;
        task.prev_light_offset = previous == primitive_light_buffer_offsets.end() ? -1 : previous->second;

        primitive_light_buffer_offsets[light.stable_key] = light_buffer_offset;
        ++light_buffer_offset;
        tasks.emplace_back(task);
        primitive_light_infos.emplace_back(light.light_info);

        switch (light.light_class) {
            case EAnalyticLightClass::Finite:
                ++num_finite_primitive_lights;
                break;
            case EAnalyticLightClass::Infinite:
                ++num_infinite_primitive_lights;
                break;
            case EAnalyticLightClass::Environment:
                ++num_environment_lights;
                break;
        }
    }

    if (rt_ctx.scene_params.enable_env_map && rt_ctx.env_pdf_tex) {
        PolymorphicLightInfo light_info{};
        light_info.color_type_flags = static_cast<uint>(EPolyLightType::ELEnv)
                                      << g_poly_morphic_light_type_shift;

        PackPolyLightColor(float3(rt_ctx.scene_params.env_map_scale), light_info);
        light_info.direction1  = rt_ctx.scene_params.env_map_handle;
        const uint3 env_extent = rt_ctx.env_pdf_tex->GetExtent();
        light_info.direction2  = env_extent.x | (env_extent.y << 16);
        light_info.scalars     = Fp32ToFp16(rt_ctx.scene_params.env_map_rotation);
        light_info.scalars |= g_poly_morphic_light_env_is_scalar_bit;

        constexpr uint64 environment_light_key = ~0ull;
        const auto       previous              = primitive_light_buffer_offsets.find(environment_light_key);

        PrepareLightsTask task{};
        task.primitive_id      = static_cast<uint>(primitive_light_infos.size()) | g_task_prim_light_bit;
        task.light_offset      = light_buffer_offset;
        task.num_triangles     = 1;
        task.prev_light_offset = previous == primitive_light_buffer_offsets.end() ? -1 : previous->second;

        primitive_light_buffer_offsets[environment_light_key] = light_buffer_offset;
        ++light_buffer_offset;
        tasks.emplace_back(task);
        primitive_light_infos.emplace_back(light_info);
        ++num_environment_lights;
    }

    cmd_list.PushScopeWithTimeScope("PrepareLights");

    if (!primitive_to_light.empty()) {
        cmd_list.CopyFrom(
            std::span<byte>(
                reinterpret_cast<byte*>(primitive_to_light.data()), primitive_to_light.size() * sizeof(uint)
            ),
            rt_ctx.primitive_to_light_buf->GetView(),
            "Upload primitive_to_light"
        );
    }

    cmd_list.CopyFrom(
        std::span<byte>(reinterpret_cast<byte*>(tasks.data()), tasks.size() * sizeof(PrepareLightsTask)),
        rt_ctx.task_buf->GetView(0, tasks.size() * sizeof(PrepareLightsTask)),
        "Upload tasks"
    );

    if (!primitive_light_infos.empty()) {
        cmd_list.CopyFrom(
            std::span<byte>(
                reinterpret_cast<byte*>(primitive_light_infos.data()),
                primitive_light_infos.size() * sizeof(PolymorphicLightInfo)
            ),
            rt_ctx.prim_light_buf->GetView(),
            "Upload prim light infos"
        );
    }

    cmd_list.ClearResource(rt_ctx.light_mapping_buf->GetView(), 0u);
    cmd_list.ClearResource(
        rt_ctx.local_light_pdf_tex->GetView(0, rt_ctx.local_light_pdf_tex->GetNumMips()), float4(0.f)
    );

    PrepareLightsParams params{};
    params.num_tasks          = static_cast<uint>(tasks.size());
    params.primitive_buf_hdl  = rt_ctx.GetBindlessHandles().primitive_buf_hdl;
    params.instance_buf_hdl   = rt_ctx.GetBindlessHandles().instance_buf_hdl;
    params.material_buf_hdl   = rt_ctx.GetBindlessHandles().material_buf_hdl;
    params.position_buf_hdl   = rt_ctx.GetBindlessHandles().position_buf_hdl;
    params.index_buf_hdl      = rt_ctx.GetBindlessHandles().index_buf_hdl;
    params.texcoord0_buf_hdl  = rt_ctx.GetBindlessHandles().texcoord0_buf_hdl;
    params.primitive_to_light = rt_ctx.GetBindlessHandles().primitive_to_light;

    const uint max_lights_in_buffer = static_cast<uint>(rt_ctx.light_data_buf->GetNumElement() / 2);
    params.cur_light_offset         = max_lights_in_buffer * b_odd_frame;
    params.prev_light_offset        = max_lights_in_buffer * !b_odd_frame;

    rt_ctx.is_ctx.SetLightBufferParams(
        params.cur_light_offset,
        num_finite_primitive_lights + num_emissive_triangle_lights,
        num_infinite_primitive_lights,
        num_environment_lights
    );

    cmd_list
        .Compute(
            prepare_light_pipeline,
            params,
            rt_ctx.light_data_buf->GetView(),
            rt_ctx.light_mapping_buf->GetView(),
            rt_ctx.local_light_pdf_tex->GetView(),
            rt_ctx.prim_light_buf->GetView(),
            rt_ctx.task_buf->GetView(),
            bindless_array
        )
        .Dispatch(uint3((light_buffer_offset + 255) / 256, 1, 1), "PrepareLights");

    rt_ctx.sd_utils.GenerateMips(cmd_list, rt_ctx.local_light_pdf_mips);

    cmd_list.AddCallback([primitive_to_light(std::move(primitive_to_light)),
                          primitive_light_infos(std::move(primitive_light_infos)),
                          tasks(std::move(tasks))]() {});

    cmd_list.PopScopeWithTimeScope();
    b_odd_frame = !b_odd_frame;
}

} // namespace Moer::Render::Raytracing
