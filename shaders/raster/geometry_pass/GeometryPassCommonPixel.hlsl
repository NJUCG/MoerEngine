#ifndef SHADOW_DEPTH_PASS
#define SHADOW_DEPTH_PASS 0
#endif

#include "framework/Bindless.hlsl"
#include "framework/Common.hlsl"
BINDLESS_BINDINGS(3, 2, 4, 5)
#include "shared/Geometry.h"
#include "shared/raster/geometry_pass/ShaderParameters.h"

#include "raster/geometry_pass/VertexFactory.hlsl"

[[vk::push_constant]] ConstantBuffer<Moer::GeometryPassBindlessParam> param;

#if SHADOW_DEPTH_PASS // MARK: ShadowDepthPass

void main(VertexFactory::VsOutput input) : SV_TARGET {
}

#else // MARK: GeometryPass

struct PsOutput {
    uint vbuffer : SV_TARGET0;
    float4 normal : SV_TARGET1;
    float4 tangent: SV_TARGET2;
    float2 texcoord0 : SV_TARGET3;
    float4 position : SV_TARGET4;
};

PsOutput main(VertexFactory::VsOutput input) : SV_TARGET {
    ArrayBuffer geometry_data_array = ArrayBuffer(param.geometry_data);

    Moer::GeometryData geom_data = Moer::LoadGeometryData(geometry_data_array.GetByteAddressBuffer(), input.instance_id * sizeof(Moer::GeometryData));

    PsOutput output;
    output.vbuffer = geom_data.mat_idx_and_type;
    output.normal = float4(Raster::PackNormal(input.normal), 1.0);
    output.tangent = float4(Raster::PackNormal(input.tangent), 1.0);
    output.texcoord0 = input.texcoord0;
    output.position = float4(input.world_position, 1.0);

    return output;
}

#endif // SHADOW_DEPTH_PASS