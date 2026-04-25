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
#include "misc/Timer.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "CudaVulkanTools.h"
#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"
#include "cuda_in_raster/cuda_in_raster.h"

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_fp16.h>
#include <curand.h> // for rand
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

using namespace nvinfer1;

namespace Moer::Render::Raster {

#define checkCudaErrors(val)     CudaVulkanTools::checkCudaErrorsInner((val), #val, __FILE__, __LINE__)
#define checkCusolverErrors(val) CudaVulkanTools::checkCusolverErrorsInner((val), #val, __FILE__, __LINE__)

struct TensorRTResource {

    CudaTexture ao;
    CudaTexture depth;
    CudaTexture color;
    CudaTexture motion;
    CudaTexture prev_ao;

    CudaSemaphore semaphore;

    // no RAII
    TensorRTResource(
        RasterContext& context,
        TextureRef     ao_tex,
        TextureRef     depth_tex,
        TextureRef     color_tex,
        TextureRef     motion_tex,
        TextureRef     prev_ao_tex
    ) :
        ao(context, ao_tex),
        depth(context, depth_tex),
        color(context, color_tex),
        motion(context, motion_tex),
        prev_ao(context, prev_ao_tex),
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
            LOG_WARNING(MOER_TEXT("[TRT] {}"), msg);
    }
} gLogger;

/**
 * 这个类对 TRT Engine 进行封装
 * 
 * 这个类会根据传入的onnx_path，加载onnx文件，并把它编译成TRT Engine，并进行缓存
 * - 其中，缓存的代码是ai写的，不保证正确性；缓存的编译后结果位于目录 ./target/bin/Debug/resource/ai/tensorrt_cache/ 目录下
 * 
 * 这个类会对Engine的每个Input和Output，在gpu上建立对应大小的缓存
 * - 可以通过 device_mem_addr_map[channel_name] 来访问对应的gpu内存地址
 * 
 * FIXME: 这个struct不能使用UniquePtr，否则mimalloc会发出神秘错误，原因不明
 */
struct TensorRTEngine {

    std::string                            onnx_path;
    std::string                            cache_path;
    std::unique_ptr<IBuilder>              builder;
    std::unique_ptr<INetworkDefinition>    network;
    std::unique_ptr<nvonnxparser::IParser> parser;
    std::unique_ptr<IBuilderConfig>        config;
    std::unique_ptr<ICudaEngine>           engine;
    std::unique_ptr<IExecutionContext>     context;

    UnorderedMap<std::string, __half*> device_mem_addr_map;
    UnorderedMap<std::string, size_t>  device_mem_size_map;

public:
    TensorRTEngine(const std::string& _onnx_path) : onnx_path(_onnx_path) {

        LOG_DEBUG(MOER_TEXT("Prepare to load ONNX and build TensorRT Engine."));

        cache_path = GetCachePath(onnx_path);

        // Try to load from cache first
        if (cache_path.empty() || !LoadEngineFromCache(cache_path)) { // writen by ai
            LOG_INFO(MOER_TEXT("Cache not found or invalid, building engine from ONNX..."));
            LoadEngineFromOnnx();
        }

        // Context
        context = std::unique_ptr<IExecutionContext>(engine->createExecutionContext());
        if (!context) {
            LOG_ERROR(MOER_TEXT("Context creation failed"));
            return;
        }

        LOG_INFO(MOER_TEXT("Created TensorRT IExecutionContext."));

        CreateBuffers(/* is_verbose */ true);
    }

    ~TensorRTEngine() {
        for (auto& kv : device_mem_addr_map) {
            if (kv.second != nullptr) {
                cudaFree(kv.second);
            }
        }
        device_mem_addr_map.clear();

        context.reset();
        engine.reset();

        config.reset();
        parser.reset();
        network.reset();
        builder.reset();
    }

    TensorRTEngine(const TensorRTEngine&)            = delete;
    TensorRTEngine& operator=(const TensorRTEngine&) = delete;

    // MARK: Load Random
    void LoadRandomValueToBuffers(TensorRTResource& res) {
        int nbIOTensors = engine->getNbIOTensors();

        for (int i = 0; i < nbIOTensors; ++i) {
            const char* name  = engine->getIOTensorName(i);
            Dims        shape = engine->getTensorShape(name);
            DataType    dtype = engine->getTensorDataType(name);

            if (strncmp(name, "in_", 3) == 0) {

                assert(shape.nbDims == 4);

                // 假设 shape 格式为 [N, C, H, W]
                int batch      = shape.d[0];
                int channels   = shape.d[1];
                int dst_height = shape.d[2]; // tensor 的目标高度
                int dst_width  = shape.d[3]; // tensor 的目标宽度

                size_t N = 1ULL * channels * dst_height * dst_width;
                dim3   blockSize(256);
                dim3   gridSize((N - 1) / blockSize.x / Moer::Cuda::RANDOMS_PER_THREAD);

                static uint64_t seed = 114514;

                Moer::Cuda::FillRandomHalf(
                    gridSize, blockSize, res.semaphore.stream_to_run, device_mem_addr_map[name], N, seed++
                );
            } else {
                checkCudaErrors(cudaMemset(device_mem_addr_map[name], 0, device_mem_size_map[name]));
            }
        }
    }

