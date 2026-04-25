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
#include "misc/STL.h"
#include "misc/Traits.h"
#include "platform/Platform.h"
#include "rhi/vulkan/VulkanDevice.h"
#include "rhi/vulkan/VulkanQueue.h"
#include <cuda_runtime.h>
#include <cusolverDn.h>
#include <vector>

#include "RasterResource.h"

#include <windows.h>
// 1
#include <vulkan/vulkan_core.h>
// 2
#include <vulkan/vulkan_win32.h>

namespace Moer::Render::Raster {

class CudaVulkanTools {
public:
    static void
    checkCudaErrorsInner(cudaError_t result, char const* const func, const char* const file, int const line) {
        if (result) {
            LOG_ERROR(
                MOER_TEXT("CUDA error at {}:{} code={}. error name={}. error description={}. \"{}\""),
                file,
                line,
                static_cast<uint>(result),
                cudaGetErrorName(result),
                cudaGetErrorString(result),
                func
            );
        }
    }

    static void checkCusolverErrorsInner(
        cusolverStatus_t  result,
        char const* const func,
        const char* const file,
        int const         line
    ) {
        if (result) {
            LOG_ERROR(
                MOER_TEXT("CUDA error at {}:{}. Error code={}. \"{}\""), file, line, static_cast<int32>(result), func
            );
        }
    }

    static VulkanDevice& getVulkanDevice(RenderDevice& device) {
        return *dynamic_cast<VulkanDevice*>(device.GetImpl());
    }

    static VulkanTexture& getVulkanTexture(TextureRef texture) {
        return *dynamic_cast<VulkanTexture*>(texture.Get());
    }

    static VulkanFence& getVulkanFence(FenceRef fence) {
        return *dynamic_cast<VulkanFence*>(fence.Get());
    }

    static VkCommandQueue& getVulkanQueue(CommandQueue& queue) {
        return *dynamic_cast<VkCommandQueue*>(&queue);
    }

    static VkDeviceMemory getVkDeviceMemory(VulkanDevice& vulkan_device, VulkanTexture& vulkan_texture) {
        VmaAllocationInfo info;
        vmaGetAllocationInfo(vulkan_device.GetVmaAllocator(), vulkan_texture.GetAllocation(), &info);
        return info.deviceMemory;
    }

    static VkSemaphore createSemaphoreWithoutRHI(VulkanDevice& vulkan_device) {
        VkSemaphore semaphore;

        VkSemaphoreCreateInfo create_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};

        VkExportSemaphoreCreateInfoKHR vulkanExportSemaphoreCreateInfo = {};
        vulkanExportSemaphoreCreateInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO_KHR;
        vulkanExportSemaphoreCreateInfo.pNext       = nullptr;
        vulkanExportSemaphoreCreateInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        create_info.pNext = &vulkanExportSemaphoreCreateInfo;

        VkResult result = vkCreateSemaphore(vulkan_device.GetDevice(), &create_info, nullptr, &semaphore);
        if (result != VK_SUCCESS) {
            LOG_WARNING(MOER_TEXT("CudaPass: vkCreateSemaphore Failed. VkResult = {}"), static_cast<int32>(result));
        }

        return semaphore;
    }

    static void CloseWin32Handle(HANDLE& handle) {
        if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
            CloseHandle(handle);
            handle = nullptr;
        }
    }
};

#define checkCudaErrors(val) CudaVulkanTools::checkCudaErrorsInner((val), #val, __FILE__, __LINE__)

struct CudaTexture {

    enum class EFormatElementType {
        UCHAR = 0,
        HALF,
        FLOAT,
        NUM
    };

    struct FormatDescriptor {
        cudaChannelFormatDesc desc;
        cudaTextureReadMode   read_mode;
        EFormatElementType    element_type;
        size_t                element_type_count;

