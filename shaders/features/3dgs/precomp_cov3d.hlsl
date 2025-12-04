#include "3dgs.hlsl"
[[vk::binding(0, 0)]] StructuredBuffer<Vertex>  vertex_buffer : register(t0, space0);
[[vk::binding(1, 0)]] RWStructuredBuffer<float> cov3ds_buffer : register(u0, space0);

struct RenderParams {
    float scale_factor;
};

[[vk::push_constant]] ConstantBuffer<RenderParams> params : register(b0);

[numthreads(256, 1, 1)] void main(uint3 GlobalInvocationID
                                  : SV_DispatchThreadID, uint3 LocalInvocationID
                                  : SV_GroupThreadID) {
    uint index = GlobalInvocationID.x;
    uint length, stride;
    vertex_buffer.GetDimensions(length, stride);
    if (index >= length) {
        return;
    }
    float    scale_factor = params.scale_factor;
    float3x3 S            = getIndentity3x3();
    S[0][0]               = vertex_buffer[index].scale_opacity.x * scale_factor;
    S[1][1]               = vertex_buffer[index].scale_opacity.y * scale_factor;
    S[2][2]               = vertex_buffer[index].scale_opacity.z * scale_factor;

    // Compute rotation matrix from quaternion
    float3x3 R = rotationFromQuaternion(vertex_buffer[index].rotation);

    float3x3 M     = mul(R,S);
    float3x3 cov3d = mul(M,transpose(M) );

    cov3ds_buffer[index * 6]     = cov3d[0][0];
    cov3ds_buffer[index * 6 + 1] = cov3d[0][1];
    cov3ds_buffer[index * 6 + 2] = cov3d[0][2];
    cov3ds_buffer[index * 6 + 3] = cov3d[1][1];
    cov3ds_buffer[index * 6 + 4] = cov3d[1][2];
    cov3ds_buffer[index * 6 + 5] = cov3d[2][2];
}