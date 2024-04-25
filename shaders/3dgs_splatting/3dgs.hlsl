#ifndef __3DGS_HLSL__
#define __3DGS_HLSL__
#define MAGIC       0x4d415449u
#define TILE_WIDTH  16
#define TILE_HEIGHT 16

static const float SH_C0   = 0.28209479177387814f;
static const float SH_C1   = 0.4886025119029199f;
static const float SH_C2[] = {
    1.0925484305920792f,
    -1.0925484305920792f,
    0.31539156525252005f,
    -1.0925484305920792f,
    0.5462742152960396f};
static const float SH_C3[] = {
    -0.5900435899266435f,
    2.890611442640554f,
    -0.4570457994644658f,
    0.3731763325901154f,
    -0.4570457994644658f,
    1.445305721320277f,
    -0.5900435899266435f};

struct Vertex {
    float4 position;
    float4 scale_opacity;
    float4 rotation;
    float  sh[48];
};

struct VertexAttribute {
    float4 conic_opacity;
    float4 color_radii;
    uint4  aabb;
    float2 uv;
    float  depth;
    uint   magic;
};

float3x3 rotationFromQuaternion(float4 q) {
    float qx = q.y;
    float qy = q.z;
    float qz = q.w;
    float qw = q.x;

    float qx2 = qx * qx;
    float qy2 = qy * qy;
    float qz2 = qz * qz;

    float3x3 rotationMatrix;
    rotationMatrix[0][0] = 1 - 2 * qy2 - 2 * qz2;
    rotationMatrix[0][1] = 2 * qx * qy - 2 * qz * qw;
    rotationMatrix[0][2] = 2 * qx * qz + 2 * qy * qw;

    rotationMatrix[1][0] = 2 * qx * qy + 2 * qz * qw;
    rotationMatrix[1][1] = 1 - 2 * qx2 - 2 * qz2;
    rotationMatrix[1][2] = 2 * qy * qz - 2 * qx * qw;

    rotationMatrix[2][0] = 2 * qx * qz - 2 * qy * qw;
    rotationMatrix[2][1] = 2 * qy * qz + 2 * qx * qw;
    rotationMatrix[2][2] = 1 - 2 * qx2 - 2 * qy2;

    return rotationMatrix;
}

float4x4 inverse(float4x4 m) {
    float n11 = m[0][0], n12 = m[1][0], n13 = m[2][0], n14 = m[3][0];
    float n21 = m[0][1], n22 = m[1][1], n23 = m[2][1], n24 = m[3][1];
    float n31 = m[0][2], n32 = m[1][2], n33 = m[2][2], n34 = m[3][2];
    float n41 = m[0][3], n42 = m[1][3], n43 = m[2][3], n44 = m[3][3];

    float t11 = n23 * n34 * n42 - n24 * n33 * n42 + n24 * n32 * n43 - n22 * n34 * n43 - n23 * n32 * n44 + n22 * n33 * n44;
    float t12 = n14 * n33 * n42 - n13 * n34 * n42 - n14 * n32 * n43 + n12 * n34 * n43 + n13 * n32 * n44 - n12 * n33 * n44;
    float t13 = n13 * n24 * n42 - n14 * n23 * n42 + n14 * n22 * n43 - n12 * n24 * n43 - n13 * n22 * n44 + n12 * n23 * n44;
    float t14 = n14 * n23 * n32 - n13 * n24 * n32 - n14 * n22 * n33 + n12 * n24 * n33 + n13 * n22 * n34 - n12 * n23 * n34;

    float det  = n11 * t11 + n21 * t12 + n31 * t13 + n41 * t14;
    float idet = 1.0f / det;

    float4x4 ret;

    ret[0][0] = t11 * idet;
    ret[0][1] = (n24 * n33 * n41 - n23 * n34 * n41 - n24 * n31 * n43 + n21 * n34 * n43 + n23 * n31 * n44 - n21 * n33 * n44) * idet;
    ret[0][2] = (n22 * n34 * n41 - n24 * n32 * n41 + n24 * n31 * n42 - n21 * n34 * n42 - n22 * n31 * n44 + n21 * n32 * n44) * idet;
    ret[0][3] = (n23 * n32 * n41 - n22 * n33 * n41 - n23 * n31 * n42 + n21 * n33 * n42 + n22 * n31 * n43 - n21 * n32 * n43) * idet;

    ret[1][0] = t12 * idet;
    ret[1][1] = (n13 * n34 * n41 - n14 * n33 * n41 + n14 * n31 * n43 - n11 * n34 * n43 - n13 * n31 * n44 + n11 * n33 * n44) * idet;
    ret[1][2] = (n14 * n32 * n41 - n12 * n34 * n41 - n14 * n31 * n42 + n11 * n34 * n42 + n12 * n31 * n44 - n11 * n32 * n44) * idet;
    ret[1][3] = (n12 * n33 * n41 - n13 * n32 * n41 + n13 * n31 * n42 - n11 * n33 * n42 - n12 * n31 * n43 + n11 * n32 * n43) * idet;

    ret[2][0] = t13 * idet;
    ret[2][1] = (n14 * n23 * n41 - n13 * n24 * n41 - n14 * n21 * n43 + n11 * n24 * n43 + n13 * n21 * n44 - n11 * n23 * n44) * idet;
    ret[2][2] = (n12 * n24 * n41 - n14 * n22 * n41 + n14 * n21 * n42 - n11 * n24 * n42 - n12 * n21 * n44 + n11 * n22 * n44) * idet;
    ret[2][3] = (n13 * n22 * n41 - n12 * n23 * n41 - n13 * n21 * n42 + n11 * n23 * n42 + n12 * n21 * n43 - n11 * n22 * n43) * idet;

    ret[3][0] = t14 * idet;
    ret[3][1] = (n13 * n24 * n31 - n14 * n23 * n31 + n14 * n21 * n33 - n11 * n24 * n33 - n13 * n21 * n34 + n11 * n23 * n34) * idet;
    ret[3][2] = (n14 * n22 * n31 - n12 * n24 * n31 - n14 * n21 * n32 + n11 * n24 * n32 + n12 * n21 * n34 - n11 * n22 * n34) * idet;
    ret[3][3] = (n12 * n23 * n31 - n13 * n22 * n31 + n13 * n21 * n32 - n11 * n23 * n32 - n12 * n21 * n33 + n11 * n22 * n33) * idet;

    return ret;
}

float3x3 getIndentity3x3() {
    return float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1);
}

float determinant(float2x2 mat) {
    return mat._m00 * mat._m11 - mat._m01 * mat._m10;
}

float2x2 adjoint(float2x2 mat) {
    return float2x2(mat._m11, -mat._m01, -mat._m10, mat._m00);
}

float2x2 inverse(float2x2 mat) {
    float    det     = determinant(mat);
    float    det_inv = 1.0 / det;
    float2x2 adj     = adjoint(mat);
    return det_inv * adj;
}

#endif