    // MARK: Load Zero
    void LoadZeroToBuffers() {
        int nbIOTensors = engine->getNbIOTensors();

        for (int i = 0; i < nbIOTensors; ++i) {
            const char* name  = engine->getIOTensorName(i);
            Dims        shape = engine->getTensorShape(name);
            DataType    dtype = engine->getTensorDataType(name);

            checkCudaErrors(cudaMemset(device_mem_addr_map[name], 0, device_mem_size_map[name]));
        }
    }

    // MARK: Engine1 Load
    void Engine1_LoadTexturesToBuffers(
        TensorRTResource& res,
        uint              ao_only_idx,
        bool              is_verbose,
        bool              is_force_ldr
    ) {

        int nbIOTensors = engine->getNbIOTensors();

        for (int i = 0; i < nbIOTensors; ++i) {
            const char* name  = engine->getIOTensorName(i);
            Dims        shape = engine->getTensorShape(name);
            DataType    dtype = engine->getTensorDataType(name);

            // 5.1 calculate array size

            // 假设 shape 格式为 [N, C, H, W]
            int batch      = shape.d[0];
            int channels   = shape.d[1];
            int dst_height = shape.d[2]; // tensor 的目标高度
            int dst_width  = shape.d[3]; // tensor 的目标宽度

            // CUDA kernel 执行配置 - 基于目标尺寸
            dim3 blockSize(16, 16);
            dim3 gridSize(
                (dst_width + blockSize.x - 1) / blockSize.x, (dst_height + blockSize.y - 1) / blockSize.y
            );

            /**
             * 默认所有数据从RGBA开始填，即 depth占R；motion vector占RG
             */
            auto copy_to_buf = [&](CudaTexture& src_tex, int channels, __half* d_target) {
                CudaTexture::EFormatElementType type       = src_tex.GetElementType();
                size_t                          type_count = src_tex.GetElementTypeCount();

                bool use_tone_mapping =
                    is_force_ldr && (std::string(name).find("color") != std::string::npos);

                if (type == CudaTexture::EFormatElementType::UCHAR && type_count == 4) {
                    Moer::Cuda::CopySurfaceToBuffer_Resize_NCHW_Half_Uchar4(
                        gridSize,
                        blockSize,
                        res.semaphore.stream_to_run,
                        // 0, // no stream object
                        src_tex.GetSurfaceObjectList(),
                        d_target,
                        use_tone_mapping,
                        src_tex.width,
                        src_tex.height,
                        dst_width,
                        dst_height,
                        channels
                    );
                } else if (type == CudaTexture::EFormatElementType::UCHAR && type_count == 1) {
                    Moer::Cuda::CopySurfaceToBuffer_Resize_NCHW_Half_Uchar1(
                        gridSize,
                        blockSize,
                        res.semaphore.stream_to_run,
                        // 0, // no stream object
                        src_tex.GetSurfaceObjectList(),
                        d_target,
                        use_tone_mapping,
                        src_tex.width,
                        src_tex.height,
                        dst_width,
                        dst_height,
                        channels
                    );
                } else if (type == CudaTexture::EFormatElementType::FLOAT && type_count == 4) {
                    Moer::Cuda::CopySurfaceToBuffer_Resize_NCHW_Half_Float4(
                        gridSize,
                        blockSize,
                        res.semaphore.stream_to_run,
                        // 0, // no stream object
                        src_tex.GetSurfaceObjectList(),
                        d_target,
                        use_tone_mapping,
                        src_tex.width,
                        src_tex.height,
                        dst_width,
                        dst_height,
                        channels
                    );
                } else if (type == CudaTexture::EFormatElementType::FLOAT && type_count == 1) {
                    Moer::Cuda::CopySurfaceToBuffer_Resize_NCHW_Half_Float1(
                        gridSize,
                        blockSize,
                        res.semaphore.stream_to_run,
                        // 0, // no stream object
                        src_tex.GetSurfaceObjectList(),
                        d_target,
                        use_tone_mapping,
                        src_tex.width,
                        src_tex.height,
                        dst_width,
                        dst_height,
                        channels
                    );
                } else if (type == CudaTexture::EFormatElementType::HALF && type_count == 2) {
                    Moer::Cuda::CopySurfaceToBuffer_Resize_NCHW_Half_Half2(
                        gridSize,
                        blockSize,
                        res.semaphore.stream_to_run,
                        // 0, // no stream object
                        src_tex.GetSurfaceObjectList(),
                        d_target,
                        use_tone_mapping,
                        src_tex.width,
                        src_tex.height,
                        dst_width,
                        dst_height,
                        channels
                    );
                } else if (type == CudaTexture::EFormatElementType::HALF && type_count == 4) {
                    Moer::Cuda::CopySurfaceToBuffer_Resize_NCHW_Half_Half4(
                        gridSize,
                        blockSize,
                        res.semaphore.stream_to_run,
                        // 0, // no stream object
                        src_tex.GetSurfaceObjectList(),
                        d_target,
                        use_tone_mapping,
                        src_tex.width,
                        src_tex.height,
                        dst_width,
                        dst_height,
                        channels
                    );
                } else {
                    assert(false);
                }

                if (is_verbose) {
                    LOG_DEBUG(
                        MOER_TEXT("tex {}: size from ({}, {}) to ({}, {}); channels = {}; type = {}{}"),
                        name,
                        src_tex.width,
                        src_tex.height,
                        dst_width,
                        dst_height,
                        channels,
                        (type == CudaTexture::EFormatElementType::FLOAT ?
                             "float" :
                             (type == CudaTexture::EFormatElementType::HALF ? "half" : "uchar")),
                        type_count
                    );
                }
            };

            if (std::strcmp(name, "in_ao") == 0) {
                copy_to_buf((ao_only_idx ? res.prev_ao : res.ao), 1, device_mem_addr_map[name]);

            } else if (std::strcmp(name, "in_depth") == 0) {
                copy_to_buf(res.depth, 1, device_mem_addr_map[name]);

            } else if (std::strcmp(name, "in_color") == 0) {
                copy_to_buf(res.color, 3, device_mem_addr_map[name]);

            } else if (std::strcmp(name, "in_motion") == 0) {
                copy_to_buf(res.motion, 2, device_mem_addr_map[name]);

            } else if (std::strcmp(name, "in_prev_ao") == 0) {
                copy_to_buf((ao_only_idx ? res.ao : res.prev_ao), 1, device_mem_addr_map[name]);

            } else if (std::strcmp(name, "in_prev_embed") == 0) {

                static bool isFirstTime = true;
                if (isFirstTime) {
                    isFirstTime = false;

                    // 第一次执行，置0
                    checkCudaErrors(cudaMemset(device_mem_addr_map[name], 0, device_mem_size_map[name]));

                } else {

                    // 第N次执行，设置为 out_embed
                    checkCudaErrors(cudaMemcpy(
                        device_mem_addr_map[name],
                        device_mem_addr_map["out_embed"],
                        device_mem_size_map[name],
                        cudaMemcpyDeviceToDevice
                    ));
                }

            } else {
                // random value should be ok
                // checkCudaErrors(cudaMemset(device_mem_addr_map[name], 0, device_mem_size_map[name]));
            }
        }
    }

