
#include <algorithm>
#include <cmath>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <torch/types.h>
#include <vector>

// Templated CUDA kernel for separable Gaussian blur
// Optimized with compile-time template parameters C (channels)

// Helper function for reflect boundary condition
__device__ inline int reflect_coord(int coord, int size) {
    if (coord < 0) {
        return -coord;
    } else if (coord >= size) {
        return 2 * size - 2 - coord;
    }
    return coord;
}

template<
    const int C             = 4,
    const int KERNEL_RADIUS = 3,
    const int BLOCK_SIZE    = 32>
__global__ void gaussian_blur_kernel(
    const float* __restrict__ input, // Input tensor [H*W*C]
    float* __restrict__ output,      // Output tensor [H*W*C]
    const int H,                     // Tensor height
    const int W,                     // Tensor width
    float     sigma                  // Gaussian sigma parameter
) {
    // Thread and block indices
    int tx = threadIdx.x;
    int ty = threadIdx.y;
    int bx = blockIdx.x;
    int by = blockIdx.y;

    // Global coordinates
    int x = bx * blockDim.x + tx;
    int y = by * blockDim.y + ty;

    if (x >= W || y >= H) return;

    // Shared memory for horizontal blur results - with padding for kernel radius
    constexpr int    PADDED_SIZE = BLOCK_SIZE + 2 * KERNEL_RADIUS;
    __shared__ float shared_data[PADDED_SIZE][PADDED_SIZE][C];

    float inv_sigma_sq = 1.0f / (2.0f * sigma * sigma);

    // Pre-compute normalized Gaussian weights
    float gaussian_weights[2 * KERNEL_RADIUS + 1];
    float weight_sum = 0.0f;

// First pass: compute raw weights and sum
#pragma unroll
    for (int i = 0; i <= 2 * KERNEL_RADIUS; i++) {
        int offset          = i - KERNEL_RADIUS;
        gaussian_weights[i] = expf(-offset * offset * inv_sigma_sq);
        weight_sum += gaussian_weights[i];
    }

// Second pass: normalize weights
#pragma unroll
    for (int i = 0; i <= 2 * KERNEL_RADIUS; i++) { gaussian_weights[i] /= weight_sum; }

    // Step 1: Apply horizontal blur and store in shared memory
    // Apply horizontal blur for all channels
    float h_sum[C] = {0.0f};

#pragma unroll
    for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
        int   sample_x = reflect_coord(x + i, W);
        float weight   = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
        for (int c = 0; c < C; c++) {
            int idx = (y * W + sample_x) * C + c;
            h_sum[c] += weight * input[idx];
        }
    }

// Store horizontally blurred result in shared memory
#pragma unroll
    for (int c = 0; c < C; c++) { shared_data[ty + KERNEL_RADIUS][tx + KERNEL_RADIUS][c] = h_sum[c]; }

    // Load halo data for vertical blur
    // Top halo: load data for rows above current block
    if (ty < KERNEL_RADIUS) {
        int global_y    = by * BLOCK_SIZE + ty - KERNEL_RADIUS;
        int reflected_y = reflect_coord(global_y, H);

        float halo_sum[C] = {0.0f};
#pragma unroll
        for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
            int   sample_x = reflect_coord(x + i, W);
            float weight   = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
            for (int c = 0; c < C; c++) {
                int idx = (reflected_y * W + sample_x) * C + c;
                halo_sum[c] += weight * input[idx];
            }
        }
#pragma unroll
        for (int c = 0; c < C; c++) { shared_data[ty][tx + KERNEL_RADIUS][c] = halo_sum[c]; }
    }

    // Bottom halo: load data for rows below current block
    if (ty >= BLOCK_SIZE - KERNEL_RADIUS) {
        int global_y    = by * BLOCK_SIZE + ty + KERNEL_RADIUS;
        int reflected_y = reflect_coord(global_y, H);

        float halo_sum[C] = {0.0f};
#pragma unroll
        for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
            int   sample_x = reflect_coord(x + i, W);
            float weight   = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
            for (int c = 0; c < C; c++) {
                int idx = (reflected_y * W + sample_x) * C + c;
                halo_sum[c] += weight * input[idx];
            }
        }
#pragma unroll
        for (int c = 0; c < C; c++) {
            shared_data[ty + 2 * KERNEL_RADIUS][tx + KERNEL_RADIUS][c] = halo_sum[c];
        }
    }

    __syncthreads();

    // Step 2: Apply vertical Gaussian blur using shared memory data
    float v_sum[C] = {0.0f};

#pragma unroll
    for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
        // Calculate global y coordinate and apply reflect if needed
        int global_sample_y    = y + i;
        int reflected_sample_y = reflect_coord(global_sample_y, H);

        // Map reflected global coordinate back to shared memory index
        int local_sample_y = reflected_sample_y - by * BLOCK_SIZE;
        int shared_y       = local_sample_y + KERNEL_RADIUS;

        float weight = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
        for (int c = 0; c < C; c++) { v_sum[c] += weight * shared_data[shared_y][tx + KERNEL_RADIUS][c]; }
    }

