#pragma once

#include "log/LogSystem.h"
#include "misc/Traits.h"
#include "platform/Platform.h"
#include <cuda_runtime.h>
#include <vector>

#include "RasterResource.h" // 循环include，但是问题不大

#if defined(PLATFORM_WINDOWS)
// #include <vulkan/vulkan_win32.h>
#include <windows.h>
#endif

namespace Moer::Render::Raster {

struct RasterContext;

void checkCudaErrors(cudaError_t result) {
    if (result) {
        LOG_ERROR(
            "CUDA Error code={}. error name={}. error description={}",
            static_cast<uint>(result),
            cudaGetErrorName(result),
            cudaGetErrorString(result)
        );
    }
}

/**
 * 封装并管理了CUDA Pass相关的各种资源
 * 
 * 此资源需要手动申请和释放，非RAII
 * 
 * TODO: 将cuda objects封装进runtime
 */
struct CudaResource {

    bool isValid = false;

    // non-cuda objects
    TextureRef previousPassOutputTexture;
    uint       mipLevels;

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

    // no RAII
    CudaResource() {}
    ~CudaResource() {}

    CudaResource(const CudaResource&)            = delete;
    CudaResource& operator=(const CudaResource&) = delete;

    bool IsValid() { return mipLevels != 0; }

    // #if defined(PLATFORM_WINDOWS)
    void CreateAllocate(RasterContext& context, TextureRef _previousPassOutputTexture) {

        previousPassOutputTexture = _previousPassOutputTexture;
        mipLevels                 = previousPassOutputTexture.Get()->GetNumMips();

        if (mipLevels == 0) {
            LOG_WARNING("Raster Pipeline, The number of mipmaps of textures are ZERO!");
            return;
        }

        // MARK: image

        {
            cudaExternalMemoryHandleDesc desc;
            memset(&desc, 0, sizeof(desc));
            desc.type = cudaExternalMemoryHandleTypeOpaqueWin32;
            {
                // HANDLE handle;

                // VkMemoryGetWin32HandleInfoKHR info;
                // info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
                // info.pNext      = NULL;
                // info.memory     = textureImageMemory;
                // info.handleType = (VkExternalMemoryHandleTypeFlagBitsKHR)externalMemoryHandleType;

                // vkGetMemoryWin32HandleKHR();
            }
        }

        // MARK: semaphore
    }

    void Free() {

        if (mipLevels == 0) return;

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
    }

    // #else
    //     void CreateAllocate(RasterContext context, TextureRef _previousPassOutputTexture) {
    //         LOG_WARNING("OS isn't Windows, CUDA Pass is invalid!");
    //         return;
    //     }
    //     void Free() { return; }
    // #endif
};

} // namespace Moer::Render::Raster