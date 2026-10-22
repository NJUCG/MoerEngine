#ifndef MOER_GAUSSIAN_MATH_HLSLI
#define MOER_GAUSSIAN_MATH_HLSLI
#include "shared/gaussian/GaussianRasterParameters.h"

float3x3 GaussianRotation(float4 q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return float3x3(
        1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w),
        2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w),
        2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y));
}

// SH uses the asset's local viewing direction. Instance transforms rotate the appearance with the asset.
float3 GaussianSH(Moer::GGaussian gaussian, float3 direction, uint degree) {
    float x = direction.x, y = direction.y, z = direction.z;
    float3 color = 0.28209479177387814 * gaussian.sh[0].xyz;
    if (degree >= 1) {
        color += 0.4886025119029199 * (-y * gaussian.sh[1].xyz + z * gaussian.sh[2].xyz - x * gaussian.sh[3].xyz);
    }
    if (degree >= 2) {
        color += 1.0925484305920792 * x*y * gaussian.sh[4].xyz;
        color -= 1.0925484305920792 * y*z * gaussian.sh[5].xyz;
        color += 0.31539156525252005 * (2*z*z-x*x-y*y) * gaussian.sh[6].xyz;
        color -= 1.0925484305920792 * x*z * gaussian.sh[7].xyz;
        color += 0.5462742152960396 * (x*x-y*y) * gaussian.sh[8].xyz;
    }
    if (degree >= 3) {
        color -= 0.5900435899266435 * y*(3*x*x-y*y) * gaussian.sh[9].xyz;
        color += 2.890611442640554 * x*y*z * gaussian.sh[10].xyz;
        color -= 0.4570457994644658 * y*(4*z*z-x*x-y*y) * gaussian.sh[11].xyz;
        color += 0.3731763325901154 * z*(2*z*z-3*x*x-3*y*y) * gaussian.sh[12].xyz;
        color -= 0.4570457994644658 * x*(4*z*z-x*x-y*y) * gaussian.sh[13].xyz;
        color += 1.445305721320277 * z*(x*x-y*y) * gaussian.sh[14].xyz;
        color -= 0.5900435899266435 * x*(x*x-3*y*y) * gaussian.sh[15].xyz;
    }
    return max(color + 0.5, 0.0);
}

uint GaussianSaturatedAdd(uint lhs, uint rhs, uint capacity) {
    // Host bounds capacity to <= 2^24, so adding two saturated values cannot wrap.
    return min(lhs + rhs, capacity + 1);
}
#endif
