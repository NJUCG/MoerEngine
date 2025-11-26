#include "cuda_interpo_test.h"

#include "cuda_common.h"

#include <chrono>
#include <cstdio>
#include <cuda_runtime.h>
#include <iostream>
#include <vector>

namespace Moer { namespace Cuda {

static const uint64 TILE = 256;

__global__ void reduction_kernel(const uint64* a, uint64* ans, int n, int CountPerThread) {
    __shared__ uint64 sData[TILE];

    int tid = threadIdx.x;
    int i   = blockIdx.x * blockDim.x * CountPerThread + tid;

    uint64 sum = 0.f;
    for (int j = 0; j < CountPerThread && i < n; j++, i += blockDim.x) { sum += a[i]; }

    sData[tid] = sum;

    __syncthreads();
    // 此时，sData[0~TILE-1]，每个地方都存了2个数的和

    for (int stride = blockDim.x / 2; stride > 0; stride >>= 1) {
        if (tid < stride) {                    // 每次减少一半激活的thread
            sData[tid] += sData[tid + stride]; // 因为是自己管理共享内存，所以这里要自己保证读写不冲突
        }
        __syncthreads();
    }

    if (tid == 0) { // 只有第一个thread执行
        atomicAdd(ans, sData[0]);
    }
}

void cuda_reduction_kernel(
    uint32  blocksPerGrid,
    uint32  threadsPerBlock,
    uint32  CountPerThread,
    uint64* dData,
    int     n,
    uint64* dOut
) {
    reduction_kernel<<<blocksPerGrid, threadsPerBlock>>>(dData, dOut, n, CountPerThread);
}

}} // namespace Moer::Cuda