// Write final result for all channels
#pragma unroll
    for (int c = 0; c < C; c++) {
        int out_idx     = (y * W + x) * C + c;
        output[out_idx] = v_sum[c];
    }
}

// FP16 version of the Gaussian blur kernel
template<
    const int C             = 4,
    const int KERNEL_RADIUS = 3,
    const int BLOCK_SIZE    = 32>
__global__ void gaussian_blur_kernel_fp16(
    const __half* __restrict__ input, // Input tensor [H*W*C]
    __half* __restrict__ output,      // Output tensor [H*W*C]
    const int H,                      // Tensor height
    const int W,                      // Tensor width
    float     sigma                   // Gaussian sigma parameter
) {
    // Thread and block indices
    int tx = threadIdx.x;
    int ty = threadIdx.y;
    int bx = blockIdx.x;
    int by = blockIdx.y;

    // Global coordinates
    int x = bx * blockDim.x + tx;
    int y = by * blockDim.y + ty;

    if (x >= W || y >= H) return;

    // Shared memory for horizontal blur results - with padding for kernel radius
    constexpr int     PADDED_SIZE = BLOCK_SIZE + 2 * KERNEL_RADIUS;
    __shared__ __half shared_data[PADDED_SIZE][PADDED_SIZE][C];

    float inv_sigma_sq = 1.0f / (2.0f * sigma * sigma);

    // Pre-compute normalized Gaussian weights
    float gaussian_weights[2 * KERNEL_RADIUS + 1];
    float weight_sum = 0.0f;

// First pass: compute raw weights and sum
#pragma unroll
    for (int i = 0; i <= 2 * KERNEL_RADIUS; i++) {
        int offset          = i - KERNEL_RADIUS;
        gaussian_weights[i] = expf(-offset * offset * inv_sigma_sq);
        weight_sum += gaussian_weights[i];
    }

// Second pass: normalize weights
#pragma unroll
    for (int i = 0; i <= 2 * KERNEL_RADIUS; i++) { gaussian_weights[i] /= weight_sum; }

    // Step 1: Apply horizontal blur and store in shared memory
    // Apply horizontal blur for all channels
    float h_sum[C] = {0.0f};

#pragma unroll
    for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
        int   sample_x = reflect_coord(x + i, W);
        float weight   = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
        for (int c = 0; c < C; c++) {
            int idx = (y * W + sample_x) * C + c;
            h_sum[c] += weight * __half2float(input[idx]);
        }
    }

// Store horizontally blurred result in shared memory
#pragma unroll
    for (int c = 0; c < C; c++) {
        shared_data[ty + KERNEL_RADIUS][tx + KERNEL_RADIUS][c] = __float2half(h_sum[c]);
    }

    // Load halo data for vertical blur
    // Top halo: load data for rows above current block
    if (ty < KERNEL_RADIUS) {
        int global_y    = by * BLOCK_SIZE + ty - KERNEL_RADIUS;
        int reflected_y = reflect_coord(global_y, H);

        float halo_sum[C] = {0.0f};
#pragma unroll
        for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
            int   sample_x = reflect_coord(x + i, W);
            float weight   = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
            for (int c = 0; c < C; c++) {
                int idx = (reflected_y * W + sample_x) * C + c;
                halo_sum[c] += weight * __half2float(input[idx]);
            }
        }
#pragma unroll
        for (int c = 0; c < C; c++) { shared_data[ty][tx + KERNEL_RADIUS][c] = __float2half(halo_sum[c]); }
    }

    // Bottom halo: load data for rows below current block
    if (ty >= BLOCK_SIZE - KERNEL_RADIUS) {
        int global_y    = by * BLOCK_SIZE + ty + KERNEL_RADIUS;
        int reflected_y = reflect_coord(global_y, H);

        float halo_sum[C] = {0.0f};
#pragma unroll
        for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
            int   sample_x = reflect_coord(x + i, W);
            float weight   = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
            for (int c = 0; c < C; c++) {
                int idx = (reflected_y * W + sample_x) * C + c;
                halo_sum[c] += weight * __half2float(input[idx]);
            }
        }
#pragma unroll
        for (int c = 0; c < C; c++) {
            shared_data[ty + 2 * KERNEL_RADIUS][tx + KERNEL_RADIUS][c] = __float2half(halo_sum[c]);
        }
    }

    __syncthreads();

    // Step 2: Apply vertical Gaussian blur using shared memory data
    float v_sum[C] = {0.0f};

#pragma unroll
    for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; i++) {
        // Calculate global y coordinate and apply reflect if needed
        int global_sample_y    = y + i;
        int reflected_sample_y = reflect_coord(global_sample_y, H);

        // Map reflected global coordinate back to shared memory index
        int local_sample_y = reflected_sample_y - by * BLOCK_SIZE;
        int shared_y       = local_sample_y + KERNEL_RADIUS;

        float weight = gaussian_weights[i + KERNEL_RADIUS];

#pragma unroll
        for (int c = 0; c < C; c++) {
            v_sum[c] += weight * __half2float(shared_data[shared_y][tx + KERNEL_RADIUS][c]);
        }
    }