    // MARK: Engine2 Load
    void Engine2_LoadEngine1OutputToBuffers(
        const cudaStream_t& stream_to_run,
        TensorRTEngine&     engine1,
        cusolverDnHandle_t  cusolver
    ) {

        int nbIOTensors = engine->getNbIOTensors();

        for (int i = 0; i < nbIOTensors; ++i) {
            const char* name  = engine->getIOTensorName(i);
            Dims        shape = engine->getTensorShape(name);
            DataType    dtype = engine->getTensorDataType(name);

            // TODO: 优化空间，考虑0开销拷贝
            auto copy_buf_between_engine = [&](const char* engine1_name, const char* engine2_name) {
                checkCudaErrors(cudaMemcpy(
                    device_mem_addr_map[engine2_name],
                    engine1.device_mem_addr_map[engine1_name],
                    device_mem_size_map[engine2_name],
                    cudaMemcpyDeviceToDevice
                ));

                assert(engine1.device_mem_size_map[engine1_name] == device_mem_size_map[engine2_name]);
            };

            if (std::strcmp(name, "in_X_model") == 0) {
                copy_buf_between_engine("out_X_model", name);

            } else if (std::strcmp(name, "in_coeffs_batch") == 0) {

                Moer::Cuda::SolveBatchedFXP16(
                    stream_to_run,
                    cusolver,
                    shape.d[0],
                    shape.d[1],
                    shape.d[2],
                    engine1.device_mem_addr_map["out_XTX_batch"],
                    engine1.device_mem_addr_map["out_XTY_batch"],
                    device_mem_addr_map[name],
                    1e-3
                );

            } else if (std::strcmp(name, "in_upscale_kernel") == 0) {
                copy_buf_between_engine("out_upscale_kernel", name);

            } else if (std::strcmp(name, "in_color") == 0) {
                copy_buf_between_engine("out_color", name);

            } else if (std::strcmp(name, "in_prev_ao") == 0) {
                copy_buf_between_engine("out_prev_ao", name);

            } else {
                // random value should be ok
                // checkCudaErrors(cudaMemset(device_mem_addr_map[name], 0, device_mem_size_map[name]));
            }
        }
    }

