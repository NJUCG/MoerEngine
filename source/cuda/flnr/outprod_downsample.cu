#include <algorithm>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <torch/types.h>
#include <vector>

// Forward declaration of gaussian_blur function from gaussian_blur.cu
torch::Tensor gaussian_blur(torch::Tensor input_moments, float sigma, int kernel_radius);
torch::Tensor linalg_solve(torch::Tensor XtX, torch::Tensor XtY, float epsilon, float eta);
torch::Tensor upsample_apply(torch::Tensor coeffs, torch::Tensor guide, int scale_factor);

// Templated CUDA kernel for outer product computation and downsampling
// Optimized with compile-time template parameters Q (guide channels) and C (target channels)
template<
    const int Q  = 1,
    const int C  = 1,
    const int DF = 8,
    const int BM = 32,
    const int BN = 32>
__global__ void outprod_downsample_kernel(
    float* __restrict__ guide,      // Input: Guide channels [H*W*Q]
    float* __restrict__ target,     // Input: Target signal Y [H*W*C]
    float* __restrict__ moment_XtX, // Output: X^T*X moments [H/downsample*W/downsample*(Q+1)*(Q+1)]
    float* __restrict__ moment_XtY, // Output: X^T*Y moments [H/downsample*W/downsample*(Q+1)*C]
    const int H,                    // Image height
    const int W                     // Image width
) {
    // Calculate output position
    // int out_x = blockIdx.x * blockDim.x + threadIdx.x;
    // int out_y = blockIdx.y * blockDim.y + threadIdx.y;
    int bx = blockIdx.x;
    int by = blockIdx.y;
    int tx = threadIdx.x;
    int ty = threadIdx.y;

    int out_H = (H + DF - 1) / DF;
    int out_W = (W + DF - 1) / DF;

    int out_x = bx * blockDim.x + tx;
    int out_y = by * blockDim.y + ty;

    if (out_x >= out_W || out_y >= out_H) return;

    // Calculate input region bounds
    int start_x = out_x * DF;
    int start_y = out_y * DF;
    int end_x   = min(start_x + DF, W);
    int end_y   = min(start_y + DF, H);

    constexpr int Q_plus_1 = Q + 1;

    // Initialize accumulation arrays with compile-time sizes for optimization
    float XtX_accum[Q_plus_1][Q_plus_1] = {0.0f}; // Fixed size based on template parameter
    float XtY_accum[Q_plus_1][C]        = {0.0f}; // Fixed size based on template parameters

    // Accumulate outer products over the downsample region
#pragma unroll
    for (int y = start_y; y < end_y; y++) {
#pragma unroll
        for (int x = start_x; x < end_x; x++) {
            int pixel_idx = y * W + x;

            // Construct augmented guide vector: [1, g1, g2, ..., gQ]
            float augmented_guide[Q_plus_1];
            augmented_guide[0] = 1.0f; // Bias term

// Load guide channels (unrolled for better performance)
#pragma unroll
            for (int q = 0; q < Q; q++) { augmented_guide[q + 1] = guide[pixel_idx * Q + q]; }

            // Load target signal values
            float target_values[C];
#pragma unroll
            for (int c = 0; c < C; c++) { target_values[c] = target[pixel_idx * C + c]; }

// Compute outer product: augmented_guide * augmented_guide^T (unrolled)
#pragma unroll
            for (int i = 0; i < Q_plus_1; i++) {
#pragma unroll
                for (int j = 0; j < Q_plus_1; j++) {
                    XtX_accum[i][j] += augmented_guide[i] * augmented_guide[j];
                }
            }

// Compute cross product: augmented_guide * target_values^T (unrolled)
#pragma unroll
            for (int i = 0; i < Q_plus_1; i++) {
#pragma unroll
                for (int c = 0; c < C; c++) { XtY_accum[i][c] += augmented_guide[i] * target_values[c]; }
            }
        }
    }

    // Store results in global memory
    int   out_idx = out_y * out_W + out_x;
    float inv     = 1.0f / (DF * DF);
#pragma unroll
    for (int i = 0; i < Q_plus_1; i++) {
        // Store X^T*X moment matrix (unrolled)
#pragma unroll
        for (int j = 0; j < Q_plus_1; j++) {
            int XtX_idx         = out_idx * Q_plus_1 * Q_plus_1 + i * Q_plus_1 + j;
            moment_XtX[XtX_idx] = XtX_accum[i][j] * inv;
        }

        // Store X^T*Y moment matrix (unrolled)
#pragma unroll
        for (int c = 0; c < C; c++) {
            int XtY_idx         = out_idx * Q_plus_1 * C + i * C + c;
            moment_XtY[XtY_idx] = XtY_accum[i][c] * inv;
        }
    }
}

