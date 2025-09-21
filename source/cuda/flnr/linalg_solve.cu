#include <algorithm>
#include <cmath>
#include <cublas_v2.h>
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cusolverDn.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <torch/types.h>
#include <vector>

// Helper macro for cuBLAS error checking
#define CUBLAS_CHECK(call)                                                     \
    do {                                                                       \
        cublasStatus_t status = call;                                          \
        if (status != CUBLAS_STATUS_SUCCESS) {                                 \
            printf("cuBLAS error at %s:%d: %d\n", __FILE__, __LINE__, status); \
            return torch::empty({0});                                          \
        }                                                                      \
    } while (0)

// Helper macro for CUDA error checking
#define CUDA_CHECK(call)                                                                        \
    do {                                                                                        \
        cudaError_t error = call;                                                               \
        if (error != cudaSuccess) {                                                             \
            printf("CUDA error at %s:%d: %s\n", __FILE__, __LINE__, cudaGetErrorString(error)); \
            return torch::empty({0});                                                           \
        }                                                                                       \
    } while (0)

// CUDA kernel to populate pointer arrays directly on GPU
__global__ void populate_pointer_arrays(
    float** XtX_array,
    float** A_array,
    float*  XtX_base,
    float*  A_base,
    int     batch_size,
    int     matrix_stride,
    int     vector_stride
) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < batch_size) {
        XtX_array[i] = XtX_base + i * matrix_stride;
        A_array[i]   = A_base + i * vector_stride;
    }
}

torch::Tensor linalg_solve(
    torch::Tensor XtX,     // X^T*X moments [B, H, W, (Q+1)x(Q+1)]
    torch::Tensor XtY,     // X^T*Y moments [B, H, W, (Q+1)xC]
    float         epsilon, // Additive regularization
    float         eta      // Multiplicative regularization
) {
    const int B            = XtX.size(0);
    const int H            = XtX.size(1);
    const int W            = XtX.size(2);
    const int Q_ADD_SQUARE = XtX.size(3);
    const int Q_plus_1     = std::sqrt(Q_ADD_SQUARE);
    const int C            = XtY.size(3) / Q_plus_1;

    int batch_size = B * H * W;

    // Create output tensor A: [B, H, W, (Q+1)*C]
    auto          options = torch::TensorOptions().dtype(torch::kFloat32).device(XtX.device());
    torch::Tensor A       = torch::zeros({B, H, W, Q_plus_1 * C}, options);

    // Reshape for batch operations: [B*H*W, Q+1, Q+1] and [B*H*W, Q+1, C]
    auto XtX_batch = XtX.view({batch_size, Q_plus_1, Q_plus_1}).contiguous();
    auto XtY_batch = XtY.view({batch_size, Q_plus_1, C}).contiguous();
    auto A_batch   = A.view({batch_size, Q_plus_1, C});

    // Add regularization: XtX += εI
    auto I    = torch::eye(Q_plus_1, options).unsqueeze(0).expand({batch_size, Q_plus_1, Q_plus_1});
    XtX_batch = XtX_batch + epsilon * I;

    // Copy XtY to A (will be overwritten with solution)
    A_batch.copy_(XtY_batch);

    // Get tensor data pointers (ensure tensors are contiguous)
    float* d_XtX = XtX_batch.data_ptr<float>();
    float* d_A   = A_batch.data_ptr<float>();

    // Create cuBLAS handle
    cublasHandle_t handle;
    CUBLAS_CHECK(cublasCreate(&handle));

    // Create device pointer arrays for batch operations
    float** d_XtX_ptrs;
    float** d_A_ptrs;
    CUDA_CHECK(cudaMalloc(&d_XtX_ptrs, batch_size * sizeof(float*)));
    CUDA_CHECK(cudaMalloc(&d_A_ptrs, batch_size * sizeof(float*)));

    // Create host pointer arrays and copy to device - standard approach
    std::vector<float*> h_XtX_ptrs(batch_size);
    std::vector<float*> h_A_ptrs(batch_size);
    for (int i = 0; i < batch_size; ++i) {
        h_XtX_ptrs[i] = d_XtX + i * Q_plus_1 * Q_plus_1;
        h_A_ptrs[i]   = d_A + i * Q_plus_1 * C;
    }
    CUDA_CHECK(
        cudaMemcpy(d_XtX_ptrs, h_XtX_ptrs.data(), batch_size * sizeof(float*), cudaMemcpyHostToDevice)
    );
    CUDA_CHECK(cudaMemcpy(d_A_ptrs, h_A_ptrs.data(), batch_size * sizeof(float*), cudaMemcpyHostToDevice));

    // Allocate pivot and info arrays
    int* d_pivot;
    int* d_info_getrf;
    CUDA_CHECK(cudaMalloc(&d_pivot, batch_size * Q_plus_1 * sizeof(int)));
    CUDA_CHECK(cudaMalloc(&d_info_getrf, batch_size * sizeof(int)));

    // Batch LU factorization (XtX data will be replaced with LU factors)
    CUBLAS_CHECK(
        cublasSgetrfBatched(handle, Q_plus_1, d_XtX_ptrs, Q_plus_1, d_pivot, d_info_getrf, batch_size)
    );

    // Check LU factorization success
    std::vector<int> h_info_getrf(batch_size);
    CUDA_CHECK(
        cudaMemcpy(h_info_getrf.data(), d_info_getrf, batch_size * sizeof(int), cudaMemcpyDeviceToHost)
    );

    // Batch solve: XtX * result = XtY (result will overwrite XtY data)
    int h_info_getrs = 0;
    CUBLAS_CHECK(cublasSgetrsBatched(
        handle,
        CUBLAS_OP_N,
        Q_plus_1,
        C,
        (const float**)d_XtX_ptrs,
        Q_plus_1,
        d_pivot,
        d_A_ptrs,
        Q_plus_1,
        &h_info_getrs,
        batch_size
    ));

    // Clean up
    CUDA_CHECK(cudaFree(d_XtX_ptrs));
    CUDA_CHECK(cudaFree(d_A_ptrs));
    CUDA_CHECK(cudaFree(d_pivot));
    CUDA_CHECK(cudaFree(d_info_getrf));
    CUBLAS_CHECK(cublasDestroy(handle));

    return A;
}

