#pragma once

#include "cuda_common.h"

namespace Moer { namespace Cuda {

MOER_CUDA_API void cuda_reduction_kernel(
    uint32  blocksPerGrid,
    uint32  threadsPerBlock,
    uint32  CountPerThread,
    uint64* dData,
    int     n,
    uint64* dOut
);

}} // namespace Moer::Cuda