    void Run(const cudaStream_t& stream_to_run) {
        bool is_success = context->enqueueV3(stream_to_run);
        if (!is_success) {
            LOG_ERROR(MOER_TEXT("TRT Pass: enqueueV3 failed."));
        }
    }

private:
    std::string GetCachePath(const std::string& onnx_file_path) { // writen by ai
        auto cache_dir = ConfigManager::GetInstance().GetEditorResourcePath() / "ai" / "tensorrt_cache";
        std::filesystem::create_directories(cache_dir);

        // Generate hash from ONNX file content
        std::ifstream file(onnx_file_path, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR(MOER_TEXT("Failed to open ONNX file for hashing: {}"), onnx_file_path);
            return "";
        }

        std::hash<std::string> hasher;
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        size_t      hash_value = hasher(content);

        return (cache_dir / (std::to_string(hash_value) + ".trt")).string();
    }

    bool LoadEngineFromCache(const std::string& cache_file_path) { // writen by ai
        std::ifstream file(cache_file_path, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        file.seekg(0, std::ios::end);
        size_t size = file.tellg();
        file.seekg(0, std::ios::beg);

        std::vector<char> engine_data(size);
        file.read(engine_data.data(), size);
        file.close();

        auto runtime = std::unique_ptr<IRuntime>(createInferRuntime(gLogger));
        if (!runtime) {
            LOG_ERROR(MOER_TEXT("Failed to create TensorRT runtime for cache loading"));
            return false;
        }

        engine = std::unique_ptr<ICudaEngine>(runtime->deserializeCudaEngine(engine_data.data(), size));
        if (!engine) {
            LOG_ERROR(MOER_TEXT("Failed to deserialize cached engine"));
            return false;
        }

        LOG_DEBUG(MOER_TEXT("Successfully loaded TensorRT engine from cache: {}"), cache_file_path);
        return true;
    }

    bool SaveEngineToCache(const std::string& cache_file_path) { // writen by ai
        if (!engine) {
            LOG_ERROR(MOER_TEXT("No engine to save to cache"));
            return false;
        }

        auto serialized_engine = std::unique_ptr<IHostMemory>(engine->serialize());
        if (!serialized_engine) {
            LOG_ERROR(MOER_TEXT("Failed to serialize engine"));
            return false;
        }

        std::ofstream file(cache_file_path, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR(MOER_TEXT("Failed to open cache file for writing: {}"), cache_file_path);
            return false;
        }

        file.write(static_cast<const char*>(serialized_engine->data()), serialized_engine->size());
        file.close();

        LOG_INFO(MOER_TEXT("Successfully saved TensorRT engine to cache: {}"), cache_file_path);
        return true;
    }

    // MARK: LoadEngine ONNX
    void LoadEngineFromOnnx() {
        // 1. Builder
        {
            builder = std::unique_ptr<IBuilder>(createInferBuilder(gLogger));

            // 检查设备是否支持 FP16
            if (!builder->platformHasFastFp16()) {
                LOG_WARNING(MOER_TEXT("Platform does not have fast FP16 support"));
            }

            // 设置多线程构建
            int32_t numThreads = std::thread::hardware_concurrency();
            if (!builder->setMaxThreads(numThreads)) {
                LOG_WARNING(MOER_TEXT("Failed to set max threads to {}, using default"), numThreads);
            } else {
                LOG_DEBUG(MOER_TEXT("Builder configured to use {} threads"), builder->getMaxThreads());
            }
        }
        // 1.1 Network
        {
            // // 创建强类型网络（推荐方式）
            // NetworkDefinitionCreationFlags flags =
            //     1U << static_cast<uint32_t>(NetworkDefinitionCreationFlag::kSTRONGLY_TYPED);
            // network = std::unique_ptr<INetworkDefinition>(builder->createNetworkV2(flags));

            // 普通网络
            network = std::unique_ptr<INetworkDefinition>(builder->createNetworkV2(0));
        }
        // 1.2 Parser
        {
            parser = std::unique_ptr<nvonnxparser::IParser>(nvonnxparser::createParser(*network, gLogger));

            if (!parser->parseFromFile(onnx_path.c_str(), static_cast<int>(ILogger::Severity::kWARNING))) {
                LOG_ERROR(MOER_TEXT("ONNX parse failed. Please check the ONNX file path!"));
                return;
            }
        }
        // 1.3 Convert to FP16
        {
            // // 设置网络默认精度为 FP16
            // for (int i = 0; i < network->getNbLayers(); i++) {
            //     auto* layer = network->getLayer(i);
            //     layer->setPrecision(DataType::kHALF);

            //     // 强制所有输出为 FP16
            //     for (int j = 0; j < layer->getNbOutputs(); j++) {
            //         layer->getOutput(j)->setType(DataType::kHALF);
            //     }
            // }

            // 设置输入精度
            for (int i = 0; i < network->getNbInputs(); i++) {
                network->getInput(i)->setType(DataType::kHALF);
            }

            // 设置输出精度
            for (int i = 0; i < network->getNbOutputs(); i++) {
                network->getOutput(i)->setType(DataType::kHALF);
            }

            LOG_DEBUG(
                MOER_TEXT("Set all network tensors to FP16. Layers/Inputs/Outputs: {}/{}/{}"),
                network->getNbLayers(),
                network->getNbInputs(),
                network->getNbOutputs()
            );
        }

        // 2. Config
        config = std::unique_ptr<IBuilderConfig>(builder->createBuilderConfig());
        config->setMemoryPoolLimit(MemoryPoolType::kWORKSPACE, 1ULL << 30); // 1GB

        // 可选的构建优化
        config->setBuilderOptimizationLevel(3); // 默认级别

        // 强制fp16
        // trt10.12废弃的方法，但是简单
        {
            config->setFlag(BuilderFlag::kFP16);
            config->setFlag(BuilderFlag::kOBEY_PRECISION_CONSTRAINTS);
        }

        auto profile = builder->createOptimizationProfile();
        config->addOptimizationProfile(profile);

        // 3. Engine
        LOG_INFO(MOER_TEXT("Started to Build TensorRT ICudaEngine. It may take several minutes..."));

        engine = std::unique_ptr<ICudaEngine>(builder->buildEngineWithConfig(*network, *config));
        if (!engine) {
            LOG_ERROR(MOER_TEXT("Engine build failed"));
            return;
        }

        LOG_DEBUG(MOER_TEXT("Created TensorRT ICudaEngine."));

        // Save newly built engine to cache
        if (!cache_path.empty()) {
            SaveEngineToCache(cache_path);
        }
    }

    // MARK: CreateBuffers
    void CreateBuffers(bool is_verbose) {

        auto get_element_size = [](const DataType& dtype) {
            switch (dtype) {
                case DataType::kFLOAT:
                    return sizeof(float);
                case DataType::kHALF:
                    return sizeof(__half);
                case DataType::kINT8:
                    return sizeof(int8_t);
                case DataType::kINT32:
                    return sizeof(int32_t);
                default:
                    assert(false);
            };
        };

        auto get_element_count = [](const Dims& shape) {
            size_t sum = 1;
            for (int i = 0; i < shape.nbDims; i++) {
                sum *= shape.d[i];
            }
            return sum;
        };

        int nbIOTensors = engine->getNbIOTensors();

        std::ostringstream output_stream;
        output_stream << "\nEngine " << engine->getName() << " has " << nbIOTensors << " I/O tensors:\n";

        auto create_buf_for_tensor = [&](const char* name, const char* prefix, int i) {
            Dims     shape = engine->getTensorShape(name);
            DataType dtype = engine->getTensorDataType(name);

            size_t element_size  = get_element_size(dtype);
            size_t element_count = get_element_count(shape);
            size_t total_bytes   = element_count * element_size;

            assert(element_size == sizeof(__half));

            // malloc
            __half* d_memory = nullptr; // device memory
            checkCudaErrors(cudaMalloc(&d_memory, total_bytes));

            // set 0 for all bytes
            checkCudaErrors(cudaMemset(d_memory, 0, total_bytes));

            device_mem_addr_map[name] = d_memory;
            device_mem_size_map[name] = total_bytes;

            // 5.3 bind

            context->setTensorAddress(name, d_memory);

            // 5.4 output

            output_stream << "Tensor[" << i << "] (" << prefix << ") name = " << name
                          << "; element_size = " << element_size << "; shape = (";
            for (int i = 0; i < shape.nbDims; i++)
                output_stream << shape.d[i] << ", ";
            output_stream << ");\t";

            output_stream << "buffer length = " << element_count << ";\tbuffer size = " << total_bytes / 1024
                          << "KB\n";
        };

        std::vector<const char*> input_tensors;
        std::vector<const char*> output_tensors;

        for (int i = 0; i < nbIOTensors; ++i) {
            const char* name = engine->getIOTensorName(i);

            nvinfer1::TensorIOMode mode = engine->getTensorIOMode(name);

            if (mode == nvinfer1::TensorIOMode::kINPUT) {
                input_tensors.push_back(name);
            } else if (mode == nvinfer1::TensorIOMode::kOUTPUT) {
                output_tensors.push_back(name);
            } else {
                assert(false);
            }
        }

        for (int i = 0; i < input_tensors.size(); ++i) {
            create_buf_for_tensor(input_tensors[i], "Input ", i);
        }
        for (int i = 0; i < output_tensors.size(); ++i) {
            create_buf_for_tensor(output_tensors[i], "Output", input_tensors.size() + i);
        }

        if (is_verbose) {
            LOG_DEBUG(MOER_TEXT("TensorRT Buffers Info: {}"), output_stream.str());
        }
    }
};

/**
 * MARK: CUDA Pass
 * 
 * Reference: https://github.com/NVIDIA/cuda-samples/tree/master/Samples/5_Domain_Specific/vulkanImageCUDA
 */
class TensorRTPass {

private:
    RasterContext& context;