// Advanced regularized linear solver implementing the custom normalization method
// from the FLNR paper appendix, avoiding traditional Tikhonov regularization issues

// template<const int Q = 1,
//          const int C = 1>
// __global__ void regularized_solve_kernel(
//     const float* __restrict__ XtX,       // Input: X^T*X moments [H,W,(Q+1)x(Q+1)]
//     const float* __restrict__ XtY,       // Input: X^T*Y moments [H,W,(Q+1)xC]
//     float* __restrict__ regression_coeffs,     // Output: regression coefficients [H, W,(Q+1)xC]
//     int H,                                 // Moment tensor height
//     int W,                                  // Moment tensor width
//     float epsilon,                              // Additive regularization parameter
//     float eta                                   // Multiplicative regularization parameter
// ) {
//     int x = blockIdx.x * blockDim.x + threadIdx.x;
//     int y = blockIdx.y * blockDim.y + threadIdx.y;

//     if (x >= W || y >= H) return;

//     int pixel_idx = y * W + x;
//     constexpr int Q_plus_1 = Q + 1;

//     // Load X^T*X moment matrix for this pixel
//     float reg_xtx[Q_plus_1][Q_plus_1];
//     for (int i = 0; i < Q_plus_1; i++) {
//         for (int j = 0; j < Q_plus_1; j++) {
//             int idx = pixel_idx * Q_plus_1 * Q_plus_1 + i * Q_plus_1 + j;
//             reg_xtx[i][j] = XtX[idx];
//         }
//     }

//     // Load X^T*Y moment matrix for this pixel
//     float reg_xty[Q_plus_1][C];
//     for (int i = 0; i < Q_plus_1; i++) {
//         for (int c = 0; c < C; c++) {
//             int idx = pixel_idx * Q_plus_1 * C + i * C + c;
//             reg_xty[i][c] = XtY[idx];
//         }
//     }

//     // Step 1: Decompose X^T*X matrix according to Equation 12
//     // X^T*X = [n    u_X^T]
//     //         [u_X    S  ]

//     float n = reg_xtx[0][0];  // Number of (weighted) samples

//     // Prevent division by zero
//     if (n < 1e-8f) {
//         // Output identity mapping for degenerate cases
//         for (int i = 0; i < Q_plus_1; i++) {
//             for (int c = 0; c < C; c++) {
//                 int out_idx = pixel_idx * Q_plus_1 * C + i * C + c;
//                 regression_coeffs[out_idx] = (i == 0) ? reg_xty[i][c] : 0.0f;
//             }
//         }
//         return;
//     }

