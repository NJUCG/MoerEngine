/**
 * 此文件应该只有在宏 CUDA_PASS_IN_RASTER 被设置的情况下使用
 * 
 * 这个宏启用时，默认环境为Windows11+Vulkan；所以其他地方不再判断
 */
#pragma once

#if !defined(CUDA_PASS_IN_RASTER)
#error "This header requires CUDA_PASS_IN_RASTER=1"
#endif

#include "log/LogSystem.h"
#include "misc/Traits.h"
#include "platform/Platform.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanQueue.h"
#include "rhi/vulkan/platform/windows/WindowsSecurityAttributes.h" // For creating semaphore
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

#include "boxfilter/boxfilter.h"

namespace Moer::Render::Raster {

void checkCudaErrorsInner(
    cudaError_t       result,
    char const* const func,
    const char* const file,
    int const         line
) {
    if (result) {
        LOG_ERROR(
            "CUDA error at {}:{} code={}. error name={}. error description={}. \"{}\"",
            file,
            line,
            static_cast<uint>(result),
            cudaGetErrorName(result),
            cudaGetErrorString(result),
            func
        );
    }
}

#define checkCudaErrors(val) checkCudaErrorsInner((val), #val, __FILE__, __LINE__)

VulkanDevice& getVulkanDevice(RenderDevice& device) {
    return *dynamic_cast<VulkanDevice*>(device.GetImpl());
}

VulkanTexture& getVulkanTexture(TextureRef texture) {
    return *dynamic_cast<VulkanTexture*>(texture.Get());
}

VulkanFence& getVulkanFence(FenceRef fence) {
    return *dynamic_cast<VulkanFence*>(fence.Get());
}

VkCommandQueue& getVulkanQueue(CommandQueue& queue) {
    return *dynamic_cast<VkCommandQueue*>(&queue);
}

VkDeviceMemory getVkDeviceMemory(VulkanDevice& vulkan_device, VulkanTexture& vulkan_texture) {
    VmaAllocationInfo info;
    vmaGetAllocationInfo(vulkan_device.GetVmaAllocator(), vulkan_texture.GetAllocation(), &info);
    return info.deviceMemory;
}

VkSemaphore createSemaphoreWithoutRHI(VulkanDevice& vulkan_device) {
    VkSemaphore semaphore;

    VkSemaphoreCreateInfo create_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

    WindowsSecurityAttributes winSecurityAttributes;

    VkExportSemaphoreWin32HandleInfoKHR vulkanExportSemaphoreWin32HandleInfoKHR = {};
    vulkanExportSemaphoreWin32HandleInfoKHR.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    vulkanExportSemaphoreWin32HandleInfoKHR.pNext = NULL;
    vulkanExportSemaphoreWin32HandleInfoKHR.pAttributes = &winSecurityAttributes;
    vulkanExportSemaphoreWin32HandleInfoKHR.dwAccess = DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE;
    vulkanExportSemaphoreWin32HandleInfoKHR.name     = (LPCWSTR)NULL;

    VkExportSemaphoreCreateInfoKHR vulkanExportSemaphoreCreateInfo = {};
    vulkanExportSemaphoreCreateInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO_KHR;
    vulkanExportSemaphoreCreateInfo.pNext       = &vulkanExportSemaphoreWin32HandleInfoKHR;
    vulkanExportSemaphoreCreateInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

    create_info.pNext = &vulkanExportSemaphoreCreateInfo;

    VkResult result = vkCreateSemaphore(vulkan_device.GetDevice(), &create_info, nullptr, &semaphore);
    if (result != VK_SUCCESS) {
        LOG_WARNING("CudaPass: vkCreateSemaphore Failed. VkResult = {}", static_cast<int32>(result));
    }

    return semaphore;
}

struct CudaResource {
    bool isValid = false;

    // MARK: non-cuda objects
    TextureRef previousPassOutputTexture;
    uint       mipLevels;
    uint       imageWidth;
    uint       imageHeight;

