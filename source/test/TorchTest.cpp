#include <iostream>
#include <torch/torch.h>

int main() {
    // 创建一个张量
    torch::Tensor t = torch::rand({2, 3});
    std::cout << "Tensor t = \n" << t << std::endl;

    // 在 GPU 上创建张量（如果支持）
    if (torch::cuda::is_available()) {
        auto t_cuda = t.to(torch::kCUDA);
        auto result = (t_cuda * 2.0f + 1.0f).to(torch::kCPU);
        if (!torch::allclose(result, t * 2.0f + 1.0f)) {
            std::cerr << "LibTorch CUDA output does not match the CPU result\n";
            return 1;
        }
        std::cout << "[TESTCASE][PASS] LibTorchCuda readback=verified\n";
    } else {
        std::cout << "CUDA not available" << std::endl;
        return 1;
    }

    return 0;
}
