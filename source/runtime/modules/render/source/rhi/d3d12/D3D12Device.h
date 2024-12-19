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

#define COM_NO_WINDOWS_H
#include <wrl/client.h>
#include <d3d12.h>
#include <d3dx12/d3dx12.h>

#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <D3D12MemAlloc.h>
#include <optional>
#include "Variant.h"

template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;// maybe use custom comptr?

namespace Moer::Render {

    inline std::wstring StringWiden(std::string_view utf8str) {
        if (utf8str.empty())
            return std::wstring();
        int          cbMultiByte = static_cast<int>(utf8str.size());
        int          req         = ::MultiByteToWideChar(CP_UTF8, 0, utf8str.data(), cbMultiByte, nullptr, 0);
        std::wstring res(req, 0);
        ::MultiByteToWideChar(CP_UTF8, 0, utf8str.data(), cbMultiByte, &res[0], req);
        return res;
    }

    inline std::string StringNarrow(std::wstring_view wstr) {
        if (wstr.empty())
            return std::string();
        int         cbMultiByte = static_cast<int>(wstr.size());
        int         req         = ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), cbMultiByte, nullptr, 0, nullptr, nullptr);
        std::string res(req, 0);
        ::WideCharToMultiByte(CP_UTF8, 0, wstr.data(), cbMultiByte, &res[0], req, nullptr, nullptr);
        return res;
    }

    inline uint64_t AlignUpToPowerOfTwo(uint64_t value, uint64_t alignment) {
        const uint64_t mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    inline uint64_t AlignConstantBuffer(uint64_t value) {
        return AlignUpToPowerOfTwo(value, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    }
    inline uint64_t AlignResourceByDefaultPlacement(uint64_t value) {
        return AlignUpToPowerOfTwo(value, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
    }
    inline uint64_t AlignTextureResource(uint64_t value, bool msaa = false, bool _small = false) {
        auto alignment = 0;
        if (msaa) {
            if (_small) {
                alignment = D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
            } else {
                alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
            }
        } else {
            if (_small) {
                alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
            } else {
                alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
            }
        }
        return AlignUpToPowerOfTwo(value, alignment);
    }
    //#define D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT (512) // useful when copy
    //#define D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (512) // useful when copy
    //#define D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT (4096)  // not want to use uavcounter...

    //#define D3D12_RAYTRACING_AABB_BYTE_ALIGNMENT (8)
    //#define D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT (256)
    //#define D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT (16)
    //#define D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT (32)
    //#define D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT (64)
    //#define D3D12_RAYTRACING_TRANSFORM3X4_BYTE_ALIGNMENT (16)

    class D3D12Device;
    class D3D12GpuGlobalAllocator;
    class D3D12GraphicsCommandQueue;
    class D3D12Fence;
    class D3D12DescriptorHeap;// ?
    class D3D12Texture;
    class D3D12Buffer;
    class D3D12CommandResourceAllocator;

    // only to provide a 'device' as member
    class D3D12DeviceChild {
    public:
        D3D12DeviceChild(D3D12Device* _device) : device(_device) {}

    protected:
        D3D12Device* device;
        // note here no defer release, compare to VulkanDeviceObject
        // seems not very necessary because of heavily using countableref
        // if need, we can do it explicitly
    };

    struct Allocation {
        D3D12MA::Allocation* alloc    = nullptr;
        ID3D12Resource*      resource = nullptr;

        Allocation()                             = default;
        Allocation(const Allocation&)            = delete;
        Allocation& operator=(const Allocation&) = delete;
        Allocation(Allocation&& other) noexcept : alloc(std::exchange(other.alloc, nullptr)), resource(std::exchange(other.resource, nullptr)) {}
        Allocation& operator=(Allocation&& other) noexcept {
            alloc    = std::exchange(other.alloc, nullptr);
            resource = std::exchange(other.resource, nullptr);
            return *this;
        }
        ~Allocation() {
            if (resource) {
                resource->Release();
            }
            if (alloc) {
                alloc->Release();
            }
            // order matters
        }
    };

    class D3D12GpuGlobalAllocator {
    private:
        ComPtr<D3D12MA::Allocator> d3d12Allocator;
        D3D12Device*               device;

    public:
        D3D12GpuGlobalAllocator(D3D12Device* _device);

        Allocation AllocateBufferHeap(std::string_view _name,// name?
                                      uint64           _byte_size,
                                      uint64           _alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                                      D3D12_HEAP_TYPE  _heap_type = D3D12_HEAP_TYPE_DEFAULT);
        // todo texture heap
    };


    class D3D12Texture final : public Texture, public D3D12DeviceChild {
    private:
        Allocation          allocation;
    };

    // not for readback/upload
    class D3D12Buffer final : public Buffer, public D3D12DeviceChild {
    private:
        Allocation allocation;// ? resource location ?

    public:
        D3D12Buffer(D3D12Device* _device, const BufferInfo& _info);
        D3D12Buffer(D3D12Device* _device, const BufferInfo& _info, Allocation&& _allocation);

        ~D3D12Buffer();

        ID3D12Resource*           Native() const { return allocation.resource; }
        D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddress() const { return allocation.resource->GetGPUVirtualAddress(); }

        void            Destroy() override;                            // from Buffer.RHIResource, mainly called by CountableRef
        RENDER_API void SetName(const std::string_view _name) override;// from Buffer
    };

    class D3D12Fence final : public Fence, public D3D12DeviceChild {
    private:
        ComPtr<ID3D12Fence> fence;
        HANDLE              event;
        std::atomic<uint64> current_value = 0;

    public:
        D3D12Fence(D3D12Device* _device);
        ~D3D12Fence();
        D3D12Fence(const D3D12Fence&) = delete;

        ID3D12Fence* Native() const { return fence.Get(); }
        uint64_t     GetValue() const override;// by default, get latest value, not cached 'current_value'
        void         Wait(uint64_t _value) override;

        bool IsFenceComplete(uint64 _value);
        void WaitOnHost(uint64 _value);
        void SignalOnHost(uint64 _value);
    };

    struct D3D12StagingBufferView {
        D3D12Buffer*              buffer;
        uint64                    byte_offset;
        D3D12_GPU_VIRTUAL_ADDRESS gpu_base_address_remapped;// resource->GetGPUVirtualAddress() + byte_offset
        uint8*                    cpu_base_address_remapped;
    };

    class D3D12CommandList {
    private:
        D3D12CommandResourceAllocator&     allocator;
        ComPtr<ID3D12GraphicsCommandList7> list;// 7 for enhanced barrier

    public:
        D3D12CommandList(D3D12CommandResourceAllocator&);
        D3D12CommandList(const D3D12CommandList&) = delete;
        D3D12CommandList(D3D12CommandList&&)      = default;

        ID3D12GraphicsCommandList7* Native() const { return list.Get(); }
        void                        Begin();
        void                        End();

        void CopyBuffer(D3D12Buffer* _src, D3D12Buffer* _dst, uint64 _size, uint64 _src_offset, uint64 _dst_offset);
        void CopyData(D3D12StagingBufferView _dst, const void* _data, uint64 _size);
        void CopyData(void* _dst, D3D12StagingBufferView _src, uint64 _size);
    };

    class D3D12ResourceStateTracker {

    };

    //class D3D12ResourceStateTracker {
    //private:
    //    //std::vector<D3D12_GLOBAL_BARRIER> globalBarriers;
    //    //std::vector<D3D12_TEXTURE_BARRIER> textureBarriers;
    //    Array<D3D12_BUFFER_BARRIER> bufferBarriers;

    //    struct TextureSubresourceState {
    //        TextureStateDescription before;
    //        TextureStateDescription after;
    //    };
    //    struct TextureState {// ref nvrhi state tracking
    //        // all
    //        TextureStateDescription before;
    //        TextureStateDescription after;
    //        // sub
    //        std::vector<TextureSubresourceState> subresources;

    //        TextureStateDescription initial;
    //    };
    //    struct BufferState {
    //        BufferStateDescription before;
    //        BufferStateDescription after;

    //        BufferStateDescription initial;
    //    };

    //    std::map<GpuTexture*, TextureState> textureStates;
    //    std::map<GpuBuffer*, BufferState>   bufferStates;
    //    std::set<GpuTexture*>               pendingTextures;  // textures need to be transitioned in this layer
    //    std::set<GpuTexture*>               subdivideTextures;// need to consider subresource state explicitly from now on
    //    std::set<GpuBuffer*>                pendingBuffers;   // buffers need to be transitioned in this layer

    //public:
    //    // preprocess
    //    void StartState(GpuTexture* tex, TextureSubresourceSet subresourceSet, EResourceAccessType accessType);
    //    void RecordState(GpuTexture* tex, TextureSubresourceSet subresourceSet, EResourceAccessType accessType);
    //    void StartState(GpuBuffer* buffer, EResourceAccessType accessType);
    //    void RecordState(GpuBuffer* buffer, EResourceAccessType accessType);

    //    // before pass execute, generate and dispatch barriers
    //    // (also flush state 'after' to 'before' in ResolveBarriers)
    //    void ResolveBarriers();
    //    void DispatchBarriers(ID3D12GraphicsCommandList7* list);

    //    // after whole execute , restore state to initial ?
    //    void RestoreState();

    //    void Reset();
    //};

    // only for upload/readback buffer now
    class D3D12BuddyAllocator : public D3D12DeviceChild {
    public:
        friend struct BuddyBlock;

        // not consider mem used for multi-frame
        struct BuddyBlock {
            D3D12BuddyAllocator* parent             = nullptr;
            uint32               block_offset       = 0;
            uint32               block_order        = 0;
            uint32               memory_byte_offset = 0;// offset start from the underlying buffer
            uint32               required_size      = 0;

            bool IsValid() const {
                return parent != nullptr;// not accurate but enough
            }

            D3D12Buffer* GetUnderlyingBuffer() const {
                return parent->underlying_buffer.get();
            }

            uint8* GetCpuAddressRemapped() const {
                return parent->ptr_mapped + memory_byte_offset;
            }

            D3D12_GPU_VIRTUAL_ADDRESS GetGpuVirtualAddressRemapped() const {
                return parent->underlying_buffer->GetGpuVirtualAddress() + memory_byte_offset;
            }

            // current target usage dont need this, instead, it became a problem
            //~BuddyBlock() {
            //    if (parent) {
            //        parent->Deallocate(*this);
            //    }
            //}
        };

        D3D12BuddyAllocator(D3D12Device* _device, D3D12_HEAP_TYPE _heap_type, uint32 _total_byte_size, uint32 _min_block_byte_size);
        D3D12BuddyAllocator(const D3D12BuddyAllocator&) = delete;
        D3D12BuddyAllocator(D3D12BuddyAllocator&&)      = default;
        ~D3D12BuddyAllocator();

        BuddyBlock Allocate(uint32 _size, uint32 _alignment);

        void Deallocate(const BuddyBlock& _block);

        void CleanUpAllocations();

    private:
        void Initialize();

        // useful if min_block_size % alignment != 0
        uint32 GetSizeToAllocate(uint32 _size, uint32 _alignment);

        bool CanAllocate(uint32 required_size);

        uint32 SizeToBlockCount(uint32 _size) const {
            return (_size + (min_block_byte_size - 1)) / min_block_byte_size;
        }
        static uint32 BlockCountToOrder(uint32 _size) {
            unsigned long Result;
            _BitScanReverse(&Result, _size + _size - 1);// ceil(log2(size))
            return Result;
        }
        static uint32 OrderToBlockCount(uint32 _order) {
            return ((uint32)1) << _order;
        }

        BuddyBlock AllocateInternal(uint32 _size, uint32 _alignment);

        // return block id/offset, i.e. block serial number
        uint32 AllocateBlock(uint32 _order);

        struct RetiredBlock {
            uint32 offset;
            uint32 order;
        };
        void DeallocateInternal(RetiredBlock _block);

        void DeallocateBlock(uint32 _offset, uint32 _order);

    private:
        const uint32 total_byte_size;
        const uint32 min_block_byte_size;

        const uint32 max_order;
        uint32       total_used_byte_size;

        Array<Set<uint32>>  free_blocks;
        Array<RetiredBlock> deferred_deallocate_blocks;

        const D3D12_HEAP_TYPE  heap_type;
        UniquePtr<D3D12Buffer> underlying_buffer;
        uint8*                 ptr_mapped;
    };

    class D3D12MultiBuddyAllocator : public D3D12DeviceChild {
    private:
        Array<D3D12BuddyAllocator> allocators;
        const uint32               each_total_byte_size;
        const uint32               min_block_byte_size;
        const D3D12_HEAP_TYPE      heap_type;

    public:
        D3D12MultiBuddyAllocator(D3D12Device* _device, D3D12_HEAP_TYPE _heap_type, uint32 _each_total_byte_size, uint32 _min_block_byte_size);

        D3D12BuddyAllocator::BuddyBlock Allocate(uint32 _size, uint32 _alignment);

        void CleanUpAllocations();
    };

    // avoid buddyblock dtor...// ok it is weired
    class D3D12MultiBuddyAllocatorAutoFree : public D3D12DeviceChild {
    private:
        D3D12MultiBuddyAllocator                      allocator;
        Array<Array<D3D12BuddyAllocator::BuddyBlock>> allocated_blocks;

    public:
        D3D12MultiBuddyAllocatorAutoFree(D3D12Device* _device, D3D12_HEAP_TYPE _heap_type, uint32 _each_total_byte_size, uint32 _min_block_byte_size);

        D3D12StagingBufferView Allocate(uint32 _size, uint32 _alignment);

        void CleanUpAllocations();
    };

    // specialize for constant buffer
    class D3D12FastConstantAllocator : public D3D12DeviceChild {
    private:
        D3D12MultiBuddyAllocatorAutoFree allocator;
        const uint32                     page_size;
        uint32                           offset;
        D3D12StagingBufferView           underlying_current;

    public:
        D3D12FastConstantAllocator(D3D12Device* _device, uint32 _total_byte_size, uint32 _min_block_byte_size);

        D3D12StagingBufferView Allocate(uint32 _size);

        void CleanUpAllocations();
    };

    // resource & cmd allocator
    class D3D12CommandResourceAllocator : public D3D12DeviceChild {
    private:
        friend class D3D12CommandList;
        D3D12GraphicsCommandQueue&     queue;
        ComPtr<ID3D12CommandAllocator> cmd_allocator;
        UniquePtr<D3D12CommandList>    cmd_list;// follow the pattern allocator:cmdlist = 1:1; ptr is to delay the ctor

        Array<std::function<void()>> on_complete_callbacks;
        D3D12ResourceStateTracker    tracker;

        static constexpr uint32          larget_buffer_byte_size_threshold = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        Array<UniquePtr<D3D12Buffer>>    large_buffers;// only for upload/readback
        D3D12MultiBuddyAllocatorAutoFree upload_allocator;
        D3D12MultiBuddyAllocatorAutoFree readback_allocator;
        D3D12FastConstantAllocator       constant_allocator;

        // in terms of each buffer may need different state, this can't use manual sub-allocation from a big buffer
        // rather we prefer sub-allocation from a heap, which simply rely on D3D12MA
        // note we don't have 'explicit' cross-frame cache for this, unlike upload/readback
        struct HeapPlacedBufferAllocator {
            D3D12CommandResourceAllocator& parent;

            Array<UniquePtr<D3D12Buffer>> allocated;// can't simply use array<buffer> because of atomic inside

            D3D12Buffer* Allocate(uint64 _size, EBufferUsageFlags _flag);
            void         Reset();
        };
        HeapPlacedBufferAllocator default_buffer_allocator;
        HeapPlacedBufferAllocator scratch_buffer_allocator;

    public:
        D3D12CommandResourceAllocator(D3D12GraphicsCommandQueue& queue);
        D3D12CommandResourceAllocator(const D3D12CommandResourceAllocator&) = delete;
        D3D12CommandResourceAllocator(D3D12CommandResourceAllocator&&)      = default;

        ID3D12CommandAllocator*    Native() const { return cmd_allocator.Get(); }
        D3D12Device*               GetDevice() const { return device; }
        D3D12CommandList*          GetCommandList() { return cmd_list.get(); }
        D3D12ResourceStateTracker& GetResourceStateTracker() { return tracker; }

    public:
        void Reset();
        void AddOnComplete(std::function<void()>&& _func) {
            on_complete_callbacks.push_back(std::move(_func));
        }
        void OnComplete() {
            for (auto&& cb : on_complete_callbacks) {
                cb();
            }
            on_complete_callbacks.clear();
        }

        D3D12StagingBufferView AllocateConstantBuffer(uint32 _size);
        D3D12StagingBufferView AllocateUploadBuffer(uint64 _size, uint32 _alignment);
        D3D12StagingBufferView AllocateReadbackBuffer(uint64 _size, uint32 _alignment);
        D3D12Buffer*           AllocateScratch(uint64 _size);
        D3D12Buffer*           AllocateDefaultBuffer(uint64 _size);// no special usage(uav), mainly for intermediate uploaded/staging resource vb,ib
    };

    class D3D12GraphicsCommandQueue final : public CommandQueue, public D3D12DeviceChild {
    private:
        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_LIST_TYPE    queue_type;
        D3D12Fence                 queue_fence;
        std::atomic<uint64>        last_submitted_fence_value;
        std::atomic<uint64>        last_completed_fence_value;
        // ideally, 'last_completed_fence_value' should be same as queue_fence.current_value.
        // however consider case recycle allocator, we first wait gpu signal queue_fence to X, then reset allocator, then update lastxxx to X
        // this make a difference. so 'last_completed_fence_value' means a bit more than gpufence in terms of cpu time
        CircularQueue<uint64, 3> pending_fence_values;// to limit the number of cmdallocators / on-the-fly frames

        LockFreeQueueBase<D3D12CommandResourceAllocator, false> ready_allocators;
        // ptr size not satisfy the 'inline version' requirement,
        // this internally only hold D3D12CommandResourceAllocator* (not push uniqueptr into

        using EventType = Variant<
            UniquePtr<D3D12CommandResourceAllocator>,
            Array<std::function<void()>>,
            SignalEvent,
            WaitEvent>;

        struct QueueEvent {
            EventType event;
            uint64    timeline;
            bool      wake_thread;
            template<typename Arg>
                requires std::is_constructible_v<EventType, Arg&&>
            QueueEvent(Arg&& _event, uint64 _timeline, bool _wake_thread) : event(std::forward<Arg>(_event)), timeline(_timeline), wake_thread(_wake_thread) {
            }

            QueueEvent(QueueEvent&& _other) noexcept : event(std::move(_other.event)), timeline(_other.timeline), wake_thread(_other.wake_thread) {
            }
        };
        DEQueue<QueueEvent>         event_queue;
        std::mutex                  mtx;
        std::condition_variable_any cv;

        std::atomic<bool> is_second_thread_busy = false;// to wait ExecuteThread() to complete when Sync()
        std::jthread      thd;                          // dtor first

    private:
        void ExecuteThread(std::stop_token _st);

    public:
        ID3D12CommandQueue*                      Native() const { return queue.Get(); }
        D3D12Device*                             GetDevice() const { return device; }
        D3D12_COMMAND_LIST_TYPE                  GetType() const { return queue_type; }
        UniquePtr<D3D12CommandResourceAllocator> RequestCommandResourceAllocator();

        void Complete(uint64 fence_value);

    public:
        D3D12GraphicsCommandQueue(D3D12Device* _device, EQueueType _type);
        ~D3D12GraphicsCommandQueue();

        // queue.signal is not exposed, exist in cmdsubmit.signalevent
        void      Wait(WaitEvent _event) override;
        WaitEvent Execute(CmdSubmit&& _submit) override;
        void      Present(SwapchainRef _swapchain, TextureView _target) override;
        void      Sync() override;
    };

    class D3D12Swapchain final : public Swapchain {
    public:
        D3D12Swapchain(D3D12Device& device, const SwapchainCreateInfo& info);
        ~D3D12Swapchain() = default;

        void Recreate(const SwapchainCreateInfo&) override;

        uint GetBackbufferIndex() const { return frame_index; }
        //DescriptorIndex GetBackbufferRTV() const { return m_backbufferRTVs[m_frameIndex]; }
        //GpuTexture*     GetBackbuffer() const { return m_backbufferTextures[m_frameIndex].get(); }

        uint32_t GetWidth() const { return width; }
        uint32_t GetHeight() const { return height; }

        void Present();

    private:
        void CreateSizeDependentResources();

    private:
        D3D12Device&                  device;
        uintptr_t                     window_handle;
        ComPtr<IDXGISwapChain3>       swapchain;
        Array<ComPtr<ID3D12Resource>> backbuffers;
        uint                          width;
        uint                          height;
        uint                          frame_index;
        /*     DescriptorIndex             m_backbufferRTVs[FrameLatency];
        std::unique_ptr<GpuTexture> m_backbufferTextures[FrameLatency];*/
    };

    //class D3D12DescriptorSetsLayout;
    //class D3D12DescriptorSetAllocator;
    //class D3D12DescriptorSetWriter;

    struct D3D12RHIConfig {
        bool force_sync = true;// true if want to wait for ExecuteThread() in CommandQueue::Sync()
        //bool want_capture = false; // put here because we have to initialize capturer before the device is created
        //uint32 api_version = VK_API_VERSION_1_3; // feature level?
    };

    class D3D12Device final : public RenderDevice::Impl {
        friend class D3D12GraphicsCommandQueue;

    public:
        D3D12Device(const D3D12RHIConfig&& _config);
        ~D3D12Device();

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
        //void           EnqueueDeferredRelease(RHIResource* _object);
        //void           FlushDeferredReleases();
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
        D3D12RHIConfig config;

        ComPtr<IDXGIInfoQueue> dxgi_info_queue;
        //ComPtr<IDXGIDebug>        dxgi_debug;
        //ComPtr<ID3D12DebugDevice> debug_device;
        ComPtr<ID3D12Debug>      debug_interface;
        ComPtr<ID3D12InfoQueue>  debug_info_queue;// can remove this once we find out how to create queue1
        ComPtr<ID3D12InfoQueue1> debug_info_queue1;
        DWORD                    debug_log_callback_cookie;

        ComPtr<ID3D12Device10>             device;// use 10 for enhanced barrier
        ComPtr<IDXGIFactory6>              factory;
        ComPtr<IDXGIAdapter3>              adapter;
        UniquePtr<D3D12GpuGlobalAllocator> gpu_global_allocator;// ptr to construct after device

        //D3D12DescriptorHeap                            m_global_descriptor_heap{};
        UniquePtr<D3D12GraphicsCommandQueue> gfx_queue{};
        //UniquePtr<D3D12CommandQueue>              compute_queue{};
        //UniquePtr<D3D12CopyQueue>                          copy_queue{};
        //LockFreeQueueBase<RHIResource, false, 64> deferred_release_queue{};
        static constexpr uint immutable_sampler_count = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
        //StaticArray<VkSampler, immutable_sampler_count> immutable_samplers{};

    public:
        ID3D12Device10*          Native() const { return device.Get(); }
        IDXGIFactory6*           NativeFactory() const { return factory.Get(); }
        IDXGIAdapter3*           NativeAdapter() const { return adapter.Get(); }
        D3D12GpuGlobalAllocator* GetGpuGlobalAllocator() { return gpu_global_allocator.get(); }

    public:
        static constexpr uint bindless_sampler_cnt = 256;
        static constexpr uint cmd_alloc_limits     = 3;
        //UniquePtr<DeviceInternalShaders> internal_shaders;

    private:
        //friend VkCommandQueue;
    };

}// namespace Moer::Render

#endif// D3D12_DEVICE_H