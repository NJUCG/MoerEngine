#ifndef MOER_GAUSSIAN_TILE_CULL_HLSLI
#define MOER_GAUSSIAN_TILE_CULL_HLSLI

float GaussianQuadraticLowerBound(float3 conic, float2 delta) {
    float xx = conic.x * delta.x * delta.x;
    float xy = 2 * conic.y * delta.x * delta.y;
    float yy = conic.z * delta.y * delta.y;
    // Thin, rotated ellipses can subtract large terms. Bias toward keeping a tile
    // rather than dropping a contribution due to cancellation/FP32 rounding.
    return xx + xy + yy - 1e-4 * (abs(xx) + abs(xy) + abs(yy) + 1);
}

bool GaussianTileMayContribute(
    float2 center, float3 conic, float cutoff_power, uint2 tile, uint2 resolution
) {
    if (!all(isfinite(conic)) || conic.x <= 0 || conic.z <= 0) return true;
    float2 delta_min = float2(tile * 16) - center;
    float2 delta_max = float2(min(tile * 16 + 15, resolution - 1)) - center;
    if (all(delta_min <= 0) && all(delta_max >= 0)) return true;
    // A positive-definite quadratic reaches its rectangle minimum at the center
    // or on an edge. Continuous pixel-center bounds conservatively enclose every
    // discrete pixel checked by Splat; retain the original tile rectangle too.
    float min_q = GaussianQuadraticLowerBound(
        conic, float2(delta_min.x, clamp(-conic.y * delta_min.x / conic.z, delta_min.y, delta_max.y))
    );
    min_q = min(min_q, GaussianQuadraticLowerBound(
        conic, float2(delta_max.x, clamp(-conic.y * delta_max.x / conic.z, delta_min.y, delta_max.y))
    ));
    min_q = min(min_q, GaussianQuadraticLowerBound(
        conic, float2(clamp(-conic.y * delta_min.y / conic.x, delta_min.x, delta_max.x), delta_min.y)
    ));
    min_q = min(min_q, GaussianQuadraticLowerBound(
        conic, float2(clamp(-conic.y * delta_max.y / conic.x, delta_min.x, delta_max.x), delta_max.y)
    ));
    // alpha = opacity * exp(-q/2); no extra 3-sigma ellipse cutoff is introduced.
    return !isfinite(min_q) || min_q <= 2 * cutoff_power;
}

#endif
