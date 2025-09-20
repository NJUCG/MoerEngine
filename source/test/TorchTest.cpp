#include <torch/torch.h>
#include <iostream>

int main() {
    // 创建一个张量
    torch::Tensor t = torch::rand({2, 3});
    std::cout << "Tensor t = \n"
              << t << std::endl;

    // 在 GPU 上创建张量（如果支持）
    if (torch::cuda::is_available()) {
        auto t_cuda = t.to(torch::kCUDA);
        std::cout << "Tensor on CUDA = \n"
                  << t_cuda << std::endl;
    } else {
        std::cout << "CUDA not available" << std::endl;
    }

    return 0;
}
