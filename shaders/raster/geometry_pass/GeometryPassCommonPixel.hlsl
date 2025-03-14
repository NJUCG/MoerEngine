#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/raster/geometry_pass/ShaderParameters.h"

#include "raster/geometry_pass/VertexFactory.hlsl"

struct PsOutput {
    uint vbuffer : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float2 texcoord0 : SV_TARGET2;
    float4 position : SV_TARGET3;
};

[[vk::push_constant]] ConstantBuffer<Moer::GeometryPassBindlessParam> param;

Texture2D<float> texture : register(t0, space1);
SamplerState defaultSampler : register(s0, space0);

PsOutput main(VertexFactory::VsOutput input) : SV_TARGET {
    ArrayBuffer geometry_data_array = ArrayBuffer(param.geometry_data);

    Moer::GeometryData geom_data = Moer::LoadGeometryData(geometry_data_array.GetByteAddressBuffer(), input.instance_id * sizeof(Moer::GeometryData));

    PsOutput output;
    output.vbuffer = geom_data.mat_idx_and_type;
    output.normal = float4(Raster::PackNormal(input.normal), 1.0);
    output.texcoord0 = input.texcoord0;
    output.position = float4(input.world_position, 1.0);

    return output;
}