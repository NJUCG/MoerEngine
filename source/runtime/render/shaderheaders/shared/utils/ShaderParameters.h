#ifndef MOER_SHARED_UTILS_SHADER_PARAMETERS_H
#define MOER_SHARED_UTILS_SHADER_PARAMETERS_H

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif

struct BuildMipsParam {
    uint2 src_size;
    uint  src_mip_level;
    uint  num_mip_levels;
};

struct GenLowDiscrepancySequenceParam {
    uint num_samples;
    uint num_dimensions;
};

struct ShowTextureParams {
    uint2 dst_dim;
    uint  bdls_handle;
    uint  mip_level;
    bool  use_bindless;
};

#ifdef __cplusplus
}
#else
}
#endif

#endif //MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H