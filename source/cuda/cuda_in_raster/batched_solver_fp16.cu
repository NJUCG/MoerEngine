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
#include <vector>
#include <vector_functions.h>
#include <vector_types.h>

#include "common/helper_math.h"

namespace Moer::Cuda {

#define CUDA_CHECK(call)                                                                                  \
    do {                                                                                                  \
        cudaError_t e = (call);                                                                           \
        if (e != cudaSuccess) {                                                                           \
            std::cerr << "CUDA Error: " << cudaGetErrorString(e) << " at " << __FILE__ << ":" << __LINE__ \
                      << std::endl;                                                                       \
            std::exit(EXIT_FAILURE);                                                                      \
        }                                                                                                 \
    } while (0)

#define CUSOLVER_CHECK(call)                                                                            \
    do {                                                                                                \
        cusolverStatus_t s = (call);                                                                    \
        if (s != CUSOLVER_STATUS_SUCCESS) {                                                             \
            std::cerr << "cuSOLVER Error: " << s << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
            std::exit(EXIT_FAILURE);                                                                    \
        }                                                                                               \
    } while (0)

// batched_solver_fp16.cu
// Compile with: nvcc -lcusolver -lcublas batched_solver_fp16.cu -o batched_solver_fp16

// ---- kernels ----

// half -> float (contiguous)
__global__ void HalfToFloatKernel(const __half* __restrict__ src, float* __restrict__ dst, int N) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < N)
        dst[i] = __half2float(src[i]);
}

// float -> half (contiguous)
__global__ void FloatToHalfKernel(const float* __restrict__ src, __half* __restrict__ dst, int N) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < N)
        dst[i] = __float2half(src[i]);
}

// Add eps to diagonal for each batch: A is B x (D*D) packed row-major per matrix
__global__ void AddEpsBatchedKernel(float* A, int B, int D, float eps) {
    int b = blockIdx.x * blockDim.x + threadIdx.x;
    if (b >= B)
        return;
    float* Ab = A + (size_t)b * D * D;
    for (int i = 0; i < D; ++i) {
        Ab[i * D + i] += eps;
    }
}

// build device pointer array: ptrs[b] = base + b * strideElements
__global__ void BuildPtrArrayKernel(float** ptrs, float* base, int B, int strideElems) {
    int b = blockIdx.x * blockDim.x + threadIdx.x;
    if (b >= B)
        return;
    ptrs[b] = base + (size_t)b * strideElems;
}

// same for XTY where stride = D*M
// (reuse same kernel above, since strideElems is generic)

// ---- Host function ----

static constexpr bool IS_VERBOSE = false; // 如果需要输出错误信息，可以将此设置为true