        size_t element_type_bits() {
            switch (element_type) {
                case EFormatElementType::UCHAR:
                    return 8;
                case EFormatElementType::HALF:
                    return 16;
                case EFormatElementType::FLOAT:
                    return 32;
                default:
                    assert(false);
            }
        }
    };

    // non-cuda objects
    TextureRef moer_texture;
    uint       mip_levels;
    uint       width;
    uint       height;

    // 如果是cudaChannelFormatKindFloat，则必须使用float读取
    // 如果是cudaChannelFormatKindUnsigned，则必须使用uchar读取
    EFormatElementType element_type;       // float4 的 float
    size_t             element_type_count; // float4 的 4

    // cuda objects
    cudaExternalMemory_t             cudaExtMemImageBuffer;
    cudaMipmappedArray_t             cudaMipmappedImageArray;
    std::vector<cudaSurfaceObject_t> surfaceObjectList;
    cudaSurfaceObject_t*             d_surfaceObjectList;
    cudaTextureObject_t              textureObjMipMapInput;

    // win32 objects
    HANDLE win32_handle;

    cudaTextureObject_t GetTextureObject() {
        return textureObjMipMapInput;
    }

    cudaSurfaceObject_t* GetSurfaceObjectList() {
        return d_surfaceObjectList;
    }

    EFormatElementType GetElementType() const {
        return element_type;
    }

    size_t GetElementTypeCount() const {
        return element_type_count;
    }