    UniquePtr<TensorRTResource> res;
    UniquePtr<TensorRTEngine>   engine1;
    UniquePtr<TensorRTEngine>   engine2;

    cusolverDnHandle_t cusolver = nullptr;

public:
    TensorRTPass(
        RasterContext& _context,
        TextureRef     ao_tex,
        TextureRef     depth_tex,
        TextureRef     color_tex,
        TextureRef     motion_tex,
        TextureRef     prev_ao_tex
    ) :
        context(_context) {

        // cusolver
        checkCusolverErrors(cusolverDnCreate(&cusolver));

        res = MakeUnique<TensorRTResource>(context, ao_tex, depth_tex, color_tex, motion_tex, prev_ao_tex);

        engine1 = MakeUnique<TensorRTEngine>((ConfigManager::GetInstance().GetEditorResourcePath() / "ai" /
                                              "onnx_models" / "model4_part1.onnx")
                                                 .string());

        engine2 = MakeUnique<TensorRTEngine>((ConfigManager::GetInstance().GetEditorResourcePath() / "ai" /
                                              "onnx_models" / "model4_part3.onnx")
                                                 .string());
    }
    ~TensorRTPass() {
        engine2.reset();
        engine1.reset();
        res.reset();
        cusolverDnDestroy(cusolver);
    }

