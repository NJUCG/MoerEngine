
#include "cuda_in_raster.h"

#include <cassert>
#include <cstdio>
#include <cublas_v2.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cusolverDn.h>
#include <iostream>
#include <texture_indirect_functions.h>
#include <type_traits>
#include <vector_functions.h>
#include <vector_types.h>

#include "common/helper_math.h"

namespace Moer::Cuda {

// 添加类型转换辅助函数
template<typename T>
__device__ T convert_float_to_type(float value);

template<>
__device__ float convert_float_to_type<float>(float value) {
    return value;
}

template<>
__device__ __half convert_float_to_type<__half>(float value) {
    return __float2half(value);
}

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

__global__ void d_test_2_kernel(
    cudaSurfaceObject_t* surfMipMapArray,
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

            unsigned int pix =
                surf2Dread<unsigned int>(surfMipMapArray[mipLevelIdx], x * sizeof(unsigned int), y);
            float4 input = rgbaIntToFloat(pix);

            // 创建棋盘格模式
            float4 color;
            if ((x / 32 + y / 32) % 2 == 0) {
                color = make_float4(1.0f, 0.0f, 0.0f, 1.0f); // 红色
            } else {
                color = make_float4(0.0f, 1.0f, 0.0f, 1.0f); // 绿色
            }

            color = input + debug_param * (color - input);

            unsigned int packedColor = rgbaFloatToInt(color);
            surf2Dwrite(packedColor, surfMipMapArray[mipLevelIdx], x * sizeof(unsigned int), y);
        }
    }
}

// // 模板化的单通道复制函数
// template<typename T>
// __global__ void CopySurfaceToBuffer_Resize_Single_Template(
//     cudaSurfaceObject_t surface,
//     T*                  output_buffer,
//     int                 src_width,
//     int                 src_height,
//     int                 dst_width,
//     int                 dst_height,
//     int                 channel_idx = 0,
//     int                 batch_idx   = 0
// ) {
//     int dst_x = blockIdx.x * blockDim.x + threadIdx.x;
//     int dst_y = blockIdx.y * blockDim.y + threadIdx.y;

//     if (dst_x < dst_width && dst_y < dst_height) {
//         // 计算在源纹理中的浮点坐标
//         float src_x_f = (dst_x + 0.5f) * src_width / dst_width - 0.5f;
//         float src_y_f = (dst_y + 0.5f) * src_height / dst_height - 0.5f;

//         // 简单的最近邻采样
//         int src_x = __float2int_rn(src_x_f);
//         int src_y = __float2int_rn(src_y_f);

//         // 边界检查
//         src_x = max(0, min(src_x, src_width - 1));
//         src_y = max(0, min(src_y, src_height - 1));

//         // 从 surface 读取像素
//         float4 pixel = surf2Dread<float4>(surface, src_x * sizeof(float4), src_y);

//         float value;
//         switch (channel_idx) {
//             case 0:
//                 value = pixel.x;
//                 break; // R
//             case 1:
//                 value = pixel.y;
//                 break; // G
//             case 2:
//                 value = pixel.z;
//                 break; // B
//             case 3:
//                 value = pixel.w;
//                 break; // A
//             default:
//                 value = 0.0f;
//                 break;
//         }

//         // NCHW 格式: [batch, 1, height, width]
//         int idx            = batch_idx * dst_height * dst_width + dst_y * dst_width + dst_x;
//         output_buffer[idx] = convert_float_to_type<T>(value);
//     }
// }

// MARK: Tex/Buf拷贝与转换

// 添加类型转换辅助函数
template<typename SurfaceEType>
__device__ float4 convertToFloat4(const SurfaceEType& t);

// 特化版本
template<>
__device__ float4 convertToFloat4<float1>(const float1& t) {
    return make_float4(t.x, 0.0f, 0.0f, 0.0f);
}

template<>
__device__ float4 convertToFloat4<float2>(const float2& t) {
    return make_float4(t.x, t.y, 0.0f, 0.0f);
}

template<>
__device__ float4 convertToFloat4<float4>(const float4& t) {
    return t;
}

template<>
__device__ float4 convertToFloat4<uchar1>(const uchar1& t) {
    float norm = t.x / 255.0f;
    return make_float4(norm, 0.0f, 0.0f, 0.0f);
}

template<>
__device__ float4 convertToFloat4<uchar4>(const uchar4& t) {
    return make_float4(t.x / 255.0f, t.y / 255.0f, t.z / 255.0f, t.w / 255.0f);
}

template<>
__device__ float4 convertToFloat4<__half2>(const __half2& t) {
    return make_float4(t.x, t.y, 0.0f, 0.0f);
}

// MARK: - kernel

