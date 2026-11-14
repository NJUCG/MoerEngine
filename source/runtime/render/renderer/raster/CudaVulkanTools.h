/**
 * 此文件应该只有在宏 WITH_CUDA 被设置的情况下使用
 * 
 * 这个宏启用时，默认环境为Windows11+Vulkan；所以其他地方不再判断
 */
#pragma once

#include "misc/Types.h"

#if !defined(WITH_CUDA)
#error "This header requires WITH_CUDA=1"
#endif

#include "log/LogSystem.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "platform/Platform.h"
#include "rhi/executor/RHIExecutor.h"
#include "rhi/backends/vulkan/VulkanDevice.h"
#include "rhi/backends/vulkan/VulkanTimeline.h"
#include <cuda_runtime.h>
#include <cusolverDn.h>
#include <initializer_list>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "RasterResource.h"

#include <windows.h>

namespace Moer::Render::Raster {

class CudaVulkanTools {
public:
    static void
    checkCudaErrorsInner(cudaError_t result, char const* const func, const char* const file, int const line) {
        if (result) {
            LOG_ERROR(
                "CUDA error at {}:{} code={}. error name={}. error description={}. \"{}\"",
                file,
                line,
                static_cast<uint32>(result),
                cudaGetErrorName(result),
                cudaGetErrorString(result),
                func
            );
            throw std::runtime_error(std::string(func) + ": " + cudaGetErrorString(result));
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
                "CUDA error at {}:{}. Error code={}. \"{}\"", file, line, static_cast<int32>(result), func
            );
            throw std::runtime_error(std::string(func) + ": cuSOLVER error " + std::to_string(result));
        }
    }

    static VulkanDevice& getVulkanDevice(RenderDevice& device) {
        return *dynamic_cast<VulkanDevice*>(device.GetImpl());
    }

    static VulkanTexture& getVulkanTexture(TextureRef texture) {
        return *dynamic_cast<VulkanTexture*>(texture.Get());
    }

