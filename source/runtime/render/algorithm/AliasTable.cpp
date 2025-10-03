#include "algorithm/AliasTable.h"
#include "misc/Traits.h"

namespace Moer::Render::Algo {
AliasTable::AliasTable(std::span<const float> _src_value) {
    uint n = _src_value.size();
    prob.resize(n);
    alias.resize(n);
    Array<float> src_value(_src_value.begin(), _src_value.end());
    Array<int>   small, large;

    small.reserve(n);
    large.reserve(n);

    for (uint i = 0; i < n; i++) {
        src_value[i] *= n;
        if (src_value[i] < 1.0f) {
            small.push_back(i);
        } else {
            large.push_back(i);
        }
    }

    while (!small.empty() && !large.empty()) {
        int l = small.back();
        small.pop_back();
        int g = large.back();
        large.pop_back();
        prob[l]      = src_value[l];
        alias[l]     = g;
        src_value[g] = src_value[g] + src_value[l] - 1.0f;
        if (src_value[g] < 1.0f) {
            small.push_back(g);
        } else {
            large.push_back(g);
        }
    }

    while (!small.empty()) {
        prob[small.back()] = 1.0f;
        small.pop_back();
    }

    while (!large.empty()) {
        prob[large.back()] = 1.0f;
        large.pop_back();
    }
}
} // namespace Moer::Render::Algo