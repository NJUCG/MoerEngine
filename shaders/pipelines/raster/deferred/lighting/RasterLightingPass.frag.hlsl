#include "core/common/Bindless.hlsl"
#include "core/common/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)

#include "materials/Brdf.hlsli"
#include "materials/Material.hlsli"
#include "pipelines/raster/deferred/lighting/Lighting.hlsli"

#include "shared/raster/ShaderParameters.h"

[[vk::push_constant]] ConstantBuffer<Moer::MaterialPassBindlessParam> param;

float3 WorldPosFromDepth(float depth, float2 screen_uv, float4x4 inv_view_proj) {
    float4 clip    = float4(screen_uv.x * 2.f - 1.f, 1.f - screen_uv.y * 2.f, depth, 1.0);
    float4 world_w = mul(inv_view_proj, clip);
    float3 pos     = world_w.xyz / world_w.w;
    return pos;
}

float4 main(float2 in_uv : TEXCOORD0) : SV_TARGET {
    // MARK: Textures
    uint            material_id  = TextureHandle(param.vbuffer).Sample2D<uint>(in_uv);
    ArrayBuffer     material_buf = ArrayBuffer(param.material_buf_hdl);
    Moer::GMaterial mat          = material_buf.Load<Moer::GMaterial>(material_id);

    // MARK: Lighting Data
    ArrayBuffer        global_params = ArrayBuffer(param.global_param_handle);
    Moer::LightingData lighting_data = global_params.Load<Moer::LightingData>(0);

    // MARK: GBuffer
    float2 uv     = TextureHandle(param.gbuffer_uv).Sample2D<float2>(in_uv);
    float  depth  = TextureHandle(param.gbuffer_depth).Sample2D<float>(in_uv);
    float3 normal = normalize(
        Raster::UnpackNormal(TextureHandle(param.gbuffer_normal).Sample2D<float3>(in_uv))
    ); // 因为法线mipmap不满足线性关系，所以这里需要normalize
    float3 tangent =
        normalize(Raster::UnpackNormal(TextureHandle(param.gbuffer_tangent).Sample2D<float3>(in_uv))); // 同上
    float3 position = WorldPosFromDepth(depth, in_uv, lighting_data.inv_view_proj);

    // - Lights
    ArrayBuffer light_buf = ArrayBuffer(param.light_buf_hdl);

    // MARK: Skybox(Deprecated)
    if (depth == 0.0) {
        return float4(1.0, 0.0, 0.0, 1.0);
    }

    // MARK: PBR
    float3 albedo =
        GetTextureData<float3>(mat.albedo_map_hdl, uv, mat.albedo_factor.xyz, MISSING_TEXTURE_COLOR);

    float2 metallic_roughness = GetTextureData<float2>(
        mat.metallic_roughness_map_hdl,
        uv,
        float2(mat.metallic_factor, mat.roughness_factor),
        float2(mat.metallic_factor, mat.roughness_factor)
    );
    float metallic  = metallic_roughness.x;
    float roughness = metallic_roughness.y;

    float3 N   = GetNormalFromNormalMap(mat.normal_map_hdl, uv, normal, tangent);
    float3 V   = normalize(lighting_data.camera_position - position.xyz);
    float  NoV = saturate(dot(N, V));

    BRDFContext brdf_ctx;
    brdf_ctx.Init(
        roughness,
        albedo,
        metallic,
        N,
        V,
        TextureHandle(lighting_data.lut_ggx_emu_handle).Sample2D<float3>(float2(NoV, roughness)),
        TextureHandle(lighting_data.lut_ggx_eavg_handle).Sample2D<float3>(float2(0.0, roughness))
    );

    brdf_ctx.SetConfig(
        lighting_data.brdf_enable_multi_scatter,
        lighting_data.brdf_NDF_mode,
        lighting_data.brdf_G_mode,
        lighting_data.brdf_G_is_ibl
    );

    // - Shadow
    float shadow = TextureHandle(param.shadow_mask_handle).Sample2D<float>(in_uv);

    // MARK: Shading
    LightContext light_ctx;
    light_ctx.Init(brdf_ctx, position, lighting_data.lut_ggx_emu_handle);

    for (uint i = 0; i < lighting_data.light_count; i++) {
        Moer::GLight light = light_buf.Load<Moer::GLight>(i);

        light_ctx.AccumulateLight(light, shadow);
    }

    float3 color = light_ctx.GetResult();

    if (param.enable_extra_ambient) {
        color += param.extra_ambient_intensity * param.extra_ambient_color * brdf_ctx.albedo;
    }

    color = max(color, 0.0);

    return float4(color, 1.0);
}