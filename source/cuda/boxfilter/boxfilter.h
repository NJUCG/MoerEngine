#pragma once

#include <MoerCudaAPI.h>
#include <cuda_runtime.h>

namespace Moer::Cuda {

MOER_CUDA_API void d_boxfilter_rgba_x(
    int                  blocksPerGrid,
    int                  threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaSurfaceObject_t* dstSurfMipMapArray,
    cudaTextureObject_t  textureMipMapInput,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    int                  filter_radius
);

MOER_CUDA_API void d_boxfilter_rgba_y(
    int                  blocksPerGrid,
    int                  threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaSurfaceObject_t* dstSurfMipMapArray,
    cudaSurfaceObject_t* srcSurfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    int                  filter_radius
);

MOER_CUDA_API void d_test(
    dim3                 blocksPerGrid,
    dim3                 threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaTextureObject_t  textureMipMapInput,
    cudaSurfaceObject_t* dstSurfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    float                color
);

} // namespace Moer::Cuda