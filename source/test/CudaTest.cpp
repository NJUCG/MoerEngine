#include <iostream>
#include <cstdio>
#include <cuda_runtime.h>

#include "misc/Traits.h"

#include "cuda_interpo_test.h"

using uint64 = Moer::uint64;

int main() {

    std::cout << "CUDA TEST" << std::endl;

    static const uint64 n = 1 << 20;

    std::vector<uint64> hA(n);
    uint64              hOut;

    for (int i = 0; i < n; i++) {
        hA[i] = i % 10;
    }

    // device addr
    uint64* dA;
    uint64* dOut;
    cudaMalloc((void**)&dA, n * sizeof(uint64));
    cudaMalloc((void**)&dOut, sizeof(uint64));

    cudaMemcpy(dA, hA.data(), n * sizeof(uint64), cudaMemcpyHostToDevice);

    uint64 CountPerThread  = 2048;
    uint64 threadsPerBlock = 256;
    uint64 blocksPerGrid   = (n - 1) / (CountPerThread * threadsPerBlock) + 1;

    Moer::Cuda::cuda_reduction_kernel(blocksPerGrid, threadsPerBlock, CountPerThread, dA, n, dOut);

    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess) {
        printf("CUDA Error: %s\n", cudaGetErrorString(err));
    }

    cudaDeviceSynchronize();

    cudaMemcpy(&hOut, dOut, sizeof(uint64), cudaMemcpyDeviceToHost);

    // output
    uint64 answer_o1 = (1LL * n / 10) * ((0 + 9) * 10 / 2) + (0LL + (n - 1) % 10) * ((n - 1) % 10 + 1) / 2;

    std::cout << "CUDA Result: " << hOut << std::endl;
    std::cout << "CPU  Result: " << answer_o1 << std::endl;
    if (hOut == answer_o1) {
        std::cout << "Passed!" << std::endl;
    } else {
        std::cout << "Error!" << std::endl;
    }

    return 0;
}