    TensorRTPass(const TensorRTPass&)            = delete;
    TensorRTPass& operator=(const TensorRTPass&) = delete;

    void RecreateResource(
        RasterContext& context,
        TextureRef     ao_tex,
        TextureRef     depth_tex,
        TextureRef     color_tex,
        TextureRef     motion_tex,
        TextureRef     prev_ao_tex
    ) {
        res.reset();
        res = MakeUnique<TensorRTResource>(context, ao_tex, depth_tex, color_tex, motion_tex, prev_ao_tex);
    }

    /**

        Engine Unnamed Network 0 has 14 I/O tensors:

        Tensor[0]  (Input ) name = in_ao;              element_size = 2; shape = (1, 1, 540, 960, );   buffer length = 518400;   buffer size = 1012KB
        Tensor[1]  (Input ) name = in_depth;           element_size = 2; shape = (1, 1, 540, 960, );   buffer length = 518400;   buffer size = 1012KB
        Tensor[2]  (Input ) name = in_color;           element_size = 2; shape = (1, 3, 540, 960, );   buffer length = 1555200;  buffer size = 3037KB
        Tensor[3]  (Input ) name = in_motion;          element_size = 2; shape = (1, 2, 540, 960, );   buffer length = 1036800;  buffer size = 2025KB
        Tensor[4]  (Input ) name = in_prev_ao;         element_size = 2; shape = (1, 1, 540, 960, );   buffer length = 518400;   buffer size = 1012KB
        Tensor[5]  (Input ) name = in_prev_embed;      element_size = 2; shape = (1, 32, 540, 960, );  buffer length = 16588800; buffer size = 32400KB

        Tensor[6]  (Output) name = out_XTX_batch;      element_size = 2; shape = (8040, 4, 4, );       buffer length = 128640;   buffer size = 251KB
        Tensor[7]  (Output) name = out_XTY_batch;      element_size = 2; shape = (8040, 4, 1, );       buffer length = 32160;    buffer size = 62KB
        Tensor[8]  (Output) name = out_X_model;        element_size = 2; shape = (1, 3, 540, 960, );   buffer length = 1555200;  buffer size = 3037KB
        Tensor[9]  (Output) name = out_ao;             element_size = 2; shape = (1, 1, 540, 960, );   buffer length = 518400;   buffer size = 1012KB
        Tensor[10] (Output) name = out_upscale_kernel; element_size = 2; shape = (1, 16, 540, 960, );  uffer length = 8294400;   buffer size = 16200KB
        Tensor[11] (Output) name = out_color;          element_size = 2; shape = (1, 3, 540, 960, );   buffer length = 1555200;  buffer size = 3037KB
        Tensor[12] (Output) name = out_prev_ao;        element_size = 2; shape = (1, 1, 540, 960, );   buffer length = 518400;   buffer size = 1012KB
        Tensor[13] (Output) name = out_embed;          element_size = 2; shape = (1, 32, 540, 960, );  buffer length = 16588800; buffer size = 32400KB

        Engine Unnamed Network 0 has 7 I/O tensors:

        Tensor[0] (Input )  name = in_X_model;         element_size = 2; shape = (1, 3, 540, 960, );   buffer length = 1555200; buffer size = 3037KB
        Tensor[1] (Input )  name = in_coeffs_batch;    element_size = 2; shape = (8040, 4, 1, );       buffer length = 32160;   buffer size = 62KB
        Tensor[2] (Input )  name = in_upscale_kernel;  element_size = 2; shape = (1, 16, 540, 960, );  buffer length = 8294400; buffer size = 16200KB
        Tensor[3] (Input )  name = in_color;           element_size = 2; shape = (1, 3, 540, 960, );   buffer length = 1555200; buffer size = 3037KB
        Tensor[4] (Input )  name = in_prev_ao;         element_size = 2; shape = (1, 1, 540, 960, );   buffer length = 518400;  buffer size = 1012KB

        Tensor[5] (Output)  name = out_final_output;   element_size = 2; shape = (1, 3, 1080, 1920, ); buffer length = 6220800; buffer size = 12150KB
        Tensor[6] (Output)  name = out_denoised_ao;    element_size = 2; shape = (1, 1, 540, 960, );   buffer length = 518400;  buffer size = 1012KB

    */

