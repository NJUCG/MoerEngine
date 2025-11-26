#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#include <iostream>
#include <memory>

using namespace nvinfer1;

const char* ONNX_PATH = "D:\\Data\\NJU3a\\1-MoerEngine\\Assets\\model4_t.onnx";

// Logger
class Logger : public ILogger {
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING)
            std::cout << "[TRT] " << msg << std::endl;
    }
} gLogger;

/**
 * Example Output:
 *   Engine has 2 I/O tensors:
 *   Tensor[0] name = data
 *   Tensor[1] name = resnetv19_dense0_fwd
 *   Input dims: 1 3 224 224
 *   Output dims: 1 1000
 */
int main() {
    // 1. Builder
    auto       builder       = std::unique_ptr<IBuilder>(createInferBuilder(gLogger));
    const auto explicitBatch = 1U << static_cast<uint32_t>(NetworkDefinitionCreationFlag::kEXPLICIT_BATCH);

    auto network = std::unique_ptr<INetworkDefinition>(builder->createNetworkV2(explicitBatch));

    auto parser = std::unique_ptr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, gLogger));

    if (!parser->parseFromFile(ONNX_PATH, static_cast<int>(ILogger::Severity::kWARNING))) {
        std::cerr << "ONNX parse failed. Please check the ONNX file path!\n";
        return -1;
    }

    // 2. Config
    auto config = std::unique_ptr<IBuilderConfig>(builder->createBuilderConfig());
    config->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE, 1ULL << 30); // 1GB

    auto profile = builder->createOptimizationProfile();

    // 假设模型的输入名是 "data"，维度 [N, C, H, W]，N 可以是动态
    profile->setDimensions("data", OptProfileSelector::kMIN, Dims4(1, 3, 224, 224));
    profile->setDimensions("data", OptProfileSelector::kOPT, Dims4(1, 3, 224, 224));
    profile->setDimensions("data", OptProfileSelector::kMAX, Dims4(1, 3, 224, 224));

    config->addOptimizationProfile(profile);

    // 2.1 Engine
    auto engine = std::unique_ptr<ICudaEngine>(builder->buildEngineWithConfig(*network, *config));
    if (!engine) {
        std::cerr << "Engine build failed\n";
        return -1;
    }

    // 3. Context
    auto context = std::unique_ptr<IExecutionContext>(engine->createExecutionContext());
    if (!context) {
        std::cerr << "Context creation failed\n";
        return -1;
    }

    // 4. 输入输出张量
    int nbIOTensors = engine->getNbIOTensors();
    std::cout << "Engine has " << nbIOTensors << " I/O tensors:\n";
    for (int i = 0; i < nbIOTensors; ++i) {
        std::cout << "Tensor[" << i << "] name = " << engine->getIOTensorName(i) << "\n";
    }

    const char* inputName  = engine->getIOTensorName(0); // 假设第0个是输入
    const char* outputName = engine->getIOTensorName(1); // 假设第1个是输出
    auto        inputDims  = engine->getTensorShape(inputName);
    auto        outputDims = engine->getTensorShape(outputName);

    std::cout << "Input dims: ";
    for (int i = 0; i < inputDims.nbDims; i++)
        std::cout << inputDims.d[i] << " ";
    std::cout << "\n";

    std::cout << "Output dims: ";
    for (int i = 0; i < outputDims.nbDims; i++)
        std::cout << outputDims.d[i] << " ";
    std::cout << "\n";

    return 0;
}