/*
  Solve for each batch:
    (XTX_b + eps * I) * W_b = XTY_b
  Inputs (on GPU):
    d_XTX_h: __half*  size = B * D * D  (stored contiguous per batch)
    d_XTY_h: __half*  size = B * D * M
  Output (on GPU, overwritten):
    d_W_h:   __half*  size = B * D * M
*/
void SolveBatchedFXP16(
    const cudaStream_t& stream_to_run,
    cusolverDnHandle_t  cusolver,
    int                 B,
    int                 D,
    int                 M,
    const __half*       d_XTX_h, // device pointer
    const __half*       d_XTY_h, // device pointer
    __half*             d_W_h,   // device pointer (output)
    float               eps
) {
    assert(B > 0 && D > 0 && M > 0);

    const int threads = 256;
    // 1) allocate FP32 temp buffers
    float *d_XTX_f = nullptr, *d_XTY_f = nullptr, *d_W_f = nullptr;
    CUDA_CHECK(cudaMalloc(&d_XTX_f, (size_t)B * D * D * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&d_XTY_f, (size_t)B * D * M * sizeof(float)));
    CUDA_CHECK(cudaMalloc(&d_W_f, (size_t)B * D * M * sizeof(float)));

    // 2) half->float convert
    int blocksA = (B * D * D + threads - 1) / threads;
    int blocksB = (B * D * M + threads - 1) / threads;
    HalfToFloatKernel<<<blocksA, threads, 0, stream_to_run>>>(d_XTX_h, d_XTX_f, B * D * D);
    HalfToFloatKernel<<<blocksB, threads, 0, stream_to_run>>>(d_XTY_h, d_XTY_f, B * D * M);
    CUDA_CHECK(cudaGetLastError());

    // 3) add eps on diag for each batch
    int blocksBatch = (B + threads - 1) / threads;
    AddEpsBatchedKernel<<<blocksBatch, threads, 0, stream_to_run>>>(d_XTX_f, B, D, eps);
    CUDA_CHECK(cudaGetLastError());

    // 4) build device pointer arrays (for batched APIs)
    float** d_XTX_ptrs = nullptr;
    float** d_XTY_ptrs = nullptr;
    CUDA_CHECK(cudaMalloc(&d_XTX_ptrs, B * sizeof(float*)));
    CUDA_CHECK(cudaMalloc(&d_XTY_ptrs, B * sizeof(float*)));

    // stride in elements:
    int strideXTX = D * D;
    int strideXTY = D * M;

    // Launch kernel to populate pointer arrays
    BuildPtrArrayKernel<<<blocksBatch, threads, 0, stream_to_run>>>(d_XTX_ptrs, d_XTX_f, B, strideXTX);
    BuildPtrArrayKernel<<<blocksBatch, threads, 0, stream_to_run>>>(d_XTY_ptrs, d_XTY_f, B, strideXTY);
    CUDA_CHECK(cudaGetLastError());

    // 5) allocate info array
    int* d_info = nullptr;
    CUDA_CHECK(cudaMalloc(&d_info, B * sizeof(int)));
    CUDA_CHECK(cudaMemset(d_info, 0, B * sizeof(int)));

    // 6) batched Cholesky (upper) and solve
    // Note: cusolverDnSpotrfBatched expects pointer to device array of pointers
    CUSOLVER_CHECK(cusolverDnSpotrfBatched(cusolver, CUBLAS_FILL_MODE_UPPER, D, d_XTX_ptrs, D, d_info, B));

    // check info
    std::vector<int> h_info(B);
    CUDA_CHECK(cudaMemcpy(h_info.data(), d_info, B * sizeof(int), cudaMemcpyDeviceToHost));
    int failures = 0;
    for (int i = 0; i < B; ++i)
        if (h_info[i] != 0)
            ++failures;
    if (IS_VERBOSE && failures > 0) {
        std::cerr << "Warning: " << failures << " batches failed in cholesky (info!=0). First few info: ";
        for (int i = 0; i < std::min(B, 8); ++i)
            std::cerr << h_info[i] << " ";
        std::cerr << std::endl;
        // Note: we continue; user can decide how to handle
    }

    // Solve using spotrsBatched: A (factorized) * X = B  (A is in d_XTX_ptrs, B is d_XTY_ptrs)
    CUSOLVER_CHECK(cusolverDnSpotrsBatched(
        cusolver, CUBLAS_FILL_MODE_UPPER, D, M, (float**)d_XTX_ptrs, D, d_XTY_ptrs, D, d_info, B
    ));

    // check info again
    CUDA_CHECK(cudaMemcpy(h_info.data(), d_info, B * sizeof(int), cudaMemcpyDeviceToHost));
    failures = 0;
    for (int i = 0; i < B; ++i)
        if (h_info[i] != 0)
            ++failures;
    if (IS_VERBOSE && failures > 0) {
        std::cerr << "Warning: " << failures << " batches failed in spotrs (info!=0)." << std::endl;
    }

    // 7) XTY_f now contains solutions for each batch; copy to d_W_f (they already are in place)
    // If we want explicit copy:
    CUDA_CHECK(cudaMemcpy(d_W_f, d_XTY_f, (size_t)B * D * M * sizeof(float), cudaMemcpyDeviceToDevice));

    // 8) float -> half
    FloatToHalfKernel<<<blocksB, threads, 0, stream_to_run>>>(d_W_f, d_W_h, B * D * M);
    CUDA_CHECK(cudaGetLastError());

    // 9) cleanup
    CUDA_CHECK(cudaFree(d_XTX_f));
    CUDA_CHECK(cudaFree(d_XTY_f));
    CUDA_CHECK(cudaFree(d_W_f));
    CUDA_CHECK(cudaFree(d_XTX_ptrs));
    CUDA_CHECK(cudaFree(d_XTY_ptrs));
    CUDA_CHECK(cudaFree(d_info));
}

} // namespace Moer::Cuda