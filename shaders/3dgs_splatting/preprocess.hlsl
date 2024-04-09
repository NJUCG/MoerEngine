#include "3dgs.hlsl"

struct Params {
    float4   camera_position;
    row_major float4x4 proj_mat;
    row_major float4x4 view_mat;
    uint     width;
    uint     height;
    float    tan_fovx;
    float    tan_fovy;
};

[[vk::binding(0, 0)]] StructuredBuffer<Vertex>            vertex_buffer : register(t0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<float>             cov3ds_buffer : register(t1, space0);
[[vk::binding(1, 1)]] RWStructuredBuffer<VertexAttribute> vertex_attribute_buffer : register(u0, space1);
[[vk::binding(2, 1)]] RWStructuredBuffer<uint>            tile_overlap_buffer : register(u1, space1);

//[[vk::push_constant]] ConstantBuffer<Params> params  : register(b0);
[[vk::binding(0, 1)]] ConstantBuffer<Params> params : register(b0);
// Compute shader entry point

float3x3 get_projection_jacobian_approx(float3 t) {
    float limx = 1.3 * params.tan_fovx;
    float limy = 1.3 * params.tan_fovy;
    float txtz = t.x / t.z;
    float tytz = t.y / t.z;
    t.x        = min(limx, max(-limx, txtz)) * t.z;
    t.y        = min(limy, max(-limy, tytz)) * t.z;

    float focal_x = params.width / (2 * params.tan_fovx);
    float focal_y = params.height / (2 * params.tan_fovy);

    return float3x3(
        focal_x / t.z, 0, -(focal_x * t.x) / (t.z * t.z), 0, focal_y / t.z, -(focal_y * t.y) / (t.z * t.z), 0, 0, 0);
}

//计算投影得2D协方差矩阵
float2x2 compute_cov2d(float3 cam, uint index) {
    float3x3 J     = get_projection_jacobian_approx(cam);
    float3x3 W     = transpose((float3x3)params.view_mat);
    float3x3 Sigma = float3x3(
        cov3ds_buffer[index * 6], cov3ds_buffer[index * 6 + 1], cov3ds_buffer[index * 6 + 2], cov3ds_buffer[index * 6 + 1], cov3ds_buffer[index * 6 + 3], cov3ds_buffer[index * 6 + 4], cov3ds_buffer[index * 6 + 2], cov3ds_buffer[index * 6 + 4], cov3ds_buffer[index * 6 + 5]);
    float3x3 T     = mul(J,W);
    float3x3 cov2d = mul(T,mul(Sigma,transpose(T)) );
    cov2d[0][0] += 0.3f;
    cov2d[1][1] += 0.3f;
    return (float2x2)cov2d;
}

float3 get_sh_float3(uint ind, uint index) {
    return float3(vertex_buffer[index].sh[ind * 3], vertex_buffer[index].sh[ind * 3 + 1], vertex_buffer[index].sh[ind * 3 + 2]);
}

float3 compute_sh(uint index) {

    float3 ray_direction = vertex_buffer[index].position.xyz - params.camera_position.xyz;
    ray_direction /= length(ray_direction);
    float x = ray_direction.x, y = ray_direction.y, z = ray_direction.z;

    float3 c = SH_C0 * get_sh_float3(0, index);

    c -= SH_C1 * get_sh_float3(1, index) * y;
    c += SH_C1 * get_sh_float3(2, index) * z;
    c -= SH_C1 * get_sh_float3(3, index) * x;

    c += SH_C2[0] * get_sh_float3(4, index) * x * y;
    c += SH_C2[1] * get_sh_float3(5, index) * y * z;
    c += SH_C2[2] * get_sh_float3(6, index) * (2.0 * z * z - x * x - y * y);
    c += SH_C2[3] * get_sh_float3(7, index) * z * x;
    c += SH_C2[4] * get_sh_float3(8, index) * (x * x - y * y);

    c += SH_C3[0] * get_sh_float3(9, index) * (3.0 * x * x - y * y) * y;
    c += SH_C3[1] * get_sh_float3(10, index) * x * y * z;
    c += SH_C3[2] * get_sh_float3(11, index) * (4.0 * z * z - x * x - y * y) * y;
    c += SH_C3[3] * get_sh_float3(12, index) * z * (2.0 * z * z - 3.0 * x * x - 3.0 * y * y);
    c += SH_C3[4] * get_sh_float3(13, index) * x * (4.0 * z * z - x * x - y * y);
    c += SH_C3[5] * get_sh_float3(14, index) * (x * x - y * y) * z;
    c += SH_C3[6] * get_sh_float3(15, index) * x * (x * x - 3.0 * y * y);

    c += 0.5;

    if (c.x < 0.0) {
        c.x = 0.0;
    }

    return c;
}

float ndc2Pix(float v, int S) {
    return ((v + 1.0) * S - 1.0) * 0.5;
}

[numthreads(TILE_WIDTH * TILE_HEIGHT, 1, 1)] void main(uint3 dispatchThreadId
                                                       : SV_DispatchThreadID,uint3 groupThreadId : SV_GroupThreadID,uint3 groupID : SV_GroupID) {
    uint index = dispatchThreadId.x;


    uint compute_index = groupID.x * TILE_WIDTH * TILE_HEIGHT + groupThreadId.x;




    uint length, stride;
    vertex_buffer.GetDimensions(length, stride);
    if (index >= length) {
        return;
    }
    

    uint2 tile_shape = uint2((params.width + TILE_WIDTH - 1) / TILE_WIDTH, (params.height + TILE_HEIGHT - 1) / TILE_HEIGHT);

    vertex_attribute_buffer[index].color_radii.w = 0.0;
    tile_overlap_buffer[index]                   = 0;

    float4 p_hom = mul(vertex_buffer[index].position,params.proj_mat );
    float  p_w   = 1.0f / p_hom.w;
    float3 ndc   = float3(p_hom.xyz * p_w);

    float4 p_view = mul( vertex_buffer[index].position,params.view_mat);


    if (p_view.z <= 0.2f) {
        return;
    }

    float2x2 cov2d = compute_cov2d(p_view.xyz, index);
   

    float    det   = determinant(cov2d);
    if (det <= 0.0) {
        return;
    }
        
    float2x2 conic                                   = inverse(cov2d);

    vertex_attribute_buffer[index].conic_opacity.xyz = float3(conic[0][0], conic[0][1], conic[1][1]);
    vertex_attribute_buffer[index].conic_opacity.w   = vertex_buffer[index].scale_opacity.w;

    float mid     = 0.5 * (cov2d[0][0] + cov2d[1][1]);
    float lambda1 = mid + sqrt(max(0.1, mid * mid - det));
    float lambda2 = mid - sqrt(max(0.1, mid * mid - det));
    float lambda  = max(lambda1, lambda2);
    float radii   = ceil(3.0 * sqrt(lambda));

    float2 uv = float2(ndc2Pix(ndc.x, int(params.width)), ndc2Pix(ndc.y, int(params.height)));

    uint4 bounding_box = uint4(
        uint(clamp(int((uv.x - radii) / TILE_WIDTH), 0, tile_shape.x)),
        uint(clamp(int((uv.y - radii) / TILE_HEIGHT), 0, tile_shape.y)),
        uint(clamp(int((uv.x + radii + TILE_WIDTH - 1) / TILE_WIDTH), 0, tile_shape.x)),
        uint(clamp(int((uv.y + radii + TILE_HEIGHT - 1) / TILE_HEIGHT), 0, tile_shape.y)));

    uint num_tiles_overlap = (bounding_box.z - bounding_box.x) * (bounding_box.w - bounding_box.y);
    if (num_tiles_overlap == 0) {
        return;
    }
    vertex_attribute_buffer[index].aabb            = bounding_box;
    tile_overlap_buffer[index]                     = num_tiles_overlap;
    vertex_attribute_buffer[index].depth           = p_view.z;
    vertex_attribute_buffer[index].color_radii.w   = radii;
    vertex_attribute_buffer[index].color_radii.xyz = compute_sh(index);
    vertex_attribute_buffer[index].uv              = uv;
    vertex_attribute_buffer[index].magic           = MAGIC;
}