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

    // todo: add your code here

    // MARK: Main Content End

#ifdef __cplusplus
}
#else
}
#endif
#undef CONST
