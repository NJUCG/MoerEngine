#include <NvInfer.h>
#include <array>
#include <cmath>
#include <cuda_runtime_api.h>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
class Logger : public nvinfer1::ILogger {
    void log(Severity severity, const char* message) noexcept override {
        if (severity <= Severity::kWARNING)
            std::cerr << "[TensorRT] " << message << '\n';
    }
};
void Require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void Check(cudaError_t result) {
    if (result != cudaSuccess)
        throw std::runtime_error(cudaGetErrorString(result));
}
struct CudaFree {
    void operator()(void* pointer) const {
        cudaFree(pointer);
    }
};
struct Stream {
    cudaStream_t handle{};
    Stream() {
        Check(cudaStreamCreate(&handle));
    }
    ~Stream() {
        cudaStreamDestroy(handle);
    }
};
} // namespace

int main() {
    try {
        using namespace nvinfer1;
        Logger logger;
        auto   builder = std::unique_ptr<IBuilder>(createInferBuilder(logger));
        Require(bool(builder), "TensorRT builder creation failed");
        auto network = std::unique_ptr<INetworkDefinition>(builder->createNetworkV2(0));
        Require(bool(network), "TensorRT network creation failed");
        const float scale       = 2.0f;
        const float shift       = 1.0f;
        auto*       input       = network->addInput("input", DataType::kFLOAT, Dims{1, {8}});
        auto*       scale_layer = network->addConstant(Dims{1, {1}}, Weights{DataType::kFLOAT, &scale, 1});
        auto*       shift_layer = network->addConstant(Dims{1, {1}}, Weights{DataType::kFLOAT, &shift, 1});
        Require(input && scale_layer && shift_layer, "TensorRT input creation failed");
        auto* product =
            network->addElementWise(*input, *scale_layer->getOutput(0), ElementWiseOperation::kPROD);
        Require(product != nullptr, "TensorRT multiplication creation failed");
        auto* sum = network->addElementWise(
            *product->getOutput(0), *shift_layer->getOutput(0), ElementWiseOperation::kSUM
        );
        Require(sum != nullptr, "TensorRT addition creation failed");
        sum->getOutput(0)->setName("output");
        network->markOutput(*sum->getOutput(0));
        auto config = std::unique_ptr<IBuilderConfig>(builder->createBuilderConfig());
        Require(bool(config), "TensorRT configuration creation failed");
        config->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE, 64ULL << 20);
        auto plan = std::unique_ptr<IHostMemory>(builder->buildSerializedNetwork(*network, *config));
        Require(bool(plan), "TensorRT engine build failed");
        // The runtime must outlive the deserialized engine and its execution context.
        auto runtime = std::unique_ptr<IRuntime>(createInferRuntime(logger));
        Require(bool(runtime), "TensorRT runtime creation failed");
        auto engine =
            std::unique_ptr<ICudaEngine>(runtime->deserializeCudaEngine(plan->data(), plan->size()));
        Require(bool(engine), "TensorRT engine deserialization failed");
        auto context = std::unique_ptr<IExecutionContext>(engine->createExecutionContext());
        Require(bool(context), "TensorRT execution context creation failed");
        Stream               stream;
        std::array<float, 8> host_input{-3.0f, -1.0f, 0.0f, 0.5f, 1.0f, 2.0f, 4.0f, 16.0f};
        std::array<float, 8> host_output{};
        void*                input_raw{};
        Check(cudaMalloc(&input_raw, sizeof(host_input)));
        std::unique_ptr<void, CudaFree> device_input(input_raw);
        void*                           output_raw{};
        Check(cudaMalloc(&output_raw, sizeof(host_output)));
        std::unique_ptr<void, CudaFree> device_output(output_raw);
        Require(context->setTensorAddress("input", input_raw), "TensorRT input binding failed");
        Require(context->setTensorAddress("output", output_raw), "TensorRT output binding failed");
        Check(cudaMemcpyAsync(
            input_raw, host_input.data(), sizeof(host_input), cudaMemcpyHostToDevice, stream.handle
        ));
        Require(context->enqueueV3(stream.handle), "TensorRT inference failed");
        Check(cudaMemcpyAsync(
            host_output.data(), output_raw, sizeof(host_output), cudaMemcpyDeviceToHost, stream.handle
        ));
        Check(cudaStreamSynchronize(stream.handle));
        for (size_t i = 0; i != host_input.size(); ++i) {
            Require(
                std::isfinite(host_output[i]) && std::abs(host_output[i] - (2 * host_input[i] + 1)) < 1e-5f,
                "TensorRT inference output does not match the CPU result"
            );
        }
        std::cout << "[TESTCASE][PASS] TensorRTInference outputs=8 formula=2*x+1 readback=verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
