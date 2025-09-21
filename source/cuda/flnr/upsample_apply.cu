#include <algorithm>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <torch/types.h>
#include <vector>

// Templated CUDA kernel for upsampling and model application
// Optimized with compile-time template parameters Q (guide channels) and C (output channels)
template<
    const int Q  = 1,
    const int C  = 3,
    const int DF = 8>
__global__ void upsample_apply_kernel(
    const float* __restrict__ coeffs, // Input: Regression coefficients [B, H/DF,W/DF, (Q+1)xC]
    const float* __restrict__ guide,  // Input: High-res Guide channels [B, H, W, Q]
    float* __restrict__ output,       // Output: Denoised result [B, H, W, C]
    const int H,                      // Output image height
    const int W                       // Output image width
) {
    const int tx = threadIdx.x;
    const int ty = threadIdx.y;
    const int bx = blockIdx.x;
    const int by = blockIdx.y;
    int       x  = bx * blockDim.x + tx;
    int       y  = by * blockDim.y + ty;

    if (x >= W || y >= H) return;

    // Calculate downsampled dimensions
    int down_H = (H + DF - 1) / DF;
    int down_W = (W + DF - 1) / DF;

    // Find the corresponding downsampled position using PyTorch's align_corners=False formula
    // PyTorch uses: src_idx = (dst_idx + 0.5) * scale - 0.5 where scale = src_size / dst_size
    float scale_x = (float)down_W / W;
    float scale_y = (float)down_H / H;
    float down_x  = (x + 0.5f) * scale_x - 0.5f;
    float down_y  = (y + 0.5f) * scale_y - 0.5f;

    // Bilinear interpolation of coefficients with proper boundary handling
    int x0 = max(0, min((int)floor(down_x), down_W - 1));
    int y0 = max(0, min((int)floor(down_y), down_H - 1));
    int x1 = max(0, min(x0 + 1, down_W - 1));
    int y1 = max(0, min(y0 + 1, down_H - 1));

    // Clamp fractional coordinates to valid range
    float fx = max(0.0f, min(down_x - x0, 1.0f));
    float fy = max(0.0f, min(down_y - y0, 1.0f));

    constexpr int Q_plus_1 = Q + 1;

    // Construct augmented guide vector: [1, g1, g2, ..., gQ]
    float augmented_guide[Q_plus_1];
    augmented_guide[0] = 1.0f; // Bias term

    int pixel_idx = y * W + x;

// Load guide channels
#pragma unroll
    for (int q = 0; q < Q; q++) { augmented_guide[q + 1] = guide[pixel_idx * Q + q]; }

// Apply bilinearly interpolated model for each output channel
#pragma unroll
    for (int c = 0; c < C; c++) {
        float result = 0.0f;

// For each coefficient in the (Q+1)-dimensional model
#pragma unroll
        for (int q = 0; q < Q_plus_1; q++) {
            // Bilinear interpolation of coefficients
            int coeff_idx_00 = (y0 * down_W + x0) * Q_plus_1 * C + q * C + c;
            int coeff_idx_01 = (y0 * down_W + x1) * Q_plus_1 * C + q * C + c;
            int coeff_idx_10 = (y1 * down_W + x0) * Q_plus_1 * C + q * C + c;
            int coeff_idx_11 = (y1 * down_W + x1) * Q_plus_1 * C + q * C + c;

            float c00 = coeffs[coeff_idx_00];
            float c01 = coeffs[coeff_idx_01];
            float c10 = coeffs[coeff_idx_10];
            float c11 = coeffs[coeff_idx_11];

            // Bilinear interpolation
            float c0                 = c00 * (1.0f - fx) + c01 * fx;
            float c1                 = c10 * (1.0f - fx) + c11 * fx;
            float interpolated_coeff = c0 * (1.0f - fy) + c1 * fy;

            // Apply model: output = sum(coeff[q] * augmented_guide[q])
            result += interpolated_coeff * augmented_guide[q];
        }

        // Store result
        int output_idx     = pixel_idx * C + c;
        output[output_idx] = result;
    }
}

// PyTorch binding function
torch::Tensor upsample_apply(
    torch::Tensor coeffs, // [B, H/DF, W/DF, (Q+1) * C]
    torch::Tensor guide,  // [B, H, W, Q]
    int           factor  // DF (downsampling factor, typically 8)
) {
    // Get tensor dimensions
    int B = guide.size(0);            // Batch size (typically 1)
    int H = guide.size(1);            // Output height
    int W = guide.size(2);            // Output width
    int Q = guide.size(3);            // Guide channels
    int C = coeffs.size(3) / (Q + 1); // Output channels

    // Create output tensor
    auto          options = torch::TensorOptions().dtype(torch::kFloat32).device(guide.device());
    torch::Tensor output  = torch::zeros({B, H, W, C}, options);

    // Configure CUDA kernel launch parameters
    constexpr int BLOCK_SIZE = 16;
    dim3          block_size(BLOCK_SIZE, BLOCK_SIZE);
    dim3          grid_size((W + block_size.x - 1) / block_size.x, (H + block_size.y - 1) / block_size.y);

    // Launch kernel based on template parameters
    if (C == 3) {
        switch (Q) {
            case 1:
                upsample_apply_kernel<1, 3, 8><<<grid_size, block_size>>>(
                    coeffs.data_ptr<float>(), guide.data_ptr<float>(), output.data_ptr<float>(), H, W
                );
                break;
            case 4:
                upsample_apply_kernel<4, 3, 8><<<grid_size, block_size>>>(
                    coeffs.data_ptr<float>(), guide.data_ptr<float>(), output.data_ptr<float>(), H, W
                );
                break;
        }
    } else if (C == 1) {
        switch (Q) {
            case 1:
                upsample_apply_kernel<1, 1, 8><<<grid_size, block_size>>>(
                    coeffs.data_ptr<float>(), guide.data_ptr<float>(), output.data_ptr<float>(), H, W
                );
                break;
            case 4:
                upsample_apply_kernel<4, 1, 8><<<grid_size, block_size>>>(
                    coeffs.data_ptr<float>(), guide.data_ptr<float>(), output.data_ptr<float>(), H, W
                );
                break;
        }
    }

    return output;
}