//     // Extract u_X vector (contains n * \mu_X)
//     float u_X[Q];
//     for (int i = 0; i < Q; i++) {
//         u_X[i] = reg_xtx[0][i + 1];
//     }

//     // Step 2: Recover mean mu_X according to Equation 13
//     float mu_X[Q];
//     for (int i = 0; i < Q; i++) {
//         mu_X[i] = u_X[i] / n;
//     }

//     // Extract S matrix (correlation matrix part)
//     float S[Q][Q];
//     for (int i = 0; i < Q; i++) {
//         for (int j = 0; j < Q; j++) {
//             S[i][j] = reg_xtx[i + 1][j + 1];
//         }
//     }

//     // Step 3: Compute W matrix according to Equation 15
//     // W = S/n - mu_X^T * mu_X
//     float W[Q][Q];
//     for (int i = 0; i < Q; i++) {
//         for (int j = 0; j < Q; j++) {
//             W[i][j] = S[i][j] / n - mu_X[i] * mu_X[j];
//         }
//     }

//     // Step 4: Apply advanced regularization according to Equation 18
//     // W^ = S/n + �*diag(�_X^T * �_X) + �*I - (1-�)*�_X^T * �_X
//     float W_hat[Q][Q];
//     float mu_X_outer[Q][Q];

//     // Compute �_X^T * �_X (outer product)
//     for (int i = 0; i < Q; i++) {
//         for (int j = 0; j < Q; j++) {
//             mu_X_outer[i][j] = mu_X[i] * mu_X[j];
//         }
//     }

//     for (int i = 0; i < Q; i++) {
//         for (int j = 0; j < Q; j++) {
//             W_hat[i][j] = S[i][j] / n +
//                           eta * ((i == j) ? mu_X_outer[i][j] : 0.0f) +  // �*diag(�_X^T * �_X)
//                           epsilon * ((i == j) ? 1.0f : 0.0f) -          // �*I
//                           (1.0f - eta) * mu_X_outer[i][j];              // (1-�)*�_X^T * �_X
//         }
//     }

//     // Step 5: Extract regularized variance according to Equation 19
//     float sigma_hat_X[Q];
//     for (int i = 0; i < Q; i++) {
//         sigma_hat_X[i] = sqrtf(fmaxf(W_hat[i][i], epsilon)); // Ensure positive
//     }

//     // Step 6: Compute regularized correlation matrix according to Equation 20
//     float C_hat[Q][Q];
//     for (int i = 0; i < Q; i++) {
//         for (int j = 0; j < Q; j++) {
//             C_hat[i][j] = W_hat[i][j] / (sigma_hat_X[i] * sigma_hat_X[j]);
//         }
//     }

//     // Step 7: Process X^T*Y to get normalized B^ according to Equation 21
//     // First extract �_Y from the first row of X^T*Y
//     float mu_Y[C];
//     for (int c = 0; c < C; c++) {
//         mu_Y[c] = reg_xty[0][c] / n;
//     }

//     // Compute B^
//     float B_hat[Q][C];
//     for (int i = 0; i < Q; i++) {
//         for (int c = 0; c < C; c++) {
//             B_hat[i][c] = (reg_xty[i + 1][c] / n - mu_X[i] * mu_Y[c]) / sigma_hat_X[i];
//         }
//     }

//     // Step 8: Add final regularization to C_hat and solve according to Equation 22
//     // A^ = (C^ + �*I)^(-1) * B^

//     // Regularize diagonal of C_hat
//     for (int i = 0; i < Q; i++) {
//         C_hat[i][i] += epsilon;
//     }

//     // Solve the regularized system using Gaussian elimination
//     float A_hat[Q][C];

//     // For each output channel, solve the linear system
//     for (int c = 0; c < C; c++) {
//         // Copy C_hat to temporary matrix for Gaussian elimination
//         float temp_C[Q][Q];
//         float temp_b[Q];

//         for (int i = 0; i < Q; i++) {
//             temp_b[i] = B_hat[i][c];
//             for (int j = 0; j < Q; j++) {
//                 temp_C[i][j] = C_hat[i][j];
//             }
//         }

