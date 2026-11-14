#ifndef MOER_HISTOGRAM_HLSLI
#define MOER_HISTOGRAM_HLSLI

namespace Moer {

// x weights the floor bin; y weights its right neighbor. Integer coordinates
// keep the full point weight in the floor bin, including the highest bin.
uint2 GetHistogramBinWeights(float histogram_bin, uint point_weight) {
    uint right_weight = uint(frac(histogram_bin) * point_weight);
    return uint2(point_weight - right_weight, right_weight);
}

} // namespace Moer

#endif
