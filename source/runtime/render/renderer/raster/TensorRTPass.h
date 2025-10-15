/**
 * 此文件应该只有在宏 WITH_CUDA 被设置的情况下使用
 * 
 * 这个宏启用时，默认环境为Windows11+Vulkan；所以其他地方不再判断
 */
#pragma once

#if !defined(WITH_CUDA)
#error "This header requires WITH_CUDA=1"
#endif

#include "log/LogSystem.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "CudaVulkanTools.h"
#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>
#include <iostream>
#include <memory>

using namespace nvinfer1;

namespace Moer::Render::Raster {

#define checkCudaErrors(val) CudaVulkanTools::checkCudaErrorsInner((val), #val, __FILE__, __LINE__)

struct TensorRTResource {

    CudaTexture ao;
    CudaTexture depth;
    CudaTexture color;
    CudaTexture motion;
    CudaTexture prev_ao;
    CudaTexture prev_embed;
    CudaTexture feature;

    CudaSemaphore semaphore;

    // no RAII
    TensorRTResource(
        RasterContext& context,
        TextureRef     ao_tex,
        TextureRef     depth_tex,
        TextureRef     color_tex,
        TextureRef     motion_tex,
        TextureRef     prev_ao_tex,
        TextureRef     prev_embed_tex,
        TextureRef     feature_tex
    ) :
        ao(context, ao_tex),
        depth(context, depth_tex),
        color(context, color_tex),
        motion(context, motion_tex),
        prev_ao(context, prev_ao_tex),
        prev_embed(context, prev_embed_tex),
        feature(context, feature_tex),
        semaphore(context) {

        ;
    };
    ~TensorRTResource() = default;

    TensorRTResource(const TensorRTResource&)            = delete;
    TensorRTResource& operator=(const TensorRTResource&) = delete;
};

class TensorRTLogger : public ILogger {
    void log(Severity severity, const char* msg) noexcept override {
        if (severity <= Severity::kWARNING)
            LOG_WARNING("[TRT] {}", msg);
    }
} gLogger;

struct TensorRTEngine {
    TensorRTEngine() {

        LOG_INFO("Prepare to load ONNX and build TensorRT Engine.");

        std::string onnx_path = (ConfigManager::GetInstance().GetEditorResourcePath() / "ai" / "onnx_models" /
                                 "model4_v10.13.onnx")
                                    .string();

        // 1. Builder
        auto builder = std::unique_ptr<IBuilder>(createInferBuilder(gLogger));
        auto network = std::unique_ptr<INetworkDefinition>(builder->createNetworkV2(0));

        auto parser = std::unique_ptr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, gLogger));

