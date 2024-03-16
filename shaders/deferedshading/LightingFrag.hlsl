#include "framework/Material.hlsl"

[[vk::binding(2,0)]] Texture2D scene_textures[25];

static const uint CUR_MATERIAL_TYPE = 0;

[[vk::binding(0,1)]] StructuredBuffer<MaterialData> material_data : register(t0, space0);

// [[vk::input_attachment_index(0), vk::binding(1,0)]] SubpassInput mat_attach;
// [[vk::input_attachment_index(1), vk::binding(1,1)]] SubpassInput normal_attach;
// [[vk::input_attachment_index(2), vk::binding(1,2)]] SubpassInput pos_attach;

[[ vk::binding(1,0)]] Texture2D mat_attach;
[[ vk::binding(1,1)]] Texture2D normal_attach;
[[, vk::binding(1,2)]] Texture2D pos_attach;

[[vk::binding(0, 0)]] SamplerState defaultSampler;

// [[vk::input_attachment_index(1), vk::binding(1)]] SubpassInput depthAttach;


float4 main([[vk::location(0)]] float2 in_uv : TEXCOORD0) : SV_TARGET
{
    uint gbuffer_mat = mat_attach.Sample(defaultSampler, in_uv);
    uint mat_type = gbuffer_mat & 0x000000FF;
    if(mat_type != CUR_MATERIAL_TYPE)
    {
        discard;
    }
    uint mat_id = (gbuffer_mat & 0xFFFFFF00) >> 8;
    MaterialData mat = material_data[mat_id];
    float4 base_color;
    if(mat.albedo_map == 0)
    {
        base_color = mat.base_color_factor;
    }
    else
    {
        base_color = scene_textures[mat.albedo_map].Sample(defaultSampler, in_uv);
    }
    return base_color;
}