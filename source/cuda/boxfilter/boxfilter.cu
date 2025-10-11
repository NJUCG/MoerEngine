
#include "boxfilter.h"

#include <cstdio>
#include <cuda_runtime.h>
#include <texture_indirect_functions.h>
#include <vector_functions.h>
#include <vector_types.h>

#include "common/helper_math.h"

namespace Moer::Cuda {

// convert floating point rgba color to 32-bit integer
__device__ unsigned int rgbaFloatToInt(float4 rgba) {
    rgba.x = __saturatef(rgba.x); // clamp to [0.0, 1.0]
    rgba.y = __saturatef(rgba.y);
    rgba.z = __saturatef(rgba.z);
    rgba.w = __saturatef(rgba.w);
    return ((unsigned int)(rgba.w * 255.0f) << 24) | ((unsigned int)(rgba.z * 255.0f) << 16) |
           ((unsigned int)(rgba.y * 255.0f) << 8) | ((unsigned int)(rgba.x * 255.0f));
}

__device__ float4 rgbaIntToFloat(unsigned int c) {
    float4 rgba;
    rgba.x = (c & 0xff) * 0.003921568627f;         //  /255.0f;
    rgba.y = ((c >> 8) & 0xff) * 0.003921568627f;  //  /255.0f;
    rgba.z = ((c >> 16) & 0xff) * 0.003921568627f; //  /255.0f;
    rgba.w = ((c >> 24) & 0xff) * 0.003921568627f; //  /255.0f;
    return rgba;
}

// row pass using texture lookups
__global__ void d_boxfilter_rgba_x_kernel(
    cudaSurfaceObject_t* dstSurfMipMapArray,
    cudaTextureObject_t  textureMipMapInput,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    int                  filter_radius
) {
    float        scale = 1.0f / (float)((filter_radius << 1) + 1);
    unsigned int y     = blockIdx.x * blockDim.x + threadIdx.x;

    if (y < baseHeight) {
        for (uint32_t mipLevelIdx = 0; mipLevelIdx < mipLevels; mipLevelIdx++) {
            uint32_t width  = (baseWidth >> mipLevelIdx) ? (baseWidth >> mipLevelIdx) : 1;
            uint32_t height = (baseHeight >> mipLevelIdx) ? (baseHeight >> mipLevelIdx) : 1;
            if (y < height && filter_radius < width) {
                float  px = 1.0 / width;
                float  py = 1.0 / height;
                float4 t  = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
                for (int x = -filter_radius; x <= filter_radius; x++) {
                    t += tex2DLod<float4>(textureMipMapInput, x * px, y * py, (float)mipLevelIdx);
                }

                unsigned int dataB = rgbaFloatToInt(t * scale);
                surf2Dwrite(dataB, dstSurfMipMapArray[mipLevelIdx], 0, y);

                for (int x = 1; x < width; x++) {
                    t += tex2DLod<float4>(
                        textureMipMapInput, (x + filter_radius) * px, y * py, (float)mipLevelIdx
                    );
                    t -= tex2DLod<float4>(
                        textureMipMapInput, (x - filter_radius - 1) * px, y * py, (float)mipLevelIdx
                    );
                    unsigned int dataB = rgbaFloatToInt(t * scale);
                    surf2Dwrite(dataB, dstSurfMipMapArray[mipLevelIdx], x * sizeof(uchar4), y);
                }
            }
        }
    }
}

// column pass using coalesced global memory reads
__global__ void d_boxfilter_rgba_y_kernel(
    cudaSurfaceObject_t* dstSurfMipMapArray,
    cudaSurfaceObject_t* srcSurfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    int                  filter_radius
) {
    unsigned int x     = blockIdx.x * blockDim.x + threadIdx.x;
    float        scale = 1.0f / (float)((filter_radius << 1) + 1);

    for (uint32_t mipLevelIdx = 0; mipLevelIdx < mipLevels; mipLevelIdx++) {
        uint32_t width  = (baseWidth >> mipLevelIdx) ? (baseWidth >> mipLevelIdx) : 1;
        uint32_t height = (baseHeight >> mipLevelIdx) ? (baseHeight >> mipLevelIdx) : 1;

        if (x < width && height > filter_radius) {
            float4 t;
            // do left edge
            int          colInBytes = x * sizeof(uchar4);
            unsigned int pixFirst = surf2Dread<unsigned int>(srcSurfMipMapArray[mipLevelIdx], colInBytes, 0);
            t                     = rgbaIntToFloat(pixFirst) * filter_radius;

            for (int y = 0; (y < (filter_radius + 1)) && (y < height); y++) {
                unsigned int pix = surf2Dread<unsigned int>(srcSurfMipMapArray[mipLevelIdx], colInBytes, y);
                t += rgbaIntToFloat(pix);
            }

            unsigned int dataB = rgbaFloatToInt(t * scale);
            surf2Dwrite(dataB, dstSurfMipMapArray[mipLevelIdx], colInBytes, 0);

            for (int y = 1; (y < filter_radius + 1) && ((y + filter_radius) < height); y++) {
                unsigned int pix =
                    surf2Dread<unsigned int>(srcSurfMipMapArray[mipLevelIdx], colInBytes, y + filter_radius);
                t += rgbaIntToFloat(pix);
                t -= rgbaIntToFloat(pixFirst);

                dataB = rgbaFloatToInt(t * scale);
                surf2Dwrite(dataB, dstSurfMipMapArray[mipLevelIdx], colInBytes, y);
            }

            // main loop
            for (int y = (filter_radius + 1); y < (height - filter_radius); y++) {
                unsigned int pix =
                    surf2Dread<unsigned int>(srcSurfMipMapArray[mipLevelIdx], colInBytes, y + filter_radius);
                t += rgbaIntToFloat(pix);

                pix = surf2Dread<unsigned int>(
                    srcSurfMipMapArray[mipLevelIdx], colInBytes, y - filter_radius - 1
                );
                t -= rgbaIntToFloat(pix);

                dataB = rgbaFloatToInt(t * scale);
                surf2Dwrite(dataB, dstSurfMipMapArray[mipLevelIdx], colInBytes, y);
            }

            // do right edge
            unsigned int pixLast =
                surf2Dread<unsigned int>(srcSurfMipMapArray[mipLevelIdx], colInBytes, height - 1);
            for (int y = height - filter_radius; (y < height) && ((y - filter_radius - 1) > 1); y++) {
                t += rgbaIntToFloat(pixLast);
                unsigned int pix = surf2Dread<unsigned int>(
                    srcSurfMipMapArray[mipLevelIdx], colInBytes, y - filter_radius - 1
                );
                t -= rgbaIntToFloat(pix);
                dataB = rgbaFloatToInt(t * scale);
                surf2Dwrite(dataB, dstSurfMipMapArray[mipLevelIdx], colInBytes, y);
            }
        }
    }
}

__global__ void d_test_kernel(
    cudaTextureObject_t  textureMipMapInput,
    cudaSurfaceObject_t* dstSurfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    float                debug_param
) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;