        if (!parser->parseFromFile(onnx_path.c_str(), static_cast<int>(ILogger::Severity::kWARNING))) {
            LOG_ERROR("ONNX parse failed. Please check the ONNX file path!");
            return;
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

        LOG_INFO("Building TensorRT Engine.");

        // 2.1 Engine
        auto engine = std::unique_ptr<ICudaEngine>(builder->buildEngineWithConfig(*network, *config));
        if (!engine) {
            LOG_ERROR("Engine build failed");
            return;
        }

        LOG_INFO("Creating TensorRT Engine.");

        // 3. Context
        auto context = std::unique_ptr<IExecutionContext>(engine->createExecutionContext());
        if (!context) {
            LOG_ERROR("Context creation failed");
            return;
        }

        // 4. 输入输出张量 for debug

        /**
         * Tensor[0] name = ao;         shape = (1, 1, 540, 960, )
         * Tensor[1] name = depth;      shape = (1, 1, 540, 960, )
         * Tensor[2] name = color;      shape = (1, 3, 540, 960, )
         * Tensor[3] name = motion;     shape = (1, 2, 540, 960, )
         * Tensor[4] name = prev_ao;    shape = (1, 1, 540, 960, )
         * Tensor[5] name = prev_embed; shape = (1, 32, 540, 960, )
         * Tensor[6] name = feature;    shape = (1, 19, 540, 960, )
         */

        // 5. Bindings

        int nbIOTensors = engine->getNbIOTensors();

        std::ostringstream output_stream;
        output_stream << "Engine has " << nbIOTensors << " I/O tensors:\n";

        UnorderedMap<std::string, void*> tensor_buffers;

        {
            for (int i = 0; i < nbIOTensors; ++i) {
                const char* name  = engine->getIOTensorName(i);
                Dims        shape = engine->getTensorShape(name);
                DataType    dtype = engine->getTensorDataType(name);

                // 5.1 calculate array size

                size_t element_size = [&]() {
                    switch (dtype) {
                        case DataType::kFLOAT:
                            return sizeof(float);
                        case DataType::kHALF:
                            return size_t(sizeof(float) * 0.5); // 233
                        case DataType::kINT8:
                            return sizeof(int8_t);
                        case DataType::kINT32:
                            return sizeof(int32_t);
                        default:
                            assert(false);
                    };
                }();

                size_t element_count = [&]() {
                    size_t sum = 1;
                    for (int i = 0; i < shape.nbDims; i++) {
                        sum *= shape.d[i];
                    }
                    return sum;
                }();

                size_t total_bytes = element_count * element_size;

                // 5.2 malloc

                void* d_memory = nullptr; // device memory
                checkCudaErrors(cudaMalloc(&d_memory, total_bytes));

                // set 0 for all bytes
                checkCudaErrors(cudaMemset(d_memory, 0, total_bytes));

                tensor_buffers[name] = d_memory;

                // 5.3 bind

                context->setTensorAddress(name, d_memory);

                // 5.4 output

                output_stream << "Tensor[" << i << "] name = " << name << "; shape = (";
                for (int i = 0; i < shape.nbDims; i++)
                    output_stream << shape.d[i] << ", ";
                output_stream << ");\t";

                output_stream << "buffer length = " << element_count
                              << ";\tbuffer size = " << total_bytes / 1024 << "KB\n";
            }
        }

        LOG_INFO("TensorRT Pass: {}", output_stream.str());

        // context->setTensorAddress("ao", nullptr);
        // context->setTensorAddress("depth", nullptr);
        // context->setTensorAddress("color", nullptr);
        // context->setTensorAddress("motion", nullptr);
        // context->setTensorAddress("prev_ao", nullptr);
        // context->setTensorAddress("prev_embed", nullptr);
        // context->setTensorAddress("feature", nullptr);

        // 6. Infer

        cudaStream_t stream;
        checkCudaErrors(cudaStreamCreate(&stream));

        bool is_success = context->enqueueV3(stream);
        if (!is_success) {
            LOG_ERROR("TRT Pass: enqueueV3 failed.");
        }

        checkCudaErrors(cudaStreamSynchronize(stream));
        LOG_INFO("Inference done!");

        for (auto& kv : tensor_buffers)
            cudaFree(kv.second);

        cudaStreamDestroy(stream);
    }

    ~TensorRTEngine() = default;

    TensorRTEngine(const TensorRTEngine&)            = delete;
    TensorRTEngine& operator=(const TensorRTEngine&) = delete;
};

/**
 * MARK: CUDA Pass
 * 
 * Reference: https://github.com/NVIDIA/cuda-samples/tree/master/Samples/5_Domain_Specific/vulkanImageCUDA
 */
class TensorRTPass {
public:
    TensorRTPass(
        RasterContext& _context
        // ,
        // TextureRef     ao_tex,
        // TextureRef     depth_tex,
        // TextureRef     color_tex,
        // TextureRef     motion_tex,
        // TextureRef     prev_ao_tex,
        // TextureRef     prev_embed_tex,
        // TextureRef     feature_tex
    ) :
        context(_context) {
        // res = MakeUnique<TensorRTResource>(
        //     context, ao_tex, depth_tex, color_tex, motion_tex, prev_ao_tex, prev_embed_tex, feature_tex
        // );

        engine = MakeUnique<TensorRTEngine>();
    }
    ~TensorRTPass() {
        res.reset();
        engine.reset();
    }

    TensorRTPass(const TensorRTPass&)            = delete;
    TensorRTPass& operator=(const TensorRTPass&) = delete;

    void RecreateResource(
        RasterContext& context,
        TextureRef     ao_tex,
        TextureRef     depth_tex,
        TextureRef     color_tex,
        TextureRef     motion_tex,
        TextureRef     prev_ao_tex,
        TextureRef     prev_embed_tex,
        TextureRef     feature_tex
    ) {
        res.reset();
    }

    uint Process(RasterContext& context, const RasterConfig& ui_config, uint input_image) {
        if (ui_config.ai_is_cuda_enabled == false)
            return input_image;

        // signal

        res->semaphore.Signal();

        // cuda

        // TODO

        // wait

        res->semaphore.Wait();

        // return

        return input_image;
    }

private:
    RasterContext& context;

    UniquePtr<TensorRTResource> res;
    UniquePtr<TensorRTEngine>   engine;
};

#undef checkCudaErrors

} // namespace Moer::Render::Raster