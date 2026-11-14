#include "cuda_interpo_test.h"
#include <cuda_runtime.h>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace {
void Check(cudaError_t result) {
    if (result != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(result));
}
struct CudaFree {
    void operator()(void* pointer) const {
        cudaFree(pointer);
    }
};
} // namespace

int main() {
    try {
        using Moer::Cuda::uint64;
        constexpr uint64    count = 1 << 20;
        std::vector<uint64> input(count);
        for (uint64 i = 0; i < count; ++i)
            input[i] = i % 10;
        uint64* input_raw{};
        Check(cudaMalloc(reinterpret_cast<void**>(&input_raw), count * sizeof(uint64)));
        std::unique_ptr<uint64, CudaFree> device_input(input_raw);
        uint64*                           output_raw{};
        Check(cudaMalloc(reinterpret_cast<void**>(&output_raw), sizeof(uint64)));
        std::unique_ptr<uint64, CudaFree> device_output(output_raw);
        Check(cudaMemcpy(input_raw, input.data(), count * sizeof(uint64), cudaMemcpyHostToDevice));
        // The kernel uses atomicAdd: every invocation needs a zeroed accumulator.
        constexpr uint64 per_thread = 2048;
        constexpr uint64 threads    = 256;
        constexpr uint64 blocks     = (count - 1) / (per_thread * threads) + 1;
        const auto       expected   = std::accumulate(input.begin(), input.end(), uint64{});
        for (int iteration = 0; iteration != 2; ++iteration) {
            Check(cudaMemset(output_raw, 0, sizeof(uint64)));
            Moer::Cuda::cuda_reduction_kernel(blocks, threads, per_thread, input_raw, count, output_raw);
            Check(cudaGetLastError());
            Check(cudaDeviceSynchronize());
            uint64 actual{};
            Check(cudaMemcpy(&actual, output_raw, sizeof(actual), cudaMemcpyDeviceToHost));
            if (actual != expected)
                throw std::runtime_error("CUDA reduction does not match the CPU result");
        }
        std::cout << "[TESTCASE][PASS] CudaReduction iterations=2 result=" << expected << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
