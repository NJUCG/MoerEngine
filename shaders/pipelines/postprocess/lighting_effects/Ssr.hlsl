/**
  * SSAO implementation
  * Reference: GAMES202
  */

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "materials/Material.hlsl"
#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::SsrPipelineBindlessParam> param;

static const float Epsilon = 0.0001; // same with PBRMaterialFrag.hlsl
static const float3 ABNORMAL_COLOR = float3(0.0, 0.0, 1.0);

float linearize_depth(float depth) {
    // 1.0 - d => reverse z to regular z
    return param.near_clip * param.far_clip / (param.far_clip + (1.0 - depth) * (param.near_clip - param.far_clip));
}

float get_depth(float2 uv) {
    return TextureHandle(param.depth_tex).Sample2D<float>(uv).x;
}

bool should_apply_ssr(float2 uv) { // the performance cost is so high
    if (param.ssr_is_force_ground_enable_ssr == 1) {
        float3 normal = Raster::UnpackNormal(TextureHandle(param.normal_tex).Sample2D<float3>(uv));
        if (normal.y + 0.001 >= 1.0) return true;
    }

    // reference PBRMaterialFrag.hlsl
    uint mat_id = (TextureHandle(param.vbuffer).Sample2D<uint>(uv) & 0xFFFFFF00) >> 8;
    MaterialData mat = UnpackMaterialData<MaterialData>(param.material_buffer, mat_id);
    float2 metallic_roughness = GetTextureData<float2>(
        mat.metallic_roughness_map,
        TextureHandle(param.gbuffer_uv).Sample2D<float2>(uv),
        float2(mat.metallic_factor, mat.roughness_factor),
        float2(mat.metallic_factor, mat.roughness_factor)
    );

    if (metallic_roughness.x < param.ssr_metallic_threshold) return false;
    if (metallic_roughness.y > param.ssr_roughness_threshold) return false;
    return true;
}

float3 apply_view_projection(float3 position) {
    float4 p = mul(param.view_projection_matrix, float4(position, 1.0));
    p /= p.w;
    // 两个究极大坑�?
    // p.z is not needed to apply f(x) = x * 0.5 + 0.5;
    // p.y is need to clip y;
    return float3(p.x * 0.5 + 0.5, -p.y * 0.5 + 0.5, p.z);
}

// to approximate sqrt(x) when x is in [0, 1]
float sqrt1(float x) {
    return x * (2.0 - x);
}

float4 ssr_sample_color(float2 uv) {
    return float4(
        TextureHandle(param.color_tex).Sample2D<float4>(uv).rgb,
        sqrt1(1.0 - max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0) // this term will fade ssr effect around the screen
    );
}

float3 ssr_ray_tracing(float3 color, float3 test_point, float3 direction, float fresnel, float jitter) {
    bool hit = false;
    float4 hit_color = float4(0, 0, 0, 0);
    float3 uvd;

    for (uint i = 0; i < param.ssr_sample_count; i++) {
        float3 last_point = test_point;
        test_point += direction * pow(float(i + 1) + jitter, 1.46); // 使得每次采样步长增加
        // uvd <=> test point in clip space <=> uv + depth
        uvd = apply_view_projection(test_point);

        if (
            uvd.x < 0
            || uvd.x > 1
            || uvd.y < 0
            || uvd.y > 1
            || uvd.z < 0
            || uvd.z > 1
        ) {
            hit = true;
            break;
        }

        float sample_depth = linearize_depth(get_depth(uvd.xy));
        float test_depth = linearize_depth(uvd.z);

        bool magic_formula = (test_depth - sample_depth) < 0.001 * (1.0 + test_depth * 200.0 + float(i)); // 去除锯齿/波纹

        if (sample_depth < test_depth && magic_formula) {
            direction = test_point - last_point;
            test_point = last_point;
            float s = 1.0;

            // use binary search instead
            // TODO: use HiZ to accelerate
            for (uint j = 0; j < 4; j++) {
                direction *= 0.5;
                test_point += direction * s;
                uvd = apply_view_projection(test_point);
                sample_depth = linearize_depth(get_depth(uvd.xy));
                test_depth = linearize_depth(uvd.z);
                s = (sample_depth < test_depth) ? -1.0 : 1.0;
            }

            hit = true;
            hit_color = ssr_sample_color(uvd.xy);
            break;
        }
    }

    if (!hit) {
        hit_color = ssr_sample_color(uvd.xy);
    }

    return lerp(color, hit_color.rgb, hit_color.a * fresnel);
}

float3 ssr(float3 color, float2 uv) {
    float3 normal = TextureHandle(param.normal_tex).Sample2D<float4>(uv).rgb * 2.0 - 1.0;
    float3 position = TextureHandle(param.position_tex).Sample2D<float4>(uv).rgb;

    float3 camera_to_pixel = normalize(position - param.camera_position);
    float3 reflect_dir = normalize(reflect(camera_to_pixel, normal));

    float fresnel = 0.02 + 0.98 * pow(1.0 - dot(reflect_dir, normal), 2.0);

    float jitter = param.ssr_is_enable_jitter
        ? fmod(uv.x * param.resolution.x + uv.y * param.resolution.y, 4.0) * 0.25
        : 0.0f;

    return ssr_ray_tracing(color, position + normal * 0.1, reflect_dir * param.ssr_step_base, fresnel, jitter);
}

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {

    float3 color = TextureHandle(param.color_tex).Sample2D<float4>(uv).rgb;

    if (!should_apply_ssr(uv)) {
        return float4(color, 1.0);
    } else {
        return float4(ssr(color, uv), 1.0);
    }
}