// Write final result for all channels
#pragma unroll
    for (int c = 0; c < C; c++) {
        int out_idx     = (y * W + x) * C + c;
        output[out_idx] = __float2half(v_sum[c]);
    }
}

static constexpr size_t KERNEL_RADIUS = 3;

// PyTorch binding function
torch::Tensor gaussian_blur(
    torch::Tensor input_moments, // Input moment tensor [B, H, W, C]
    float         sigma,         // Gaussian sigma parameter
    int           kernel_radius  // Kernel radius (default: 5)
) {
    // Get tensor dimensions
    int B = input_moments.size(0);
    int H = input_moments.size(1);
    int W = input_moments.size(2);
    int C = input_moments.size(3);

    // Create output tensor with same dtype as input
    torch::Tensor output_moments = torch::zeros_like(input_moments);

    // Configure CUDA kernel launch parameters
    constexpr int BLOCK_SIZE = 32;
    dim3          block_size(BLOCK_SIZE, BLOCK_SIZE); // Use smaller block for better occupancy
    dim3          grid_size((W + block_size.x - 1) / block_size.x, (H + block_size.y - 1) / block_size.y);

    // Check if input is fp16
    if (input_moments.scalar_type() == torch::kFloat16) {
        // Launch fp16 kernel based on channel count
        switch (C) {
            case 16:
                gaussian_blur_kernel_fp16<16, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
                    reinterpret_cast<const __half*>(input_moments.data_ptr<at::Half>()),
                    reinterpret_cast<__half*>(output_moments.data_ptr<at::Half>()),
                    H,
                    W,
                    sigma
                );
                break;
            case 5:
                gaussian_blur_kernel_fp16<5, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
                    reinterpret_cast<const __half*>(input_moments.data_ptr<at::Half>()),
                    reinterpret_cast<__half*>(output_moments.data_ptr<at::Half>()),
                    H,
                    W,
                    sigma
                );
                break;
            case 4:
                gaussian_blur_kernel_fp16<4, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
                    reinterpret_cast<const __half*>(input_moments.data_ptr<at::Half>()),
                    reinterpret_cast<__half*>(output_moments.data_ptr<at::Half>()),
                    H,
                    W,
                    sigma
                );
                break;
            case 3:
                gaussian_blur_kernel_fp16<3, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
                    reinterpret_cast<const __half*>(input_moments.data_ptr<at::Half>()),
                    reinterpret_cast<__half*>(output_moments.data_ptr<at::Half>()),
                    H,
                    W,
                    sigma
                );
                break;
            case 2:
                gaussian_blur_kernel_fp16<2, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
                    reinterpret_cast<const __half*>(input_moments.data_ptr<at::Half>()),
                    reinterpret_cast<__half*>(output_moments.data_ptr<at::Half>()),
                    H,
                    W,
                    sigma
                );
                break;
            case 1:
                gaussian_blur_kernel_fp16<1, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
                    reinterpret_cast<const __half*>(input_moments.data_ptr<at::Half>()),
                    reinterpret_cast<__half*>(output_moments.data_ptr<at::Half>()),
                    H,
                    W,
                    sigma
                );
                break;
            default:
                // Fallback for other channel counts
                gaussian_blur_kernel_fp16<4, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
                    reinterpret_cast<const __half*>(input_moments.data_ptr<at::Half>()),
                    reinterpret_cast<__half*>(output_moments.data_ptr<at::Half>()),
                    H,
                    W,
                    sigma
                );
                break;
        }
    } else {
        assert(false);
        // // Launch fp32 kernel based on channel count
        // switch (C) {
        //     case 16:
        //         gaussian_blur_kernel<16, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
        //             input_moments.data_ptr<float>(), output_moments.data_ptr<float>(), H, W, sigma
        //         );
        //         break;
        //     case 5:
        //         gaussian_blur_kernel<5, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
        //             input_moments.data_ptr<float>(), output_moments.data_ptr<float>(), H, W, sigma
        //         );
        //         break;
        //     case 4:
        //         gaussian_blur_kernel<4, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
        //             input_moments.data_ptr<float>(), output_moments.data_ptr<float>(), H, W, sigma
        //         );
        //         break;
        //     case 3:
        //         gaussian_blur_kernel<3, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
        //             input_moments.data_ptr<float>(), output_moments.data_ptr<float>(), H, W, sigma
        //         );
        //         break;
        //     case 2:
        //         gaussian_blur_kernel<2, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
        //             input_moments.data_ptr<float>(), output_moments.data_ptr<float>(), H, W, sigma
        //         );
        //         break;
        //     case 1:
        //         gaussian_blur_kernel<1, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
        //             input_moments.data_ptr<float>(), output_moments.data_ptr<float>(), H, W, sigma
        //         );
        //         break;
        //     default:
        //         // Fallback for other channel counts
        //         gaussian_blur_kernel<4, KERNEL_RADIUS, BLOCK_SIZE><<<grid_size, block_size>>>(
        //             input_moments.data_ptr<float>(), output_moments.data_ptr<float>(), H, W, sigma
        //         );
        //         break;
        // }
    }

    return output_moments;
}
