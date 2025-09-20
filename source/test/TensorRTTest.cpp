#include <NvInfer.h>
#include <iostream>
#include <memory>

class Logger : public nvinfer1::ILogger {
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING) {
            std::cout << "[TensorRT] " << msg << std::endl;
        }
    }
};

int main() {
    Logger logger;

    // 创建 Builder (TRT 10 需要 unique_ptr 或手动 delete)
    nvinfer1::IBuilder* rawBuilder = nvinfer1::createInferBuilder(logger);
    if (!rawBuilder) {
        std::cerr << "Failed to create TensorRT builder!" << std::endl;
        return 1;
    }
    std::unique_ptr<nvinfer1::IBuilder> builder(rawBuilder);

    std::cout << "TensorRT builder created successfully." << std::endl;

    return 0;
}