    CudaTexture(RasterContext& context, TextureRef texture) :
        moer_texture(texture),
        mip_levels(texture.Get()->GetNumMips()),
        width(texture->GetWidth()),
        height(texture->GetHeight()) {

        if (mip_levels == 0 || width == 0 || height == 0) {
            LOG_WARNING(MOER_TEXT("CudaTexture failed to initialize, texture name: {}."), texture->GetName());
            return;
        }

        VulkanDevice&  vulkan_device  = CudaVulkanTools::getVulkanDevice(context.device);
        VulkanTexture& vulkan_texture = CudaVulkanTools::getVulkanTexture(texture);

        win32_handle = getMemoryWin32Handle(vulkan_device, vulkan_texture);

        auto format_desc   = getCudaFormatDescriptor(moer_texture->GetFormat());
        element_type       = format_desc.element_type;
        element_type_count = format_desc.element_type_count;

        {
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
            desc.handle.win32.handle = win32_handle;
            desc.size                = total_size;

            checkCudaErrors(cudaImportExternalMemory(&cudaExtMemImageBuffer, &desc));
        }
        {
            cudaExternalMemoryMipmappedArrayDesc desc;
            memset(&desc, 0, sizeof(desc));

            cudaExtent extent = make_cudaExtent(width, height, 0);

            desc.offset     = 0;
            desc.formatDesc = format_desc.desc;
            desc.extent     = extent;
            desc.flags      = 0;
            desc.numLevels  = mip_levels;

            checkCudaErrors(cudaExternalMemoryGetMappedMipmappedArray(
                &cudaMipmappedImageArray, cudaExtMemImageBuffer, &desc
            ));
        }
        { // surface
            for (uint mipLevelIdx = 0; mipLevelIdx < mip_levels; mipLevelIdx++) {
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
        { // texture
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

            texDescr.maxMipmapLevelClamp = float(mip_levels - 1);

            texDescr.readMode = format_desc.read_mode;

            checkCudaErrors(cudaCreateTextureObject(&textureObjMipMapInput, &resDescr, &texDescr, NULL));
        }
        { // surface

            assert(surfaceObjectList.size() == mip_levels);

            checkCudaErrors(cudaMalloc(
                (void**)&d_surfaceObjectList, surfaceObjectList.size() * sizeof(cudaSurfaceObject_t)
            ));

            checkCudaErrors(cudaMemcpy(
                d_surfaceObjectList,
                surfaceObjectList.data(),
                surfaceObjectList.size() * sizeof(cudaSurfaceObject_t),
                cudaMemcpyHostToDevice
            ));
        }
    }
    ~CudaTexture() {
        // destroy cuda objects
        for (int i = 0; i < mip_levels; i++) {
            checkCudaErrors(cudaDestroySurfaceObject(surfaceObjectList[i]));
        }

        checkCudaErrors(cudaFree(d_surfaceObjectList));
        checkCudaErrors(cudaFreeMipmappedArray(cudaMipmappedImageArray));
        checkCudaErrors(cudaDestroyTextureObject(textureObjMipMapInput));

        // destroy non-cuda objects

        // destroy win32 handles
        CudaVulkanTools::CloseWin32Handle(win32_handle);
    }

    CudaTexture(const CudaTexture&)            = delete;
    CudaTexture& operator=(const CudaTexture&) = delete;

private:
    static HANDLE getMemoryWin32Handle(VulkanDevice& vulkan_device, VulkanTexture& vulkan_texture) {
        HANDLE handle;

        VkDeviceMemory vk_device_memory = CudaVulkanTools::getVkDeviceMemory(vulkan_device, vulkan_texture);

        VkMemoryGetWin32HandleInfoKHR info;
        info.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
        info.pNext      = NULL;
        info.memory     = vk_device_memory;
        info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;

        // vkGetMemoryWin32HandleKHR(vulkan_device.GetDevice(), &info, &handle);
        auto result = vmaGetMemoryWin32Handle(
            vulkan_device.GetVmaAllocator(), vulkan_texture.GetAllocation(), NULL, &handle
        );
        if (result != VK_SUCCESS) {
            LOG_WARNING(
                MOER_TEXT("CudaPass: vmaGetMemoryWin32Handle Failed. VkResult = {}"), static_cast<int32>(result)
            );
        }

        return handle;
    }

    static FormatDescriptor getCudaFormatDescriptor(const EPixelFormat& pf) {
        FormatDescriptor result{};

        switch (pf) {
            case PF_R8G8B8A8_UNORM: { // color
                result.desc.x             = 8;
                result.desc.y             = 8;
                result.desc.z             = 8;
                result.desc.w             = 8;
                result.desc.f             = cudaChannelFormatKindUnsigned;
                result.read_mode          = cudaReadModeNormalizedFloat; // 只影响texture：归一化读取
                result.element_type       = EFormatElementType::UCHAR;
                result.element_type_count = 4;
                break;
            }
            case PF_R16G16B16A16_SFLOAT: { // hdr color
                result.desc.x             = 16;
                result.desc.y             = 16;
                result.desc.z             = 16;
                result.desc.w             = 16;
                result.desc.f             = cudaChannelFormatKindFloat;
                result.read_mode          = cudaReadModeElementType; // 只影响texture：原数值读取
                result.element_type       = EFormatElementType::HALF;
                result.element_type_count = 4;
                break;
            }
            case PF_D32_SFLOAT_S8_UINT: {
                assert(false);
                // cuda不支持这种格式！
                break;
            }
            case PF_D32_SFLOAT: { // depth
                result.desc.x             = 32;
                result.desc.y             = 0;
                result.desc.z             = 0;
                result.desc.w             = 0;
                result.desc.f             = cudaChannelFormatKindFloat;
                result.read_mode          = cudaReadModeElementType; // 只影响texture：原数值读取
                result.element_type       = EFormatElementType::FLOAT;
                result.element_type_count = 1;
                break;
            }
            case PF_R8_UNORM: { // ao
                result.desc.x             = 8;
                result.desc.y             = 0;
                result.desc.z             = 0;
                result.desc.w             = 0;
                result.desc.f             = cudaChannelFormatKindUnsigned;
                result.read_mode          = cudaReadModeNormalizedFloat; // 只影响texture：归一化读取
                result.element_type       = EFormatElementType::UCHAR;
                result.element_type_count = 1;
                break;
            }
            case PF_R32_SFLOAT: { // ao
                result.desc.x             = 32;
                result.desc.y             = 0;
                result.desc.z             = 0;
                result.desc.w             = 0;
                result.desc.f             = cudaChannelFormatKindFloat;
                result.read_mode          = cudaReadModeElementType; // 只影响texture：原数值读取
                result.element_type       = EFormatElementType::FLOAT;
                result.element_type_count = 1;
                break;
            }
            case PF_R16G16_SFLOAT: { // uv, motion vector
                result.desc.x             = 16;
                result.desc.y             = 16;
                result.desc.z             = 0;
                result.desc.w             = 0;
                result.desc.f             = cudaChannelFormatKindFloat;
                result.read_mode          = cudaReadModeElementType; // 只影响texture：原数值读取
                result.element_type       = EFormatElementType::HALF;
                result.element_type_count = 2;
                break;
            }
            default: {
                LOG_ERROR(MOER_TEXT("EPixelFormat doesn't support: {}"), static_cast<int>(pf));
            }
        }

        return result;
    }
}; // namespace Moer::Render::Raster

struct CudaSemaphore {

    // non-cuda objects

    VkSemaphore before_semaphore;
    VkSemaphore after_semaphore;

    // PFN
    PFN_vkGetSemaphoreWin32HandleKHR pfnGetSemaphoreWin32HandleKHR = nullptr;

    // cuda objects

    // semaphore
    cudaExternalSemaphore_t cuda_before_semaphore;
    cudaExternalSemaphore_t cuda_after_semaphore;
    cudaStream_t            stream_to_run;

    // win32 objects
    HANDLE win32_handle_before_semaphore;
    HANDLE win32_handle_after_semaphore;

    // others
    VulkanDevice&             vulkan_device;
    VkNativeQueue&            vk_native_queue;
    std::function<void(void)> pfn_sync_vk;

    CudaSemaphore(RasterContext& context) :
        vulkan_device(CudaVulkanTools::getVulkanDevice(context.device)),
        vk_native_queue(CudaVulkanTools::getVulkanQueue(context.gfx_queue).GetVkNativeQueue()) {

        pfn_sync_vk = [&]() {
            context.gfx_queue.Execute(context.cmd_list.Submit());
            context.gfx_queue.Sync();
        };

        LoadVulkanWin32PFN();

        before_semaphore = CudaVulkanTools::createSemaphoreWithoutRHI(vulkan_device);
        after_semaphore  = CudaVulkanTools::createSemaphoreWithoutRHI(vulkan_device);

        {
            VkSemaphoreGetWin32HandleInfoKHR info = {};

            info.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
            info.pNext      = NULL;
            info.semaphore  = before_semaphore;
            info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

            VkResult result = pfnGetSemaphoreWin32HandleKHR(
                vulkan_device.GetDevice(), &info, &win32_handle_before_semaphore
            );
            if (result != VK_SUCCESS || win32_handle_before_semaphore == INVALID_HANDLE_VALUE ||
                win32_handle_before_semaphore == nullptr) {
                LOG_WARNING(
                    MOER_TEXT("CudaPass: vkGetSemaphoreWin32HandleKHR Failed. VkResult = {}"), static_cast<int32>(result)
                );
            }

            cudaExternalSemaphoreHandleDesc desc = {};

            desc.type                = cudaExternalSemaphoreHandleTypeOpaqueWin32;
            desc.handle.win32.handle = win32_handle_before_semaphore;
            desc.flags               = 0;

            checkCudaErrors(cudaImportExternalSemaphore(&cuda_before_semaphore, &desc));
        }
        {
            VkSemaphoreGetWin32HandleInfoKHR info = {};

            info.sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR;
            info.pNext      = NULL;
            info.semaphore  = after_semaphore;
            info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;

            VkResult result = pfnGetSemaphoreWin32HandleKHR(
                vulkan_device.GetDevice(), &info, &win32_handle_after_semaphore
            );
            if (result != VK_SUCCESS || win32_handle_after_semaphore == INVALID_HANDLE_VALUE ||
                win32_handle_after_semaphore == nullptr) {
                LOG_WARNING(
                    MOER_TEXT("CudaPass: vkGetSemaphoreWin32HandleKHR Failed. VkResult = {}"), static_cast<int32>(result)
                );
            }

            cudaExternalSemaphoreHandleDesc desc = {};

            desc.type                = cudaExternalSemaphoreHandleTypeOpaqueWin32;
            desc.handle.win32.handle = win32_handle_after_semaphore;
            desc.flags               = 0;

            checkCudaErrors(cudaImportExternalSemaphore(&cuda_after_semaphore, &desc));
        }
        {
            checkCudaErrors(cudaStreamCreate(&stream_to_run));
        }
    }
    ~CudaSemaphore() {
        // destroy cuda objects
        checkCudaErrors(cudaDestroyExternalSemaphore(cuda_before_semaphore));
        checkCudaErrors(cudaDestroyExternalSemaphore(cuda_after_semaphore));

        // destroy non-cuda objects
        vkDestroySemaphore(vulkan_device.GetDevice(), before_semaphore, nullptr);
        vkDestroySemaphore(vulkan_device.GetDevice(), after_semaphore, nullptr);

        // destroy win32 handles
        CudaVulkanTools::CloseWin32Handle(win32_handle_before_semaphore);
        CudaVulkanTools::CloseWin32Handle(win32_handle_after_semaphore);
    }

    CudaSemaphore(const CudaSemaphore&)            = delete;
    CudaSemaphore& operator=(const CudaSemaphore&) = delete;

    void Signal() {
        // TODO: 这里默认为 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT，有优化空间
        vk_native_queue.Signal(before_semaphore);
        pfn_sync_vk();

        cudaVkSemaphoreWait(cuda_before_semaphore);
    }

    void Wait() {
        cudaVkSemaphoreSignal(cuda_after_semaphore);
        {
            auto result = cudaGetLastError();
            if (result != cudaSuccess) {
                printf("CUDA kernel error: %s\n", cudaGetErrorString(result));
            }
            // cudaStreamSynchronize(cuda_res.stream_to_run); // +50fps
        }

        // TODO: 这里默认为 VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT，有优化空间
        vk_native_queue.Wait(after_semaphore);
        pfn_sync_vk();
    }

private:
    void LoadVulkanWin32PFN() {
        pfnGetSemaphoreWin32HandleKHR = (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(
            vulkan_device.GetDevice(), "vkGetSemaphoreWin32HandleKHR"
        );

        if (!pfnGetSemaphoreWin32HandleKHR) {
            LOG_WARNING(MOER_TEXT("CudaPass: Failed to load vkGetSemaphoreWin32HandleKHR"));
        }
    }

    void cudaVkSemaphoreSignal(cudaExternalSemaphore_t& extSemaphore) {
        cudaExternalSemaphoreSignalParams extSemaphoreSignalParams;
        memset(&extSemaphoreSignalParams, 0, sizeof(extSemaphoreSignalParams));

        extSemaphoreSignalParams.params.fence.value = 0;
        extSemaphoreSignalParams.flags              = 0;
        checkCudaErrors(
            cudaSignalExternalSemaphoresAsync(&extSemaphore, &extSemaphoreSignalParams, 1, stream_to_run)
        );
    }

    void cudaVkSemaphoreWait(cudaExternalSemaphore_t& extSemaphore) {
        cudaExternalSemaphoreWaitParams extSemaphoreWaitParams;

        memset(&extSemaphoreWaitParams, 0, sizeof(extSemaphoreWaitParams));

        extSemaphoreWaitParams.params.fence.value = 0;
        extSemaphoreWaitParams.flags              = 0;

        assert(extSemaphore != nullptr);

        checkCudaErrors(
            cudaWaitExternalSemaphoresAsync(&extSemaphore, &extSemaphoreWaitParams, 1, stream_to_run)
        );
    }
};

#undef checkCudaErrors

} // namespace Moer::Render::Raster