    static VulkanTimeline& getVulkanTimeline(FenceRef fence) {
        return *dynamic_cast<VulkanTimeline*>(fence.Get());
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
    uint32     mip_levels;
    uint32     width;
    uint32     height;

    // 如果是cudaChannelFormatKindFloat，则必须使用float读取
    // 如果是cudaChannelFormatKindUnsigned，则必须使用uchar读取
    EFormatElementType element_type;       // float4 的 float
    size_t             element_type_count; // float4 的 4

    // cuda objects
    cudaExternalMemory_t             cudaExtMemImageBuffer{};
    cudaMipmappedArray_t             cudaMipmappedImageArray{};
    std::vector<cudaSurfaceObject_t> surfaceObjectList;
    cudaSurfaceObject_t*             d_surfaceObjectList{};
    cudaTextureObject_t              textureObjMipMapInput{};

    // win32 objects
    HANDLE win32_handle{};

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

    CudaTexture(RasterContext& context, TextureRef texture) : CudaTexture(context.device, texture) {}

    CudaTexture(RenderDevice& device, TextureRef texture) :
        moer_texture(texture),
        mip_levels(texture.Get()->GetNumMips()),
        width(texture->GetWidth()),
        height(texture->GetHeight()) {

        try {
            if (mip_levels == 0 || width == 0 || height == 0) {
                throw std::invalid_argument("CUDA image extent and mip count must be nonzero");
            }

            VulkanDevice&  vulkan_device  = CudaVulkanTools::getVulkanDevice(device);
            VulkanTexture& vulkan_texture = CudaVulkanTools::getVulkanTexture(texture);

            win32_handle = getMemoryWin32Handle(vulkan_device, vulkan_texture);

            auto format_desc   = getCudaFormatDescriptor(moer_texture->GetFormat());
            element_type       = format_desc.element_type;
            element_type_count = format_desc.element_type_count;

            {
                VmaAllocationInfo2 allocation{};
                vmaGetAllocationInfo2(
                    vulkan_device.GetVmaAllocator(), vulkan_texture.GetAllocation(), &allocation
                );
                cudaExternalMemoryHandleDesc desc;
                memset(&desc, 0, sizeof(desc));

                desc.type                = cudaExternalMemoryHandleTypeOpaqueWin32;
                desc.handle.win32.handle = win32_handle;
                // The exported handle names the entire VkDeviceMemory block.
                desc.size  = allocation.blockSize;
                desc.flags = allocation.dedicatedMemory ? cudaExternalMemoryDedicated : 0;

                checkCudaErrors(cudaImportExternalMemory(&cudaExtMemImageBuffer, &desc));
                CudaVulkanTools::CloseWin32Handle(win32_handle);
            }
            {
                cudaExternalMemoryMipmappedArrayDesc desc;
                memset(&desc, 0, sizeof(desc));

                cudaExtent extent = make_cudaExtent(width, height, 0);

                VmaAllocationInfo allocation{};
                vmaGetAllocationInfo(
                    vulkan_device.GetVmaAllocator(), vulkan_texture.GetAllocation(), &allocation
                );
                desc.offset     = allocation.offset;
                desc.formatDesc = format_desc.desc;
                desc.extent     = extent;
                desc.flags      = cudaArraySurfaceLoadStore;
                if ((texture->GetUsage() & ETextureUsageFlags::COLOR_ATTACHMENT) !=
                    ETextureUsageFlags::UNDEFINED) {
                    desc.flags |= cudaArrayColorAttachment;
                }
                desc.numLevels = mip_levels;

                checkCudaErrors(cudaExternalMemoryGetMappedMipmappedArray(
                    &cudaMipmappedImageArray, cudaExtMemImageBuffer, &desc
                ));
            }
            { // surface
                for (uint32 mipLevelIdx = 0; mipLevelIdx < mip_levels; mipLevelIdx++) {
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
        } catch (...) {
            Reset();
            throw;
        }
    }
    ~CudaTexture() {
        Reset();
    }

    CudaTexture(const CudaTexture&)            = delete;
    CudaTexture& operator=(const CudaTexture&) = delete;

private:
    void Reset() noexcept {
        // Objects must die before their mapped array and imported memory.
        if (textureObjMipMapInput)
            cudaDestroyTextureObject(textureObjMipMapInput);
        for (auto surface : surfaceObjectList)
            cudaDestroySurfaceObject(surface);
        if (d_surfaceObjectList)
            cudaFree(d_surfaceObjectList);
        if (cudaMipmappedImageArray)
            cudaFreeMipmappedArray(cudaMipmappedImageArray);
        if (cudaExtMemImageBuffer)
            cudaDestroyExternalMemory(cudaExtMemImageBuffer);
        CudaVulkanTools::CloseWin32Handle(win32_handle);
        textureObjMipMapInput = 0;
        surfaceObjectList.clear();
        d_surfaceObjectList     = nullptr;
        cudaMipmappedImageArray = nullptr;
        cudaExtMemImageBuffer   = nullptr;
    }

    static HANDLE getMemoryWin32Handle(VulkanDevice& vulkan_device, VulkanTexture& vulkan_texture) {
        HANDLE handle{};

        auto result = vmaGetMemoryWin32Handle(
            vulkan_device.GetVmaAllocator(), vulkan_texture.GetAllocation(), NULL, &handle
        );
        if (result != VK_SUCCESS) {
            throw std::runtime_error("Vulkan external image memory export failed");
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
                LOG_ERROR("EPixelFormat doesn't support: {}", static_cast<int>(pf));
            }
        }

        return result;
    }
}; // namespace Moer::Render::Raster

// A synchronous interop boundary between two compiled graphics graphs.
// Public images enter and leave in Sampled; CUDA owns them in GENERAL while
// its stream runs. RHI owns every Vulkan submit, including semaphore waits.
struct CudaInteropBoundary {
    cudaStream_t stream_to_run{};

    CudaInteropBoundary(RasterContext& context, std::initializer_list<TextureRef> resources) :
        CudaInteropBoundary(context.device, resources) {}

    CudaInteropBoundary(RenderDevice& device, std::initializer_list<TextureRef> resources) :
        vulkan_device(CudaVulkanTools::getVulkanDevice(device)),
        before(device.CreateFence()),
        after(device.CreateFence()),
        acquired(device.CreateFence()) {
        std::unordered_set<Texture*> unique;
        for (const auto& texture : resources) {
            if (!texture) {
                throw std::invalid_argument("CUDA interop requires physical textures");
            }
            if (unique.emplace(texture.Get()).second) {
                textures.push_back(texture);
            }
        }
        if (textures.empty()) {
            throw std::invalid_argument("CUDA interop requires a nonempty resource set");
        }
        try {
            ImportTimeline(before, cuda_before);
            ImportTimeline(after, cuda_after);
            Require(cudaStreamCreate(&stream_to_run));
        } catch (...) {
            if (cuda_before) {
                cudaDestroyExternalSemaphore(cuda_before);
            }
            if (cuda_after) {
                cudaDestroyExternalSemaphore(cuda_after);
            }
            throw;
        }
    }

    ~CudaInteropBoundary() {
        // AcquireFromCuda() normally drains the boundary. Also drain on exceptional exit
        // before imported textures or CUDA engine buffers can be destroyed.
        if (stream_to_run) {
            cudaStreamSynchronize(stream_to_run);
        }
        if (cuda_before) {
            cudaDestroyExternalSemaphore(cuda_before);
        }
        if (cuda_after) {
            cudaDestroyExternalSemaphore(cuda_after);
        }
        if (stream_to_run) {
            cudaStreamDestroy(stream_to_run);
        }
    }

    CudaInteropBoundary(const CudaInteropBoundary&)            = delete;
    CudaInteropBoundary& operator=(const CudaInteropBoundary&) = delete;

    void ReleaseToCuda() {
        ++value;
        CommandList commands(EQueueType::Graphics);
        RecordTransfer(commands, true);
        commands.Signal(before, value);
        RHIExecutor::Get().Submit(EQueueType::Graphics, commands.Submit(), ERHIExecSubmitFlags::FlushGPU);
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (!before->WaitSubmitted(value)) {
            throw std::runtime_error("CUDA release submission was rejected");
        }
        cudaExternalSemaphoreWaitParams wait{};
        wait.params.fence.value = value;
        Require(
            cudaWaitExternalSemaphoresAsync(&cuda_before, &wait, 1, stream_to_run), "wait for Vulkan release"
        );
    }

    void AcquireFromCuda() {
        cudaExternalSemaphoreSignalParams signal{};
        signal.params.fence.value = value;
        Require(
            cudaSignalExternalSemaphoresAsync(&cuda_after, &signal, 1, stream_to_run), "signal Vulkan acquire"
        );
        // A failed CUDA operation must not publish a Vulkan wait on a signal
        // that will never execute. This also bounds reuse of TensorRT buffers.
        Require(cudaStreamSynchronize(stream_to_run), "complete CUDA work before Vulkan acquire");
        auto& external_timeline = CudaVulkanTools::getVulkanTimeline(after);
        external_timeline.MarkSubmitted(value);
        external_timeline.RecordGpuCompletion(value);

        CommandList commands(EQueueType::Graphics);
        RecordTransfer(commands, false);
        commands.Signal(acquired, value);
        RHIExecutor::Get().Submit(
            EQueueType::Graphics, commands.Submit().Wait(after.Get(), value), ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
        if (!acquired->WaitSubmitted(value) || !acquired->Wait(value)) {
            throw std::runtime_error("CUDA acquire submission did not complete");
        }
    }

private:
    VulkanDevice&           vulkan_device;
    FenceRef                before;
    FenceRef                after;
    FenceRef                acquired;
    std::vector<TextureRef> textures;
    cudaExternalSemaphore_t cuda_before{};
    cudaExternalSemaphore_t cuda_after{};
    uint64                  value{};

    static void Require(cudaError_t result, const char* operation = "initialize CUDA interop") {
        if (result != cudaSuccess) {
            throw std::runtime_error(std::string(operation) + ": " + cudaGetErrorString(result));
        }
    }

    void ImportTimeline(const FenceRef& fence, cudaExternalSemaphore_t& imported) {
        const auto get_handle = reinterpret_cast<PFN_vkGetSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(vulkan_device.GetDevice(), "vkGetSemaphoreWin32HandleKHR")
        );
        if (!get_handle) {
            throw std::runtime_error("Vulkan external timeline export is unavailable");
        }
        VkSemaphoreGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        info.semaphore  = CudaVulkanTools::getVulkanTimeline(fence).GetSemaphore();
        info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        HANDLE handle{};
        if (get_handle(vulkan_device.GetDevice(), &info, &handle) != VK_SUCCESS) {
            throw std::runtime_error("Vulkan timeline export failed");
        }
        cudaExternalSemaphoreHandleDesc desc{};
        desc.type                = cudaExternalSemaphoreHandleTypeTimelineSemaphoreWin32;
        desc.handle.win32.handle = handle;
        const auto result        = cudaImportExternalSemaphore(&imported, &desc);
        CudaVulkanTools::CloseWin32Handle(handle);
        Require(result);
    }

    void RecordTransfer(CommandList& commands, bool release) const {
        commands.SetResourceStateOwnership(ERHIResourceStateOwnership::Compiled);
        const auto sampled = BarrierState::Texture(
            ERHIPipelineStageFlags::PS_ALL_COMMANDS,
            // Export all preceding Vulkan writes, including writes made
            // available by the graph's transition back to Sampled.
            release ? ERHIAccessFlags::MEMORY_READ | ERHIAccessFlags::MEMORY_WRITE :
                      ERHIAccessFlags::SHADER_READ,
            ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
        const auto external = BarrierState::Texture(
            ERHIPipelineStageFlags::PS_ALL_COMMANDS,
            ERHIAccessFlags::MEMORY_READ | ERHIAccessFlags::MEMORY_WRITE,
            ETextureLayout::TEXTURE_LAYOUT_COMMON
        );
        std::vector<BarrierCreateInfo> barriers;
        for (const auto& texture : textures) {
            TextureView view(texture);
            view.num_mips  = texture->GetNumMips();
            view.num_array = texture->GetNumArray();
            auto barrier   = BarrierCreateInfo::Transition(
                view, release ? sampled : external, release ? external : sampled, texture->GetAspectFlags()
            );
            barrier.queue_transfer = release ?
                                         BarrierQueueTransfer::ReleaseToExternal(EQueueType::Graphics) :
                                         BarrierQueueTransfer::AcquireFromExternal(EQueueType::Graphics);
            barriers.push_back(barrier);
        }
        commands.Barriers(barriers);
        commands.AddCallback([owners = textures, incoming = before, outgoing = after] {});
    }
};

#undef checkCudaErrors

} // namespace Moer::Render::Raster
