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
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include <cuda_runtime.h>
#include <vector>

#include "RasterConfig.h"
#include "RasterResource.h"
#include "RasterTool.h"

#include "rhi/vulkan/VulkanDevice.h"
#include <windows.h>
// 1
#include <vulkan/vulkan_core.h>
// 2
#include <vulkan/vulkan_win32.h>

// For creating semaphore
#include "rhi/vulkan/platform/windows/WindowsSecurityAttributes.h"

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
    // FenceRef   moerCudaUpdateVkSemaphore;
    // FenceRef   moerVkUpdateCudaSemaphore;
    VkSemaphore moerCudaUpdateVkSemaphore;
    VkSemaphore moerVkUpdateCudaSemaphore;

    // MARK: cuda objects

    // image
    cudaExternalMemory_t cudaExtMemImageBuffer;
    cudaMipmappedArray_t cudaMipmappedImageArray, cudaMipmappedImageArrayTemp, cudaMipmappedImageArrayOrig;
    std::vector<cudaSurfaceObject_t> surfaceObjectList, surfaceObjectListTemp;
    cudaSurfaceObject_t *            d_surfaceObjectList, *d_surfaceObjectListTemp;
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

    void CreateAllocate(RasterContext& context, TextureRef _previousPassOutputTexture) {

        // MARK: non-cuda

        previousPassOutputTexture = _previousPassOutputTexture;
        mipLevels                 = previousPassOutputTexture.Get()->GetNumMips();

        if (mipLevels == 0) {
            LOG_WARNING("Raster Pipeline, The number of mipmaps of textures are ZERO!");
            return;
        }

        VulkanDevice& vulkan_device = getVulkanDevice(context.device);

        // moerCudaUpdateVkSemaphore = context.device.CreateFence();
        // moerVkUpdateCudaSemaphore = context.device.CreateFence();
        moerCudaUpdateVkSemaphore = createSemaphoreWithoutRHI(vulkan_device);
        moerVkUpdateCudaSemaphore = createSemaphoreWithoutRHI(vulkan_device);

        // MARK: image

        VulkanTexture& vulkan_texture   = getVulkanTexture(_previousPassOutputTexture);
        VkDeviceMemory vk_device_memory = getVkDeviceMemory(vulkan_device, vulkan_texture);
        uint64         imageWidth       = _previousPassOutputTexture->GetWidth();
        uint64         imageHeight      = _previousPassOutputTexture->GetHeight();

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

            checkCudaErrors(
                cudaMallocMipmappedArray(&cudaMipmappedImageArrayTemp, &formatDesc, extent, mipLevels)
            );
            checkCudaErrors(
                cudaMallocMipmappedArray(&cudaMipmappedImageArrayOrig, &formatDesc, extent, mipLevels)
            );
        }
        {
            for (uint mipLevelIdx = 0; mipLevelIdx < mipLevels; mipLevelIdx++) {
                cudaArray_t      cudaMipLevelArray, cudaMipLevelArrayTemp, cudaMipLevelArrayOrig;
                cudaResourceDesc resourceDesc;

                checkCudaErrors(
                    cudaGetMipmappedArrayLevel(&cudaMipLevelArray, cudaMipmappedImageArray, mipLevelIdx)
                );
                checkCudaErrors(cudaGetMipmappedArrayLevel(
                    &cudaMipLevelArrayTemp, cudaMipmappedImageArrayTemp, mipLevelIdx
                ));
                checkCudaErrors(cudaGetMipmappedArrayLevel(
                    &cudaMipLevelArrayOrig, cudaMipmappedImageArrayOrig, mipLevelIdx
                ));

                uint32_t width  = (imageWidth >> mipLevelIdx) ? (imageWidth >> mipLevelIdx) : 1;
                uint32_t height = (imageHeight >> mipLevelIdx) ? (imageHeight >> mipLevelIdx) : 1;
                checkCudaErrors(cudaMemcpy2DArrayToArray(
                    cudaMipLevelArrayOrig,
                    0,
                    0,
                    cudaMipLevelArray,
                    0,
                    0,
                    width * sizeof(uchar4),
                    height,
                    cudaMemcpyDeviceToDevice
                ));

                memset(&resourceDesc, 0, sizeof(resourceDesc));
                resourceDesc.resType         = cudaResourceTypeArray;
                resourceDesc.res.array.array = cudaMipLevelArray;

                cudaSurfaceObject_t surfaceObject;
                checkCudaErrors(cudaCreateSurfaceObject(&surfaceObject, &resourceDesc));

                surfaceObjectList.push_back(surfaceObject);

                memset(&resourceDesc, 0, sizeof(resourceDesc));
                resourceDesc.resType         = cudaResourceTypeArray;
                resourceDesc.res.array.array = cudaMipLevelArrayTemp;

                cudaSurfaceObject_t surfaceObjectTemp;
                checkCudaErrors(cudaCreateSurfaceObject(&surfaceObjectTemp, &resourceDesc));
                surfaceObjectListTemp.push_back(surfaceObjectTemp);
            }
        }
        {

            cudaResourceDesc resDescr;
            memset(&resDescr, 0, sizeof(cudaResourceDesc));

            resDescr.resType           = cudaResourceTypeMipmappedArray;
            resDescr.res.mipmap.mipmap = cudaMipmappedImageArrayOrig;

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
            checkCudaErrors(
                cudaMalloc((void**)&d_surfaceObjectListTemp, sizeof(cudaSurfaceObject_t) * mipLevels)
            );

            checkCudaErrors(cudaMemcpy(
                d_surfaceObjectList,
                surfaceObjectList.data(),
                sizeof(cudaSurfaceObject_t) * mipLevels,
                cudaMemcpyHostToDevice
            ));
            checkCudaErrors(cudaMemcpy(
                d_surfaceObjectListTemp,
                surfaceObjectListTemp.data(),
                sizeof(cudaSurfaceObject_t) * mipLevels,
                cudaMemcpyHostToDevice
            ));
        }
        LOG_INFO("CudaPass: Created CUDA Kernel Vulkan image buffer");

        // MARK: semaphore

        VkSemaphore& cudaUpdateVkSemaphore = moerCudaUpdateVkSemaphore;
        VkSemaphore& vkUpdateCudaSemaphore = moerVkUpdateCudaSemaphore;

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
        LOG_INFO("CudaPass: Created CUDA Imported Vulkan semaphore");
    }

    void Free() {

        if (mipLevels == 0)
            return;

        // destroy cuda objects

        for (int i = 0; i < mipLevels; i++) {
            checkCudaErrors(cudaDestroySurfaceObject(surfaceObjectList[i]));
            checkCudaErrors(cudaDestroySurfaceObject(surfaceObjectListTemp[i]));
        }

        checkCudaErrors(cudaFree(d_surfaceObjectList));
        checkCudaErrors(cudaFree(d_surfaceObjectListTemp));
        checkCudaErrors(cudaFreeMipmappedArray(cudaMipmappedImageArrayTemp));
        checkCudaErrors(cudaFreeMipmappedArray(cudaMipmappedImageArrayOrig));
        checkCudaErrors(cudaFreeMipmappedArray(cudaMipmappedImageArray));
        checkCudaErrors(cudaDestroyTextureObject(textureObjMipMapInput));

        checkCudaErrors(cudaDestroyExternalSemaphore(cudaExtCudaUpdateVkSemaphore));
        checkCudaErrors(cudaDestroyExternalSemaphore(cudaExtVkUpdateCudaSemaphore));

        // destroy non-cuda objects

        // TODO: semaphores

        // destroy win32 handles

        CloseHandle(texture_handle);
        CloseHandle(semaphore_cuda2vk_handle);
        CloseHandle(semaphore_vk2cuda_handle);
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
};

/**
 * MARK: CUDA Pass
 * 
 * Reference: https://github.com/NVIDIA/cuda-samples/tree/master/Samples/5_Domain_Specific/vulkanImageCUDA
 */
class CudaPass {
public:
    CudaPass(RasterContext& context, TextureRef previousPassOutputTexture) {
        cuda_resource.LoadVulkanWin32PFN(context);
        cuda_resource.CreateAllocate(context, previousPassOutputTexture);
    }
    ~CudaPass() {
        cuda_resource.Free();
    }

    CudaPass(const CudaPass&)            = delete;
    CudaPass& operator=(const CudaPass&) = delete;

    uint Process(RasterContext& context, const RasterConfig& ui_config, uint input_image) {
        if (ui_config.ai_is_cuda_enabled == false)
            return input_image;

        return input_image;
    }

private:
    CudaResource cuda_resource;
};

#undef checkCudaErrors

} // namespace Moer::Render::Raster