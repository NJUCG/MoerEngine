#pragma once

#ifdef CONST
#undef CONST
#endif

#ifdef __cplusplus
#define CONST constexpr
#include "misc/Traits.h"

namespace Moer::Render {
#else
#define CONST const
namespace Moer {
#endif

// MARK: Main Content Begin

struct CopyPassBindlessParam {
    uint input_image;
    uint padding0;
    uint padding1;
    uint padding2;
};

// MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST
