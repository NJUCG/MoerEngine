#include "core/common/Histogram.hlsli"

static const uint kBinCount = 256;
static const uint kPointWeight = 64;

bool CheckContribution(float histogram_bin, uint2 expected_weights) {
    uint2 weights = Moer::GetHistogramBinWeights(histogram_bin, kPointWeight);
    uint left_bin = uint(floor(histogram_bin));
    uint right_bin = left_bin + 1;
    uint retained_weight = weights.x;
    float weighted_bin = float(left_bin * weights.x);
    if (right_bin < kBinCount) {
        retained_weight += weights.y;
        weighted_bin += float(right_bin * weights.y);
    }
    return all(weights == expected_weights) && retained_weight == kPointWeight &&
           weighted_bin == histogram_bin * kPointWeight;
}

float4 main() : SV_Target {
    uint failures = 0;
    failures += !CheckContribution(0.0, uint2(64, 0));
    failures += !CheckContribution(42.0, uint2(64, 0));
    failures += !CheckContribution(42.25, uint2(48, 16));
    failures += !CheckContribution(42.5, uint2(32, 32));
    failures += !CheckContribution(42.75, uint2(16, 48));
    failures += !CheckContribution(254.984375, uint2(1, 63));
    failures += !CheckContribution(255.0, uint2(64, 0));
    uint2 highest_bin_weights = Moer::GetHistogramBinWeights(255.0, kPointWeight);
    return float4(failures, highest_bin_weights, highest_bin_weights.x);
}