// FP16 version of outer product computation and downsampling kernel
template<
    const int Q  = 1,
    const int C  = 1,
    const int DF = 8,
    const int BM = 32,
    const int BN = 32>
__global__ void outprod_downsample_kernel_fp16(
    __half* __restrict__ guide,      // Input: Guide channels [H*W*Q]
    __half* __restrict__ target,     // Input: Target signal Y [H*W*C]
    __half* __restrict__ moment_XtX, // Output: X^T*X moments [H/downsample*W/downsample*(Q+1)*(Q+1)]
    __half* __restrict__ moment_XtY, // Output: X^T*Y moments [H/downsample*W/downsample*(Q+1)*C]
    const int H,                     // Image height
    const int W                      // Image width
) {
    // Calculate output position
    int bx = blockIdx.x;
    int by = blockIdx.y;
    int tx = threadIdx.x;
    int ty = threadIdx.y;

    int out_H = (H + DF - 1) / DF;
    int out_W = (W + DF - 1) / DF;

    int out_x = bx * blockDim.x + tx;
    int out_y = by * blockDim.y + ty;

    if (out_x >= out_W || out_y >= out_H) return;

    // Calculate input region bounds
    int start_x = out_x * DF;
    int start_y = out_y * DF;
    int end_x   = min(start_x + DF, W);
    int end_y   = min(start_y + DF, H);

    constexpr int Q_plus_1 = Q + 1;

    // Initialize accumulation arrays with compile-time sizes for optimization
    float XtX_accum[Q_plus_1][Q_plus_1] = {0.0f}; // Use float for accumulation precision
    float XtY_accum[Q_plus_1][C]        = {0.0f}; // Use float for accumulation precision

    // Accumulate outer products over the downsample region
#pragma unroll
    for (int y = start_y; y < end_y; y++) {
#pragma unroll
        for (int x = start_x; x < end_x; x++) {
            int pixel_idx = y * W + x;

            // Construct augmented guide vector: [1, g1, g2, ..., gQ]
            float augmented_guide[Q_plus_1];
            augmented_guide[0] = 1.0f; // Bias term

// Load guide channels (unrolled for better performance)
#pragma unroll
            for (int q = 0; q < Q; q++) { augmented_guide[q + 1] = __half2float(guide[pixel_idx * Q + q]); }

            // Load target signal values
            float target_values[C];
#pragma unroll
            for (int c = 0; c < C; c++) { target_values[c] = __half2float(target[pixel_idx * C + c]); }

// Compute outer product: augmented_guide * augmented_guide^T (unrolled)
#pragma unroll
            for (int i = 0; i < Q_plus_1; i++) {
#pragma unroll
                for (int j = 0; j < Q_plus_1; j++) {
                    XtX_accum[i][j] += augmented_guide[i] * augmented_guide[j];
                }
            }

// Compute cross product: augmented_guide * target_values^T (unrolled)
#pragma unroll
            for (int i = 0; i < Q_plus_1; i++) {
#pragma unroll
                for (int c = 0; c < C; c++) { XtY_accum[i][c] += augmented_guide[i] * target_values[c]; }
            }
        }
    }

    // Store results in global memory
    int   out_idx = out_y * out_W + out_x;
    float inv     = 1.0f / (DF * DF);
#pragma unroll
    for (int i = 0; i < Q_plus_1; i++) {
        // Store X^T*X moment matrix (unrolled)
#pragma unroll
        for (int j = 0; j < Q_plus_1; j++) {
            int XtX_idx         = out_idx * Q_plus_1 * Q_plus_1 + i * Q_plus_1 + j;
            moment_XtX[XtX_idx] = __float2half(XtX_accum[i][j] * inv);
        }

        // Store X^T*Y moment matrix (unrolled)
#pragma unroll
        for (int c = 0; c < C; c++) {
            int XtY_idx         = out_idx * Q_plus_1 * C + i * C + c;
            moment_XtY[XtY_idx] = __float2half(XtY_accum[i][c] * inv);
        }
    }
}