//         // Gaussian elimination with partial pivoting
//         for (int k = 0; k < Q; k++) {
//             // Find pivot
//             int pivot = k;
//             for (int i = k + 1; i < Q; i++) {
//                 if (fabsf(temp_C[i][k]) > fabsf(temp_C[pivot][k])) {
//                     pivot = i;
//                 }
//             }

//             // Swap rows
//             if (pivot != k) {
//                 for (int j = 0; j < Q; j++) {
//                     float temp = temp_C[k][j];
//                     temp_C[k][j] = temp_C[pivot][j];
//                     temp_C[pivot][j] = temp;
//                 }
//                 float temp = temp_b[k];
//                 temp_b[k] = temp_b[pivot];
//                 temp_b[pivot] = temp;
//             }

//             // Check for near-singular matrix
//             if (fabsf(temp_C[k][k]) < 1e-8f) {
//                 temp_C[k][k] = epsilon; // Regularize diagonal
//             }

//             // Eliminate
//             for (int i = k + 1; i < Q; i++) {
//                 float factor = temp_C[i][k] / temp_C[k][k];
//                 for (int j = k; j < Q; j++) {
//                     temp_C[i][j] -= factor * temp_C[k][j];
//                 }
//                 temp_b[i] -= factor * temp_b[k];
//             }
//         }

//         // Back substitution
//         for (int i = Q - 1; i >= 0; i--) {
//             float sum = temp_b[i];
//             for (int j = i + 1; j < Q; j++) {
//                 sum -= temp_C[i][j] * A_hat[j][c];
//             }
//             A_hat[i][c] = sum / temp_C[i][i];
//         }
//     }

//     // Step 9: Output final regression coefficients
//     // The first coefficient (bias term) needs special handling
//     for (int c = 0; c < C; c++) {
//         // Bias term: �_Y - sum(A^_i * �_X_i / �^_X_i)
//         float bias = mu_Y[c];
//         for (int i = 0; i < Q; i++) {
//             bias -= A_hat[i][c] * mu_X[i] / sigma_hat_X[i];
//         }

//         int out_idx = pixel_idx * Q_plus_1 * C + 0 * C + c;
//         regression_coeffs[out_idx] = bias;

//         // Guide channel coefficients: A^_i / �^_X_i
//         for (int i = 0; i < Q; i++) {
//             int out_idx = pixel_idx * Q_plus_1 * C + (i + 1) * C + c;
//             regression_coeffs[out_idx] = A_hat[i][c] / sigma_hat_X[i];
//         }
//     }
// }

// // PyTorch binding for advanced regularized linear solver
// torch::Tensor regularized_solve_cuda(
//     torch::Tensor XtX,      // X^T*X moments [H, W, (Q+1), (Q+1)]
//     torch::Tensor XtY,      // X^T*Y moments [H, W, (Q+1), C]
//     float epsilon,                 // Additive regularization
//     float eta                      // Multiplicative regularization
// ) {
//     // Get tensor dimensions
//     int H = XtX.size(0);
//     int W = XtX.size(1);
//     int Q_plus_1 = XtX.size(2);
//     int C = XtY.size(3);
//     int Q = Q_plus_1 - 1;

//     // Validate template parameters match input
//     constexpr int Q_TEMPLATE = 16;  // Fixed guide channels for this implementation
//     constexpr int C_TEMPLATE = 1;   // Fixed target channels for AO denoising

//     if (Q != Q_TEMPLATE) {
//         printf("Error: Expected %d guide channels, got %d\n", Q_TEMPLATE, Q);
//         return torch::empty({0});
//     }

//     constexpr int BLOCK_SIZE = 16;
//     // Create output tensor for regression coefficients
//     auto options = torch::TensorOptions().dtype(XtX.type()).device(XtX.device());
//     torch::Tensor regression_coeffs = torch::zeros({H, W, Q_plus_1, C}, options);

//     // Configure CUDA kernel launch parameters
//     dim3 block_size(BLOCK_SIZE, BLOCK_SIZE);
//     dim3 grid_size(
//         (W + block_size.x - 1) / block_size.x,
//         (H + block_size.y - 1) / block_size.y
//     );

//     // Launch templated kernel
//     gularized_solve_kernel<1, 1><<<grid_size, block_size>>>(
//         XtX.data_ptr<float>(),
//         XtY.data_ptr<float>(),
//         regression_coeffs.data_ptr<float>(),
//         H,
//         W,
//         epsilon,
//         eta
//     );

//     return regression_coeffs;
// }