    VkSemaphore cudaUpdateVkSemaphore;
    VkSemaphore vkUpdateCudaSemaphore;

    // MARK: cuda objects

    // image
    cudaExternalMemory_t             cudaExtMemImageBuffer;
    cudaMipmappedArray_t             cudaMipmappedImageArray;
    std::vector<cudaSurfaceObject_t> surfaceObjectList;
    cudaSurfaceObject_t*             d_surfaceObjectList;
    cudaTextureObject_t              textureObjMipMapInput;

    // semaphore
    cudaExternalSemaphore_t cudaExtCudaUpdateVkSemaphore;
    cudaExternalSemaphore_t cudaExtVkUpdateCudaSemaphore;
    cudaStream_t            streamToRun;

    // MARK: win32 objects
    HANDLE texture_handle;
    HANDLE semaphore_cuda2vk_handle;
    HANDLE semaphore_vk2cuda_handle;

    // MARK: PFN
    PFN_vkGetSemaphoreWin32HandleKHR pfnGetSemaphoreWin32HandleKHR = nullptr;

    // no RAII
    CudaResource()  = default;
    ~CudaResource() = default;

    CudaResource(const CudaResource&)            = delete;
    CudaResource& operator=(const CudaResource&) = delete;

    bool IsValid() const {
        return mipLevels != 0;
    }