    TextureWithHandle Process(RasterContext& context, const RasterConfig& ui_config, uint ao_only_idx) {
        assert(ui_config.ai_is_cuda_enabled);

        // log

        LogTips(ui_config);

        // signal

        res->semaphore.Signal();

        // cuda

        // TODO: 优化一下 cudaStreamSynchronize(res->semaphore.stream_to_run)
        auto sync = [&]() {
            checkCudaErrors(cudaStreamSynchronize(res->semaphore.stream_to_run));
        };

        // engine1->LoadRandomValueToBuffers(*res);
        // engine1->LoadZeroToBuffers();
        engine1->Engine1_LoadTexturesToBuffers(*res, ao_only_idx, false, ui_config.ai_trt_force_ldr);
        sync();

        engine1->Run(res->semaphore.stream_to_run);
        sync();

        engine2->Engine2_LoadEngine1OutputToBuffers(res->semaphore.stream_to_run, *engine1, cusolver);
        sync();

        engine2->Run(res->semaphore.stream_to_run);
        sync();

        // CheckBuf();

        VisualizeFeature(
            (ui_config.ai_trt_visualize_buffer.starts_with("Engine1") ? *engine1 : *engine2),
            res->color,
            ui_config.ai_trt_visualize_buffer.substr(8).c_str(),
            res->semaphore.stream_to_run,
            1.0f,
            ui_config.ai_trt_force_ldr
        );

        sync();

        // wait

        res->semaphore.Wait();

        // return

        return context.textures.lighting_output;
    }

private:
    void LogTips(const RasterConfig& ui_config) {
        if (ui_config.ao_mode != EAoMode::RTAO) {
            static LoopedTimer timer(2.0);
            if (timer.Tick()) { // 每隔2s触发一次
                LOG_WARNING(MOER_TEXT("Ambient Occlusion Mode is not RTAO. Please switch to RTAO!"));
            }
        }

        if (res->color.width != 1920 || res->color.height != 1080) {
            static LoopedTimer timer(2.0);
            if (timer.Tick()) { // 每隔2s触发一次
                LOG_WARNING(
                    MOER_TEXT("This network only support 1920 x 1080. Current resolution is {} x {}."),
                    res->color.width,
                    res->color.height
                );
            }
        }
    }

