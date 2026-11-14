/**
  * Screen-space reflections
  * Reference: GAMES202
  */

#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "pipelines/RasterCommon.hlsli"
#include "shared/raster/ShaderParameters.h"

DEFINE_SHADER_CONSTANT(Moer::SsrPipelineBindlessParam, param)

float linearize_depth(float depth) {
    // 1.0 - d => reverse z to regular z
    return moer_constants.param.near_clip * moer_constants.param.far_clip / (moer_constants.param.far_clip + (1.0 - depth) * (moer_constants.param.near_clip - moer_constants.param.far_clip));
}

float get_depth(float2 uv) {
    return TextureHandle(moer_constants.param.depth_tex).Sample2D<float>(uv).x;
}

bool should_apply_ssr(float2 uv) { // the performance cost is so high
    if (moer_constants.param.ssr_is_force_ground_enable_ssr == 1) {
        float3 normal = Raster::UnpackNormal(TextureHandle(moer_constants.param.normal_tex).Sample2D<float3>(uv));
        if (normal.y + 0.001 >= 1.0) return true;
    }

    float2 metallic_roughness = TextureHandle(moer_constants.param.gbuffer_metal_rough_ao).Sample2D<float2>(uv);

    if (metallic_roughness.x < moer_constants.param.ssr_metallic_threshold) return false;
    if (metallic_roughness.y > moer_constants.param.ssr_roughness_threshold) return false;
    return true;
}

bool TryProjectSsrPointToScreen(float3 position, out float3 uvd) {
    uvd = 0.f;
    float4 p = mul(moer_constants.param.world2clip, float4(position, 1.0));
    if (!all(isfinite(p)) || p.w <= 0.f) {
        return false;
    }
    p /= p.w;
    // Reverse-Z depth already occupies [0,1]; only xy need the viewport transform.
    uvd = float3(p.x * 0.5 + 0.5, -p.y * 0.5 + 0.5, p.z);
    return all(isfinite(uvd)) && all(uvd >= 0.f) && all(uvd <= 1.f);
}

// to approximate sqrt(x) when x is in [0, 1]
float sqrt1(float x) {
    return x * (2.0 - x);
}

float4 ssr_sample_color(float2 uv) {
    return float4(
        TextureHandle(moer_constants.param.color_tex).Sample2D<float4>(uv).rgb,
        sqrt1(1.0 - max(abs(uv.x - 0.5), abs(uv.y - 0.5)) * 2.0) // this term will fade ssr effect around the screen
    );
}

bool TraceScreenSpaceReflection(float3 test_point, float3 direction, float jitter, out float2 hit_uv) {
    hit_uv = 0.f;
    float3 uvd;

    for (uint i = 0; i < moer_constants.param.ssr_sample_count; i++) {
        float3 last_point = test_point;
        test_point += direction * pow(float(i + 1) + jitter, 1.46); // 使得每次采样步长增加
        // uvd <=> test point in clip space <=> uv + depth
        if (!TryProjectSsrPointToScreen(test_point, uvd)) {
            return false;
        }

        float sample_depth = linearize_depth(get_depth(uvd.xy));
        float test_depth = linearize_depth(uvd.z);

        bool within_depth_thickness = (test_depth - sample_depth) < 0.001 * (1.0 + test_depth * 200.0 + float(i)); // 去除锯齿/波纹

        if (sample_depth < test_depth && within_depth_thickness) {
            direction = test_point - last_point;
            test_point = last_point;
            float s = 1.0;

            // use binary search instead
            // TODO: use HiZ to accelerate
            for (uint j = 0; j < 4; j++) {
                direction *= 0.5;
                test_point += direction * s;
                if (!TryProjectSsrPointToScreen(test_point, uvd)) {
                    return false;
                }
                sample_depth = linearize_depth(get_depth(uvd.xy));
                test_depth = linearize_depth(uvd.z);
                s = (sample_depth < test_depth) ? -1.0 : 1.0;
            }

            hit_uv = uvd.xy;
            return true;
        }
    }

    // Exhausting the sample budget does not establish an intersection.
    return false;
}

float3 ssr(float3 color, float2 uv) {
    float3 normal = TextureHandle(moer_constants.param.normal_tex).Sample2D<float4>(uv).rgb * 2.0 - 1.0;
    float3 position = WorldPosFromDepthTexture(moer_constants.param.depth_tex, uv, moer_constants.param.clip2world);

    float3 camera_to_pixel = normalize(position - moer_constants.param.camera_position);
    float3 reflect_dir = normalize(reflect(camera_to_pixel, normal));

    float fresnel = 0.02 + 0.98 * pow(1.0 - dot(reflect_dir, normal), 2.0);

    float jitter = moer_constants.param.ssr_is_enable_jitter
        ? fmod(uv.x * moer_constants.param.resolution.x + uv.y * moer_constants.param.resolution.y, 4.0) * 0.25
        : 0.0f;

    float2 hit_uv;
    if (!TraceScreenSpaceReflection(position + normal * 0.1, reflect_dir * moer_constants.param.ssr_step_base, jitter, hit_uv)) {
        return color;
    }
    float4 reflected_color = ssr_sample_color(hit_uv);
    return lerp(color, reflected_color.rgb, reflected_color.a * fresnel);
}

float4 main(float2 uv : TEXCOORD0) : SV_TARGET {

    float3 color = TextureHandle(moer_constants.param.color_tex).Sample2D<float4>(uv).rgb;

    if (!should_apply_ssr(uv)) {
        return float4(color, 1.0);
    } else {
        return float4(ssr(color, uv), 1.0);
    }
}
