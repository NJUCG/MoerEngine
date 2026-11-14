#ifndef MOER_ENGINE_CLIPMAP_H
#define MOER_ENGINE_CLIPMAP_H

#include "Core.h"
#include "misc/Types.h"
namespace Moer {
class ClipMap {
public:
    static constexpr uint32 max_level_cnt = 10;

private:
    uint32 base_level;
    uint32 resolution;
    float base_block_size;

    float3 current_center;
};
struct ClipMapLevel {
    float4 center;
};
}; // namespace Moer

#endif