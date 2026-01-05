#pragma once

#include <MoerCudaAPI.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cusolverDn.h>

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

/**
 * 试了一下，可以直接从cudaSurfaceObject_t读写。那就不需要cudaTextureObject_t了
 * 
 * “这个，不需要了”
 */
MOER_CUDA_API void d_test_2(
    dim3                 blocksPerGrid,
    dim3                 threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaSurfaceObject_t* surfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    float                color
);

// 莫名其妙崩溃
// /**
//  * 这里是template，所以记得在.cu中显式生成对应类型的函数实现
//  */
// template<typename T>
// MOER_CUDA_API void CopySurfaceToBuffer_Resize_NCHW(
//     dim3                gridSize,
//     dim3                blockSize,
//     cudaStream_t        stream,
//     cudaSurfaceObject_t surface,
//     T*                  output_buffer,
//    bool                 use_tone_mapping,
//     int                 src_width,
//     int                 src_height,
//     int                 dst_width,
//     int                 dst_height,
//     int                 channels
// );

MOER_CUDA_API void CopySurfaceToBuffer_Resize_NCHW_Half_Uchar4(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    bool                 use_tone_mapping,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
);

MOER_CUDA_API void CopySurfaceToBuffer_Resize_NCHW_Half_Uchar1(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    bool                 use_tone_mapping,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
);

MOER_CUDA_API void CopySurfaceToBuffer_Resize_NCHW_Half_Float1(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    bool                 use_tone_mapping,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
);

MOER_CUDA_API void CopySurfaceToBuffer_Resize_NCHW_Half_Float4(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    bool                 use_tone_mapping,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
);

MOER_CUDA_API void CopySurfaceToBuffer_Resize_NCHW_Half_Half2(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    bool                 use_tone_mapping,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
);

MOER_CUDA_API void CopySurfaceToBuffer_Resize_NCHW_Half_Half4(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    bool                 use_tone_mapping,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
);

constexpr int RANDOMS_PER_THREAD = 256;

MOER_CUDA_API void FillRandomHalf(
    dim3         gridSize,
    dim3         blockSize,
    cudaStream_t stream,
    __half*      buffer,
    size_t       n,
    unsigned int seed
);

MOER_CUDA_API void VisualizeFeatureBuf(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              feature_buffer,
    bool                 use_tone_mapping,
    int                  src_width,
    int                  src_height,
    int                  src_channels,
    int                  dst_width,
    int                  dst_height,
    float                debug_param
);

/*
  Solve for each batch:
    (XTX_b + eps * I) * W_b = XTY_b
  Inputs (on GPU):
    d_XTX_h: __half*  size = B * D * D  (stored contiguous per batch)
    d_XTY_h: __half*  size = B * D * M
  Output (on GPU, overwritten):
    d_W_h:   __half*  size = B * D * M
*/
MOER_CUDA_API void SolveBatchedFXP16(
    const cudaStream_t& stream_to_run,
    cusolverDnHandle_t  cusolver,
    int                 B,
    int                 D,
    int                 M,
    const __half*       d_XTX_h, // device pointer
    const __half*       d_XTY_h, // device pointer
    __half*             d_W_h,   // device pointer (output)
    float               eps
);

} // namespace Moer::Cuda