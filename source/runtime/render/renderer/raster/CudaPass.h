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

#include "cuda_in_raster/cuda_in_raster.h"

namespace Moer::Render::Raster {

struct CudaResource {

    UniquePtr<CudaTexture>   cuda_texture;
    UniquePtr<CudaSemaphore> cuda_semaphore;

    // no RAII
    CudaResource()  = default;
    ~CudaResource() = default;

    CudaResource(const CudaResource&)            = delete;
    CudaResource& operator=(const CudaResource&) = delete;

    void CreateAllocate(RasterContext& context, TextureRef _previousPassOutputTexture) {
        // MARK: image
        cuda_texture = MakeUnique<CudaTexture>(context, _previousPassOutputTexture);
        LOG_INFO("CudaPass: Created CUDA Kernel Vulkan image buffer");

        // MARK: semaphore
        cuda_semaphore = MakeUnique<CudaSemaphore>(context);
        LOG_INFO("CudaPass: Created CUDA Imported Vulkan semaphore");
    }

    void Free(RasterContext& context) {
        cuda_texture.reset();
        cuda_semaphore.reset();
    }
};

/**
 * MARK: CUDA Pass
 * 
 * Reference: https://github.com/NVIDIA/cuda-samples/tree/master/Samples/5_Domain_Specific/vulkanImageCUDA
 */
class CudaPass {
public:
    CudaPass(RasterContext& _context, TextureRef previousPassOutputTexture) : context(_context) {
        cuda_res.CreateAllocate(context, previousPassOutputTexture);
    }
    ~CudaPass() {
        cuda_res.Free(context);
    }

    CudaPass(const CudaPass&)            = delete;
    CudaPass& operator=(const CudaPass&) = delete;

    void RecreateResource(TextureRef previousPassOutputTexture) {
        cuda_res.Free(context);
        cuda_res.CreateAllocate(context, previousPassOutputTexture);
    }

    uint Process(RasterContext& context, const RasterConfig& ui_config, uint input_image) {
        if (ui_config.ai_is_cuda_enabled == false)
            return input_image;

        // signal

        cuda_res.cuda_semaphore->Signal();

        // cuda

        const static uint64 TILE = 16;
        dim3                threadsPerBlock(TILE, TILE);
        dim3                blocksPerGrid(
            (cuda_res.cuda_texture->width - 1) / threadsPerBlock.x + 1,
            (cuda_res.cuda_texture->height - 1) / threadsPerBlock.y + 1
        );

        Moer::Cuda::d_test_2(
            blocksPerGrid,
            threadsPerBlock,
            cuda_res.cuda_semaphore->stream_to_run,
            cuda_res.cuda_texture->GetSurfaceObjectList(),
            cuda_res.cuda_texture->width,
            cuda_res.cuda_texture->height,
            cuda_res.cuda_texture->mip_levels,
            ui_config.ai_cuda_pass_debug_param
        );

        // wait

        cuda_res.cuda_semaphore->Wait();

        // return

        return input_image;
    }

private:
    RasterContext& context;
    CudaResource   cuda_res;
};

} // namespace Moer::Render::Raster