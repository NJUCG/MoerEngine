#ifndef MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H
#define MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H

#ifdef __cplusplus
#include "misc/Traits.h"
namespace Moer::Render {
#else
namespace Moer {
#endif

    struct EnvironmentMapParams {
    };

    struct PreprocessEnvironmentMapParams {
        uint2 src_size;
        uint  src_mip_level;
        uint  num_mip_levels;
    };

#ifdef __cplusplus
}
#else
}
#endif

#endif//MOER_SHARED_LIGHTING_SHADER_PARAMETERS_H