    void CreateAllocate(RasterContext& context, TextureRef _previousPassOutputTexture) {

        // MARK: non-cuda

        previousPassOutputTexture = _previousPassOutputTexture;
        mipLevels                 = previousPassOutputTexture.Get()->GetNumMips();
        imageWidth                = _previousPassOutputTexture->GetWidth();
        imageHeight               = _previousPassOutputTexture->GetHeight();

        if (!IsValid()) {
            LOG_WARNING("Raster Pipeline, The number of mipmaps of textures are ZERO!");
            return;
        }

        VulkanDevice& vulkan_device = getVulkanDevice(context.device);

        cudaUpdateVkSemaphore = createSemaphoreWithoutRHI(vulkan_device);
        vkUpdateCudaSemaphore = createSemaphoreWithoutRHI(vulkan_device);

        // MARK: image

        VulkanTexture& vulkan_texture   = getVulkanTexture(_previousPassOutputTexture);
        VkDeviceMemory vk_device_memory = getVkDeviceMemory(vulkan_device, vulkan_texture);

        {
            {
                VkMemoryGetWin32HandleInfoKHR info;
                info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
                info.pNext      = NULL;
                info.memory     = vk_device_memory;
                info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

                // vkGetMemoryWin32HandleKHR(vulkan_device.GetDevice(), &info, &handle);
                auto result = vmaGetMemoryWin32Handle(
                    vulkan_device.GetVmaAllocator(), vulkan_texture.GetAllocation(), NULL, &texture_handle
                );
                if (result != VK_SUCCESS) {
                    LOG_WARNING(
                        "CudaPass: vmaGetMemoryWin32Handle Failed. VkResult = {}", static_cast<int32>(result)
                    );
                }
            }

            uint64 total_size = [&]() {
                VkMemoryRequirements vkMemoryRequirements = {};
                vkGetImageMemoryRequirements(
                    vulkan_device.GetDevice(), vulkan_texture.GetHandle(), &vkMemoryRequirements
                );
                return vkMemoryRequirements.size;
            }();

            cudaExternalMemoryHandleDesc desc;
            memset(&desc, 0, sizeof(desc));

            desc.type                = cudaExternalMemoryHandleTypeOpaqueWin32;
            desc.handle.win32.handle = texture_handle;
            desc.size                = total_size;

            checkCudaErrors(cudaImportExternalMemory(&cudaExtMemImageBuffer, &desc));
        }
        {
            cudaExternalMemoryMipmappedArrayDesc desc;
            memset(&desc, 0, sizeof(desc));

            cudaExtent            extent = make_cudaExtent(imageWidth, imageHeight, 0);
            cudaChannelFormatDesc formatDesc; // PF_R8G8B8A8_UNORM
            formatDesc.x = 8;
            formatDesc.y = 8;
            formatDesc.z = 8;
            formatDesc.w = 8;
            formatDesc.f = cudaChannelFormatKindUnsigned;

            desc.offset     = 0;
            desc.formatDesc = formatDesc;
            desc.extent     = extent;
            desc.flags      = 0;
            desc.numLevels  = mipLevels;

            checkCudaErrors(cudaExternalMemoryGetMappedMipmappedArray(
                &cudaMipmappedImageArray, cudaExtMemImageBuffer, &desc
            ));
        }
        {
            for (uint mipLevelIdx = 0; mipLevelIdx < mipLevels; mipLevelIdx++) {
                cudaArray_t      cudaMipLevelArray;
                cudaResourceDesc resourceDesc;

                checkCudaErrors(
                    cudaGetMipmappedArrayLevel(&cudaMipLevelArray, cudaMipmappedImageArray, mipLevelIdx)
                );

                memset(&resourceDesc, 0, sizeof(resourceDesc));
                resourceDesc.resType         = cudaResourceTypeArray;
                resourceDesc.res.array.array = cudaMipLevelArray;

                cudaSurfaceObject_t surfaceObject;
                checkCudaErrors(cudaCreateSurfaceObject(&surfaceObject, &resourceDesc));

                surfaceObjectList.push_back(surfaceObject);
            }
        }
        {
            cudaResourceDesc resDescr;
            memset(&resDescr, 0, sizeof(cudaResourceDesc));

            resDescr.resType           = cudaResourceTypeMipmappedArray;
            resDescr.res.mipmap.mipmap = cudaMipmappedImageArray;

            cudaTextureDesc texDescr;
            memset(&texDescr, 0, sizeof(cudaTextureDesc));

            texDescr.normalizedCoords = true;
            texDescr.filterMode       = cudaFilterModeLinear;
            texDescr.mipmapFilterMode = cudaFilterModeLinear;

            texDescr.addressMode[0] = cudaAddressModeWrap;
            texDescr.addressMode[1] = cudaAddressModeWrap;

            texDescr.maxMipmapLevelClamp = float(mipLevels - 1);

            texDescr.readMode = cudaReadModeNormalizedFloat;

            checkCudaErrors(cudaCreateTextureObject(&textureObjMipMapInput, &resDescr, &texDescr, NULL));

            checkCudaErrors(
                cudaMalloc((void**)&d_surfaceObjectList, sizeof(cudaSurfaceObject_t) * mipLevels)
            );

            checkCudaErrors(cudaMemcpy(
                d_surfaceObjectList,
                surfaceObjectList.data(),
                sizeof(cudaSurfaceObject_t) * mipLevels,
                cudaMemcpyHostToDevice
            ));
        }
        LOG_INFO("CudaPass: Created CUDA Kernel Vulkan image buffer");

        // MARK: semaphore

        {
            VkSemaphoreGetWin32HandleInfoKHR info = {};

            info.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
            info.pNext      = NULL;
            info.semaphore  = cudaUpdateVkSemaphore;
            info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

            VkResult result =
                pfnGetSemaphoreWin32HandleKHR(vulkan_device.GetDevice(), &info, &semaphore_cuda2vk_handle);
            if (result != VK_SUCCESS || semaphore_cuda2vk_handle == INVALID_HANDLE_VALUE ||
                semaphore_cuda2vk_handle == nullptr) {
                LOG_WARNING(
                    "CudaPass: vkGetSemaphoreWin32HandleKHR Failed. VkResult = {}", static_cast<int32>(result)
                );
            }

            cudaExternalSemaphoreHandleDesc desc = {};

            desc.type                = cudaExternalSemaphoreHandleTypeOpaqueWin32;
            desc.handle.win32.handle = semaphore_cuda2vk_handle;
            desc.flags               = 0;

            checkCudaErrors(cudaImportExternalSemaphore(&cudaExtCudaUpdateVkSemaphore, &desc));
        }
        {
            VkSemaphoreGetWin32HandleInfoKHR info = {};

            info.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
            info.pNext      = NULL;
            info.semaphore  = vkUpdateCudaSemaphore;
            info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

            VkResult result =
                pfnGetSemaphoreWin32HandleKHR(vulkan_device.GetDevice(), &info, &semaphore_vk2cuda_handle);
            if (result != VK_SUCCESS || semaphore_vk2cuda_handle == INVALID_HANDLE_VALUE ||
                semaphore_vk2cuda_handle == nullptr) {
                LOG_WARNING(
                    "CudaPass: vkGetSemaphoreWin32HandleKHR Failed. VkResult = {}", static_cast<int32>(result)
                );
            }

            cudaExternalSemaphoreHandleDesc desc = {};

            desc.type                = cudaExternalSemaphoreHandleTypeOpaqueWin32;
            desc.handle.win32.handle = semaphore_vk2cuda_handle;
            desc.flags               = 0;

            checkCudaErrors(cudaImportExternalSemaphore(&cudaExtVkUpdateCudaSemaphore, &desc));
        }
        {
            checkCudaErrors(cudaStreamCreate(&streamToRun));
        }
        LOG_INFO("CudaPass: Created CUDA Imported Vulkan semaphore");
    }

