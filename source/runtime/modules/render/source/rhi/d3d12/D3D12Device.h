#ifndef D3D12_DEVICE_H
#define D3D12_DEVICE_H

#include "taskgraph/Event.h"
#include "misc/STL.h"

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "../RHIImpl.h"

#include "D3D12Macro.h"
#include <wrl/client.h>
#include <d3d12.h>
#include <d3dx12/d3dx12.h>

#include <dxgi1_6.h>
#include <D3D12MemAlloc.h>
#include <optional>

template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>; // maybe use custom comptr?

namespace Moer::Render {

    //class D3D12DescriptorSetsLayout;
    //class D3D12DescriptorSetAllocator;
    //class D3D12DescriptorSetWriter;

    struct D3D12RHIConfig {
        //bool want_capture = false; // put here because we have to initialize capturer before the device is created
        //uint32 api_version = VK_API_VERSION_1_3; // feature level?
    };

    class D3D12Device final : public RenderDevice::Impl {
    public:
        D3D12Device(const D3D12RHIConfig&& _config);
        ~D3D12Device() = default;

        // not implemented yet
        PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) override;
        PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders) override;

        TextureRef CreateTexture(std::string_view _name, ETextureDimension _dimension, Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint _array_size) override;

        BufferRef CreateBuffer(uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage) override;

        BindlessArrayRef CreateBindlessArray(uint _max_size) override;
        FenceRef         CreateFence() override;

        // Raytracing
        RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) override;

        RaytracingSceneRef CreateRaytracingScene() override;

        CommandQueue& GetCommandQueue(EQueueType _type) override;

        CopyQueue& GetCopyQueue() override;

        SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) override;

    private:
        void PostInit() override;

        void GetHardwareAdapter(
            ComPtr<IDXGIFactory4> pFactory,
            IDXGIAdapter1**       ppAdapter,
            bool                  requestHighPerformanceAdapter);

    public:
        void           EnqueueDeferredRelease(RHIResource* _object);
        void           FlushDeferredReleases();
        constexpr uint ImmutableSamplerCount() const { return immutable_sampler_count; }
        //const VkSampler* GetImmutableSamplers() const { return immutable_samplers.data(); }
        //const VkSampler  GetSampler(Sampler _sampler) const;
        //const uint       GetSamplerIdx(Sampler _sampler) const {
        //    uint filter  = uint(_sampler.filter);
        //    uint address = uint(_sampler.address_mode);
        //    uint compare = uint(_sampler.compare_function);
        //    return (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
        //}

        //void SetResourceName(uint64 _object, VkObjectType _object_type, const std::string_view _name);

    private:
        // device, factory, adapter
        ComPtr<ID3D12Device> device;
        ComPtr<IDXGIFactory6> factory;
        ComPtr<IDXGIAdapter3>  adapter;
        ComPtr<D3D12MA::Allocator> d3d12Allocator;

        //VmaAllocator                                    m_allocator = VK_NULL_HANDLE;
        //D3D12DescriptorHeap                            m_global_descriptor_heap{};
        //UniquePtr<VkCommandQueue>                       gfx_queue{};
        //UniquePtr<VkCommandQueue>                       compute_queue{};
        //UniquePtr<VkCopyQueue>                          copy_queue{};
        LockFreeQueueBase<RHIResource, false, 64> deferred_release_queue{};
        static constexpr uint                     immutable_sampler_count = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
        //StaticArray<VkSampler, immutable_sampler_count> immutable_samplers{};

    public:
        static constexpr uint            bindless_sampler_cnt = 256;
        static constexpr uint            cmd_alloc_limits     = 3;
        //UniquePtr<DeviceInternalShaders> internal_shaders;

    private:
        //friend VkCommandQueue;
    };

}// namespace Moer::Render

#endif// D3D12_DEVICE_H