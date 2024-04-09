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

    if (index == 0) {
        printf("rotation: %f %f %f %f\n", vertex_buffer[index].rotation.x, vertex_buffer[index].rotation.y, vertex_buffer[index].rotation.z, vertex_buffer[index].rotation.w);
        printf("S: %f %f %f %f %f %f %f %f %f\n", S[0][0], S[0][1], S[0][2], S[1][0], S[1][1], S[1][2], S[2][0], S[2][1], S[2][2]);
        printf("R: %f %f %f %f %f %f %f %f %f\n", R[0][0], R[0][1], R[0][2], R[1][0], R[1][1], R[1][2], R[2][0], R[2][1], R[2][2]);
        printf("scale: %f %f %f\n", vertex_buffer[index].scale_opacity.x, vertex_buffer[index].scale_opacity.y, vertex_buffer[index].scale_opacity.z);
        printf("cov3d: %f %f %f %f %f %f\n", cov3d[0][0], cov3d[0][1], cov3d[0][2], cov3d[1][1], cov3d[1][2], cov3d[2][2]);
        float3x3 trm = transpose(M);
        printf("trm: %f %f %f %f %f %f %f %f %f\n", trm[0][0], trm[0][1], trm[0][2], trm[1][0],trm[1][1],trm[1][2],trm[2][0],trm[2][1],trm[2][2]);
        printf("M: %f %f %f %f %f %f %f %f %f\n", M[0][0], M[0][1], M[0][2], M[1][0],M[1][1],M[1][2],M[2][0],M[2][1],M[2][2]);
    }
}