// 模板化的多通道复制函数
template<typename T, typename SurfaceEType>
__global__ void CopySurfaceToBuffer_Resize_NCHW_Template(
    cudaSurfaceObject_t* surface,
    T*                   output_buffer,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels,
    int                  batch_idx = 0
) {
    int dst_x = blockIdx.x * blockDim.x + threadIdx.x;
    int dst_y = blockIdx.y * blockDim.y + threadIdx.y;

    if (dst_x < dst_width && dst_y < dst_height) {
        // 计算在源纹理中的浮点坐标
        float src_x_f = (dst_x + 0.5f) * src_width / dst_width - 0.5f;
        float src_y_f = (dst_y + 0.5f) * src_height / dst_height - 0.5f;

        // 简单的最近邻采样
        int src_x = __float2int_rn(src_x_f);
        int src_y = __float2int_rn(src_y_f);

        // 边界检查
        src_x = max(0, min(src_x, src_width - 1));
        src_y = max(0, min(src_y, src_height - 1));

        // 从 surface[0] 读取像素
        // - 问题：不同纹理格式不一样（比如R8G8B8A8、R8、R8G8）

        SurfaceEType t2;

        // surf2Dread不支持__half2，所以只能当作uint32_t读取
        if constexpr (std::is_same_v<SurfaceEType, __half2>) {
            uint32_t h_raw = surf2Dread<uint32_t>(surface[0], src_x * sizeof(uint32_t), src_y);

            t2 = *reinterpret_cast<SurfaceEType*>(&h_raw);

        } else {
            t2 = surf2Dread<SurfaceEType>(surface[0], src_x * sizeof(SurfaceEType), src_y);
        }

        float4 t = convertToFloat4(t2);

        float4 pixel = float4(0.0f);

        if (channels == 4) {
            pixel.x = t.x;
            pixel.y = t.y;
            pixel.z = t.z;
            pixel.w = t.w;
        } else if (channels == 3) {
            pixel.x = t.x;
            pixel.y = t.y;
            pixel.z = t.z;
        } else if (channels == 2) {
            pixel.x = t.x;
            pixel.y = t.y;
        } else if (channels == 1) {
            pixel.x = t.x; // cuda支持float1，可以通过t.x访问值
        }

        // 转换为 NCHW 格式
        int base_idx = batch_idx * channels * dst_height * dst_width + dst_y * dst_width + dst_x;

        if (channels >= 1)
            output_buffer[base_idx + 0 * dst_height * dst_width] = convert_float_to_type<T>(pixel.x); // R
        if (channels >= 2)
            output_buffer[base_idx + 1 * dst_height * dst_width] = convert_float_to_type<T>(pixel.y); // G
        if (channels >= 3)
            output_buffer[base_idx + 2 * dst_height * dst_width] = convert_float_to_type<T>(pixel.z); // B
        if (channels >= 4)
            output_buffer[base_idx + 3 * dst_height * dst_width] = convert_float_to_type<T>(pixel.w); // A
    }
}

// MARK: - wrapper

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

void d_test_2(
    dim3                 blocksPerGrid,
    dim3                 threadsPerBlock,
    cudaStream_t         streamToRun,
    cudaSurfaceObject_t* surfMipMapArray,
    size_t               baseWidth,
    size_t               baseHeight,
    size_t               mipLevels,
    float                color
) {
    d_test_2_kernel<<<blocksPerGrid, threadsPerBlock, 0, streamToRun>>>(
        surfMipMapArray, baseWidth, baseHeight, mipLevels, color
    );
}

void CopySurfaceToBuffer_Resize_NCHW_Half_Uchar4(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
) {
    assert(1 <= channels && channels <= 4);
    CopySurfaceToBuffer_Resize_NCHW_Template<__half, uchar4><<<gridSize, blockSize, 0, stream>>>(
        surface, output_buffer, src_width, src_height, dst_width, dst_height, channels, 0
    );
}

void CopySurfaceToBuffer_Resize_NCHW_Half_Uchar1(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
) {
    assert(1 <= channels && channels <= 4);
    CopySurfaceToBuffer_Resize_NCHW_Template<__half, uchar1><<<gridSize, blockSize, 0, stream>>>(
        surface, output_buffer, src_width, src_height, dst_width, dst_height, channels, 0
    );
}

void CopySurfaceToBuffer_Resize_NCHW_Half_Float1(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
) {
    assert(1 <= channels && channels <= 4);
    CopySurfaceToBuffer_Resize_NCHW_Template<__half, float1><<<gridSize, blockSize, 0, stream>>>(
        surface, output_buffer, src_width, src_height, dst_width, dst_height, channels, 0
    );
}

void CopySurfaceToBuffer_Resize_NCHW_Half_Float4(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
) {
    assert(1 <= channels && channels <= 4);
    CopySurfaceToBuffer_Resize_NCHW_Template<__half, float4><<<gridSize, blockSize, 0, stream>>>(
        surface, output_buffer, src_width, src_height, dst_width, dst_height, channels, 0
    );
}

void CopySurfaceToBuffer_Resize_NCHW_Half_Half2(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              output_buffer,
    int                  src_width,
    int                  src_height,
    int                  dst_width,
    int                  dst_height,
    int                  channels
) {
    assert(1 <= channels && channels <= 4);
    CopySurfaceToBuffer_Resize_NCHW_Template<__half, __half2><<<gridSize, blockSize, 0, stream>>>(
        surface, output_buffer, src_width, src_height, dst_width, dst_height, channels, 0
    );
}