    void Free(RasterContext& context) {
        if (!IsValid())
            return;

        VulkanDevice& vulkan_device = getVulkanDevice(context.device);

        // destroy cuda objects

        for (int i = 0; i < mipLevels; i++) {
            checkCudaErrors(cudaDestroySurfaceObject(surfaceObjectList[i]));
        }

        checkCudaErrors(cudaFree(d_surfaceObjectList));
        checkCudaErrors(cudaFreeMipmappedArray(cudaMipmappedImageArray));
        checkCudaErrors(cudaDestroyTextureObject(textureObjMipMapInput));

        checkCudaErrors(cudaDestroyExternalSemaphore(cudaExtCudaUpdateVkSemaphore));
        checkCudaErrors(cudaDestroyExternalSemaphore(cudaExtVkUpdateCudaSemaphore));

        // destroy non-cuda objects

        vkDestroySemaphore(vulkan_device.GetDevice(), cudaUpdateVkSemaphore, nullptr);
        vkDestroySemaphore(vulkan_device.GetDevice(), vkUpdateCudaSemaphore, nullptr);

        // destroy win32 handles

        CloseWin32Handle(texture_handle);
        CloseWin32Handle(semaphore_cuda2vk_handle);
        CloseWin32Handle(semaphore_vk2cuda_handle);
    }

    void LoadVulkanWin32PFN(RasterContext& context) {
        VulkanDevice& vulkan_device = getVulkanDevice(context.device);

        pfnGetSemaphoreWin32HandleKHR = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(
            vulkan_device.GetDevice(), "vkGetSemaphoreWin32HandleKHR"
        );

        if (!pfnGetSemaphoreWin32HandleKHR) {
            LOG_WARNING("CudaPass: Failed to load vkGetSemaphoreWin32HandleKHR");
        }
    }

private:
    void CloseWin32Handle(HANDLE& handle) {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
            handle = nullptr;
        }
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
        cuda_res.LoadVulkanWin32PFN(context);
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

        const static uint64 TILE = 16;

        dim3 threadsPerBlock(TILE, TILE);
        dim3 blocksPerGrid(
            (cuda_res.imageWidth - 1) / threadsPerBlock.x + 1,
            (cuda_res.imageHeight - 1) / threadsPerBlock.y + 1
        );

        Moer::Cuda::d_test(
            blocksPerGrid,
            threadsPerBlock,
            cuda_res.streamToRun,
            cuda_res.textureObjMipMapInput,
            cuda_res.d_surfaceObjectList,
            cuda_res.imageWidth,
            cuda_res.imageHeight,
            cuda_res.mipLevels,
            ui_config.ai_cuda_pass_debug_param
        );

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