// PyTorch binding functions
std::tuple<torch::Tensor, torch::Tensor> outprod_downsample(
    torch::Tensor guide,  // [B, H, W, Q]
    torch::Tensor target, // [B, H, W, C]
    int           factor  // 4/8
) {
    // Get tensor dimensions
    int B = guide.size(0); // const = 1
    int H = guide.size(1);
    int W = guide.size(2);
    int Q = guide.size(3);
    int C = target.size(3);

    // Calculate output dimensions
    int       out_H    = (H + factor - 1) / factor;
    int       out_W    = (W + factor - 1) / factor;
    const int Q_plus_1 = Q + 1;

    // Create output tensors with same dtype as input
    torch::Tensor XtX = torch::zeros({B, out_H, out_W, Q_plus_1, Q_plus_1}, guide.options());
    torch::Tensor XtY = torch::zeros({B, out_H, out_W, Q_plus_1, C}, guide.options());

    // Configure CUDA kernel launch parameters
    constexpr int BM = 32;
    constexpr int BN = 32;
    dim3          block_size(BM, BN);
    dim3 grid_size((out_W + block_size.x - 1) / block_size.x, (out_H + block_size.y - 1) / block_size.y);

    // Check if input is fp16
    if (guide.scalar_type() == torch::kFloat16) {
        if (C == 1) {
            switch (Q) {
                case 1:
                    // Launch templated kernel with Q=1, C=1
                    outprod_downsample_kernel_fp16<1, 1, 8, BM, BN><<<grid_size, block_size>>>(
                        reinterpret_cast<__half*>(guide.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(target.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtX.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtY.data_ptr<at::Half>()),
                        H,
                        W
                    );
                    break;
                case 4:
                    // Launch templated kernel with Q=4, C=1
                    outprod_downsample_kernel_fp16<4, 1, 8, BM, BN><<<grid_size, block_size>>>(
                        reinterpret_cast<__half*>(guide.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(target.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtX.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtY.data_ptr<at::Half>()),
                        H,
                        W
                    );
                    break;
            }
        } else {
            switch (Q) {
                case 1:
                    // Launch templated kernel with Q=1, C=3
                    outprod_downsample_kernel_fp16<1, 3, 8, BM, BN><<<grid_size, block_size>>>(
                        reinterpret_cast<__half*>(guide.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(target.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtX.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtY.data_ptr<at::Half>()),
                        H,
                        W
                    );
                    break;
                case 4:
                    // Launch templated kernel with Q=4, C=3
                    outprod_downsample_kernel_fp16<4, 3, 8, BM, BN><<<grid_size, block_size>>>(
                        reinterpret_cast<__half*>(guide.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(target.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtX.data_ptr<at::Half>()),
                        reinterpret_cast<__half*>(XtY.data_ptr<at::Half>()),
                        H,
                        W
                    );
                    break;
            }
        }
    } else {
        // FP32 path
        if (C == 1) {
            switch (Q) {
                case 1:
                    // Launch templated kernel with Q=1, C=1
                    outprod_downsample_kernel<1, 1, 8, BM, BN><<<grid_size, block_size>>>(
                        guide.data_ptr<float>(),
                        target.data_ptr<float>(),
                        XtX.data_ptr<float>(),
                        XtY.data_ptr<float>(),
                        H,
                        W
                    );
                    break;
                case 4:
                    // Launch templated kernel with Q=1, C=1
                    outprod_downsample_kernel<4, 1, 8, BM, BN><<<grid_size, block_size>>>(
                        guide.data_ptr<float>(),
                        target.data_ptr<float>(),
                        XtX.data_ptr<float>(),
                        XtY.data_ptr<float>(),
                        H,
                        W
                    );
                    break;
            }
        } else {
            switch (Q) {
                case 1:
                    // Launch templated kernel with Q=1, C=1
                    outprod_downsample_kernel<1, 3, 8, BM, BN><<<grid_size, block_size>>>(
                        guide.data_ptr<float>(),
                        target.data_ptr<float>(),
                        XtX.data_ptr<float>(),
                        XtY.data_ptr<float>(),
                        H,
                        W
                    );
                    break;
                case 4:
                    // Launch templated kernel with Q=1, C=1
                    outprod_downsample_kernel<4, 3, 8, BM, BN><<<grid_size, block_size>>>(
                        guide.data_ptr<float>(),
                        target.data_ptr<float>(),
                        XtX.data_ptr<float>(),
                        XtY.data_ptr<float>(),
                        H,
                        W
                    );
                    break;
            }
        }
    }

    // Return both moment tensors as a tuple
    return std::make_tuple(XtX.flatten(3), XtY.flatten(3));
}