// MARK: feature可视化

// ======== 简单随机数生成器：线性同余发生器 (LCG) ========

__device__ unsigned int lcg_random(unsigned int& state) {
    state = 1664525u * state + 1013904223u;
    return state;
}

__device__ float rand01(unsigned int& state) {
    return (lcg_random(state) & 0x00FFFFFF) / float(0x01000000);
}

// ======== Kernel: 每个线程生成 256 个 half 随机数 ========

__global__ void FillRandomHalf_Kernel(__half* buffer, int N, unsigned int seed) {
    int tid   = blockIdx.x * blockDim.x + threadIdx.x;
    int start = tid * RANDOMS_PER_THREAD;

    if (start >= N)
        return;

    unsigned int state = seed ^ tid;

#pragma unroll
    for (int j = 0; j < RANDOMS_PER_THREAD; ++j) {
        int idx = start + j;
        if (idx >= N)
            break;

        float r     = rand01(state);
        buffer[idx] = __float2half(r);
    }
}

void FillRandomHalf(
    dim3         gridSize,
    dim3         blockSize,
    cudaStream_t stream,
    __half*      buffer,
    size_t       n,
    unsigned int seed
) {
    FillRandomHalf_Kernel<<<gridSize, blockSize, 0, stream>>>(buffer, static_cast<int>(n), seed);
}

// 不考虑mipmap
__global__ void VisualizeFeatureBuf_Kernel(
    cudaSurfaceObject_t* surface,
    __half*              feature_buffer, // [must 1, must 19, src_width, src_height]
    int                  src_width,
    int                  src_height,
    int                  src_channels,
    int                  dst_width,
    int                  dst_height,
    float                debug_param
) {
    int dst_x = blockIdx.x * blockDim.x + threadIdx.x;
    int dst_y = blockIdx.y * blockDim.y + threadIdx.y;

    if (dst_x < dst_width && dst_y < dst_height) {
        // 计算在源特征图中的坐标（最近邻采样）
        float src_x_f = (dst_x + 0.5f) * src_width / dst_width - 0.5f;
        float src_y_f = (dst_y + 0.5f) * src_height / dst_height - 0.5f;

        int src_x = __float2int_rn(src_x_f);
        int src_y = __float2int_rn(src_y_f);

        // 边界检查
        src_x = max(0, min(src_x, src_width - 1));
        src_y = max(0, min(src_y, src_height - 1));

        int base_idx = src_y * src_width + src_x; // batch=0固定

        float r, g, b;
        if (src_channels >= 3) {
            r = __half2float(feature_buffer[0 * src_height * src_width + base_idx]); // 第0通道
            g = __half2float(feature_buffer[1 * src_height * src_width + base_idx]); // 第1通道
            b = __half2float(feature_buffer[2 * src_height * src_width + base_idx]); // 第2通道
        } else if (src_channels == 2) {
            r = __half2float(feature_buffer[0 * src_height * src_width + base_idx]);
            g = __half2float(feature_buffer[1 * src_height * src_width + base_idx]);
            b = 0.0f;
        } else if (src_channels == 1) {
            r = g = b = __half2float(feature_buffer[0 * src_height * src_width + base_idx]);
        }

        // 可选：对剩余通道做处理（这里简单设置alpha为1）
        float a = 1.0f;

        // 归一化到[0,1]范围（根据实际特征值范围调整）
        r = __saturatef(r);
        g = __saturatef(g);
        b = __saturatef(b);

        // 转换为uchar4格式写入surface
        uchar4 pixel = make_uchar4(
            (unsigned char)(r * 255.0f),
            (unsigned char)(g * 255.0f),
            (unsigned char)(b * 255.0f),
            (unsigned char)(a * 255.0f)
        );

        // blend with input image

        uchar4 input_raw = surf2Dread<uchar4>(surface[0], dst_x * sizeof(uchar4), dst_y);

        uchar4 result = make_uchar4(
            (unsigned char)(input_raw.x + debug_param * (pixel.x - input_raw.x)),
            (unsigned char)(input_raw.y + debug_param * (pixel.y - input_raw.y)),
            (unsigned char)(input_raw.z + debug_param * (pixel.z - input_raw.z)),
            255
        );

        surf2Dwrite(result, surface[0], dst_x * sizeof(uchar4), dst_y);
    }
}

void VisualizeFeatureBuf(
    dim3                 gridSize,
    dim3                 blockSize,
    cudaStream_t         stream,
    cudaSurfaceObject_t* surface,
    __half*              feature_buffer,
    int                  src_width,
    int                  src_height,
    int                  src_channels,
    int                  dst_width,
    int                  dst_height,
    float                debug_param
) {
    VisualizeFeatureBuf_Kernel<<<gridSize, blockSize, 0, stream>>>(
        surface, feature_buffer, src_width, src_height, src_channels, dst_width, dst_height, debug_param
    );
}

} // namespace Moer::Cuda