    // MARK: Visualize
    void VisualizeFeature(
        TensorRTEngine&     engine,
        CudaTexture&        color,
        const char*         name,
        const cudaStream_t& stream_to_run,
        float               debug_param,
        bool                is_force_ldr
    ) {

        const static uint64 TILE = 16;

        dim3 threadsPerBlock(TILE, TILE);
        dim3 blocksPerGrid(
            (color.width - 1) / threadsPerBlock.x + 1, (color.height - 1) / threadsPerBlock.y + 1
        );

        if (engine.engine->getTensorShape(name).nbDims != 4) {
            LOG_WARNING(MOER_TEXT("{}.shape != 4"), name);
            return;
        }
        if (engine.device_mem_addr_map.contains(name) == false) {
            LOG_WARNING(MOER_TEXT("Cannot find this buffer: {}"), name);
            return;
        }

        // 只要输出中包含"color"这个子串，就启用 tone mapping
        bool use_tone_mapping = is_force_ldr && (std::string(name).find("color") != std::string::npos);

        Moer::Cuda::VisualizeFeatureBuf(
            blocksPerGrid,
            threadsPerBlock,
            stream_to_run,
            color.GetSurfaceObjectList(),
            (__half*)engine.device_mem_addr_map[name],
            use_tone_mapping,
            engine.engine->getTensorShape(name).d[3], // width
            engine.engine->getTensorShape(name).d[2], // height
            engine.engine->getTensorShape(name).d[1], // channels
            color.width,
            color.height,
            debug_param
        );
    }

    void CheckBuf() {
        // 检查 final_output buffer 的值：拷回 host 并统计 min/max/是否全部为 0
        auto check_buf = [&](TensorRTEngine& engine, const char* name) {
            auto it = engine.device_mem_addr_map.find(name);
            if (it == engine.device_mem_addr_map.end()) {
                return;
            }

            void*    d_final_output = it->second;
            Dims     shape          = engine.engine->getTensorShape(name);
            DataType dtype          = engine.engine->getTensorDataType(name);

            // Check if all values in final_output buffer are the same
            size_t element_count = 1;
            for (int i = 0; i < shape.nbDims; i++) {
                element_count *= shape.d[i];
            }

            std::vector<__half> h_final_output(element_count);
            checkCudaErrors(cudaMemcpy(
                h_final_output.data(), d_final_output, element_count * sizeof(__half), cudaMemcpyDeviceToHost
            ));

            bool   all_same  = true;
            __half first_val = h_final_output[0];
            __half min_val   = h_final_output[0];
            __half max_val   = h_final_output[0];
            for (size_t i = 1; i < element_count; i++) {
                if (std::abs(float(h_final_output[i] - first_val)) > 0.0001) {
                    all_same = false;
                }
                min_val = std::min(min_val, h_final_output[i]);
                max_val = std::max(max_val, h_final_output[i]);
            }

            LOG_DEBUG(
                MOER_TEXT("\tcheck {}: all values same = {}, first value = {}, min = {}, max = {}"),
                name,
                all_same,
                static_cast<float>(first_val),
                static_cast<float>(min_val),
                static_cast<float>(max_val)
            );
        };

        LOG_DEBUG(MOER_TEXT(""));
        LOG_DEBUG(MOER_TEXT("Engine 1 Input:"));
        check_buf(*engine1, "in_ao");
        check_buf(*engine1, "in_depth");
        check_buf(*engine1, "in_color");
        check_buf(*engine1, "in_motion");
        check_buf(*engine1, "in_prev_ao");
        check_buf(*engine1, "in_prev_embed");

        LOG_DEBUG(MOER_TEXT(""));
        LOG_DEBUG(MOER_TEXT("Engine 1 Output:"));
        check_buf(*engine1, "out_XTX_batch");
        check_buf(*engine1, "out_XTY_batch");
        check_buf(*engine1, "out_X_model");
        check_buf(*engine1, "out_ao");
        check_buf(*engine1, "out_upscale_kernel");
        check_buf(*engine1, "out_color");
        check_buf(*engine1, "out_prev_ao");
        check_buf(*engine1, "out_embed");

        LOG_DEBUG(MOER_TEXT(""));
        LOG_DEBUG(MOER_TEXT("Engine 2 Input:"));
        check_buf(*engine2, "in_X_model");
        check_buf(*engine2, "in_coeffs_batch");
        check_buf(*engine2, "in_upscale_kernel");
        check_buf(*engine2, "in_color");
        check_buf(*engine2, "in_prev_ao");

        LOG_DEBUG(MOER_TEXT(""));
        LOG_DEBUG(MOER_TEXT("Engine 2 Output:"));
        check_buf(*engine2, "out_final_output");
        check_buf(*engine2, "out_denoised_ao");

        LOG_DEBUG(MOER_TEXT(""));
        LOG_DEBUG(MOER_TEXT(""));
    }
};

#undef checkCudaErrors
#undef checkCusolverErrors

} // namespace Moer::Render::Raster