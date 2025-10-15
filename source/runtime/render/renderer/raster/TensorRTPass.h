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
#include "misc/Traits.h"
#include "platform/Platform.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanQueue.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include <cuda_runtime.h>
#include <vector>

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

#include <windows.h>
// 1
#include <vulkan/vulkan_core.h>
// 2
#include <vulkan/vulkan_win32.h>

namespace Moer::Render::Raster {

/**
 * MARK: TensorRT Pass
 */
class TensorRTPass {
public:
    TensorRTPass(RasterContext& _context, TextureRef previousPassOutputTexture) : context(_context) {
        cuda_res.LoadVulkanWin32PFN(context);
        cuda_res.CreateAllocate(context, previousPassOutputTexture);
    }
    ~TensorRTPass() {
        cuda_res.Free(context);
    }

    TensorRTPass(const TensorRTPass&)            = delete;
    TensorRTPass& operator=(const TensorRTPass&) = delete;

    void RecreateResource(TextureRef previousPassOutputTexture) {
        cuda_res.Free(context);
        cuda_res.CreateAllocate(context, previousPassOutputTexture);
    }

    uint Process(RasterContext& context, const RasterConfig& ui_config, uint input_image) {
        if (ui_config.ai_is_cuda_enabled == false || !cuda_res.IsValid())
            return input_image;

        VkNativeQueue& vk_native_queue = getVulkanQueue(context.gfx_queue).GetVkNativeQueue();

        // vulkan

        // TODO: 这里默认为 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT，有优化空间
        vk_native_queue.Signal(cuda_res.vkUpdateCudaSemaphore);

        context.gfx_queue.Execute(context.cmd_list.Submit());
        context.gfx_queue.Sync();

        // cuda

        cudaVkSemaphoreWait(cuda_res.cudaExtVkUpdateCudaSemaphore);

        // ...

        cudaVkSemaphoreSignal(cuda_res.cudaExtCudaUpdateVkSemaphore);

        {
            auto result = cudaGetLastError();
            if (result != cudaSuccess) {
                printf("CUDA kernel error: %s\n", cudaGetErrorString(result));
            }
            // cudaStreamSynchronize(cuda_res.streamToRun); // +50fps
        }

        // vulkan

        // TODO: 这里默认为 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT，有优化空间
        vk_native_queue.Wait(cuda_res.cudaUpdateVkSemaphore);

        context.gfx_queue.Execute(context.cmd_list.Submit());
        context.gfx_queue.Sync();

        return input_image;
    }

private:
    void cudaVkSemaphoreSignal(cudaExternalSemaphore_t& extSemaphore) {
        cudaExternalSemaphoreSignalParams extSemaphoreSignalParams;
        memset(&extSemaphoreSignalParams, 0, sizeof(extSemaphoreSignalParams));

        extSemaphoreSignalParams.params.fence.value = 0;
        extSemaphoreSignalParams.flags              = 0;
        checkCudaErrors(cudaSignalExternalSemaphoresAsync(
            &extSemaphore, &extSemaphoreSignalParams, 1, cuda_res.streamToRun
        ));
    }

    void cudaVkSemaphoreWait(cudaExternalSemaphore_t& extSemaphore) {
        cudaExternalSemaphoreWaitParams extSemaphoreWaitParams;

        memset(&extSemaphoreWaitParams, 0, sizeof(extSemaphoreWaitParams));

        extSemaphoreWaitParams.params.fence.value = 0;
        extSemaphoreWaitParams.flags              = 0;

        assert(extSemaphore != nullptr);

        checkCudaErrors(
            cudaWaitExternalSemaphoresAsync(&extSemaphore, &extSemaphoreWaitParams, 1, cuda_res.streamToRun)
        );
    }

private:
    RasterContext& context;
    CudaResource   cuda_res;

    int filter_radius = 14;
    int g_nFilterSign = 1;
};

#undef checkCudaErrors

} // namespace Moer::Render::Raster