    // if (x == 0 && y == 0) {
    //     printf("CUDA: Texture object = %llu\n", (unsigned long long)textureMipMapInput);
    //     printf("CUDA: Base dimensions = %d x %d\n", (int)baseWidth, (int)baseHeight);
    //     printf("CUDA: Mip levels = %d\n", (int)mipLevels);

    //     // 测试固定坐标
    //     float4 center = tex2DLod<float4>(textureMipMapInput, 0.5f, 0.5f, 0.0f);
    //     printf("Center sample: rgba(%.3f,%.3f,%.3f,%.3f)\n", center.x, center.y, center.z, center.w);
    // }

    for (uint32_t mipLevelIdx = 0; mipLevelIdx < mipLevels; mipLevelIdx++) {
        uint32_t width  = max((size_t)(1), baseWidth >> mipLevelIdx);
        uint32_t height = max((size_t)(1), baseHeight >> mipLevelIdx);

        if (x < width && y < height) {
            float  tex_x = (x + 0.5f) / width;
            float  tex_y = (y + 0.5f) / height;
            float4 input = tex2DLod<float4>(textureMipMapInput, tex_x, tex_y, (float)mipLevelIdx);

            // 创建棋盘格模式
            float4 color;
            if ((x / 32 + y / 32) % 2 == 0) {
                color = make_float4(1.0f, 0.0f, 0.0f, 1.0f); // 红色
            } else {
                color = make_float4(0.0f, 1.0f, 0.0f, 1.0f); // 绿色
            }

            color = input + debug_param * (color - input);

            unsigned int packedColor = rgbaFloatToInt(color);
            surf2Dwrite(packedColor, dstSurfMipMapArray[mipLevelIdx], x * sizeof(unsigned int), y);
        }
    }
}

void d_boxfilter_rgba_x(
    int                  blocksPerGrid,
    int                  threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaSurfaceObject_t* dstSurfMipMapArray,
    cudaTextureObject_t  textureMipMapInput,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    int                  filter_radius
) {
    d_boxfilter_rgba_x_kernel<<<blocksPerGrid, threadsPerBlock, 0, streamToRun>>>(
        dstSurfMipMapArray, textureMipMapInput, baseWidth, baseHeight, mipLevels, filter_radius
    );
}

void d_boxfilter_rgba_y(
    int                  blocksPerGrid,
    int                  threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaSurfaceObject_t* dstSurfMipMapArray,
    cudaSurfaceObject_t* srcSurfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    int                  filter_radius
) {
    d_boxfilter_rgba_y_kernel<<<blocksPerGrid, threadsPerBlock, 0, streamToRun>>>(
        dstSurfMipMapArray, srcSurfMipMapArray, baseWidth, baseHeight, mipLevels, filter_radius
    );
}

void d_test(
    dim3                 blocksPerGrid,
    dim3                 threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaTextureObject_t  textureMipMapInput,
    cudaSurfaceObject_t* dstSurfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    float                color
) {
    d_test_kernel<<<blocksPerGrid, threadsPerBlock, 0, streamToRun>>>(
        textureMipMapInput, dstSurfMipMapArray, baseWidth, baseHeight, mipLevels, color
    );
}

} // namespace Moer::Cuda