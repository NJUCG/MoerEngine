#ifndef MOER_ENGINE_CLIPMAP_H
#define MOER_ENGINE_CLIPMAP_H
#include "Core.h"
namespace Moer {
    class ClipMap {
    public:
        static constexpr uint max_level_cnt = 10;

    private:
        uint  base_level;
        uint  resolution;
        float base_block_size;

        float3 current_center;
    };
    struct ClipMapLevel {
        float4 center;
    };
};// namespace Moer

#endif