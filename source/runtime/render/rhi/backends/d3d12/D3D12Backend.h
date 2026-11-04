#pragma once

#include "D3D12Common.h"
#include "rhi/RHIImpl.h"
#include "rhi/RHIResourceInitilizer.h"
#include "rhi/RHIThreadOwnership.h"
#include <D3D12MemAlloc.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace Moer::Render::D3D12 {

DXGI_FORMAT           Format(EPixelFormat);
DXGI_FORMAT           VertexFormat(EVertexFormat);
DXGI_FORMAT           ResourceFormat(EPixelFormat);
DXGI_FORMAT           SampleFormat(EPixelFormat);
DXGI_FORMAT           StorageFormat(EPixelFormat);
D3D12_RESOURCE_STATES AccessState(ERHIAccessFlags, ETextureLayout = TEXTURE_LAYOUT_UNDEFINED);
D3D12_RESOURCE_STATES BufferState(EBufferState, bool write);
D3D12_RESOURCE_STATES TextureState(ETextureState, bool write);

class DescriptorHeap {
public:
    DescriptorHeap(ID3D12Device*, D3D12_DESCRIPTOR_HEAP_TYPE, uint32 capacity, bool visible);
    uint32                      Allocate(uint32 count = 1);
    void                        Free(uint32 index, uint32 count = 1);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(uint32 index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(uint32 index) const;
    ID3D12DescriptorHeap*       Native() const {
        return heap.Get();
    }

private:
    ComPtr<ID3D12DescriptorHeap> heap;
    uint32                       stride;
    std::mutex                   mutex;
    std::map<uint32, uint32>     free_ranges;
};

struct Context;

// The submission mutex owns all resource-state changes. A resource's state is
// its last ordered native use, including work still executing on the GPU.
class GpuBuffer final : public Buffer {
public:
    GpuBuffer(Context&, const BufferInfo&, D3D12_HEAP_TYPE = D3D12_HEAP_TYPE_DEFAULT);
    void SetName(std::string_view) override;
    bool SupportsOwningReadback() const noexcept override {
        return true;
    }
    ComPtr<D3D12MA::Allocation> allocation;
    ComPtr<ID3D12Resource>      resource;
    D3D12_RESOURCE_STATES       state;
    D3D12_HEAP_TYPE             heap_type;
    D3D12_GPU_VIRTUAL_ADDRESS   Address(uint64 offset = 0) const {
        return resource->GetGPUVirtualAddress() + offset;
    }
};

class GpuTexture final : public Texture {
public:
    GpuTexture(Context&, const TextureInfo&);
    void   SetName(std::string_view) override;
    uint32 GetMipByteSize(uint32) const override;
    bool   SupportsOwningReadback() const noexcept override {
        return GetNumSamples() == 1;
    }
    uint32                       Subresource(uint32 mip, uint32 layer = 0, uint32 plane = 0) const;
    ComPtr<D3D12MA::Allocation>  allocation;
    ComPtr<ID3D12Resource>       resource;
    Array<D3D12_RESOURCE_STATES> states;
};

class Timeline final : public Fence {
public:
    explicit Timeline(ID3D12Device*);
    uint64 GetValue() const override;
    bool   Wait(uint64) override;
    void   MarkSubmitted(uint64) override;
    bool   WaitSubmitted(uint64, const std::atomic_bool*) override;
    void   Reject(uint64) noexcept override;
    bool   IsRejected(uint64) const override;
    bool   IsSubmitted(uint64 value) const {
        std::lock_guard lock(mutex);
        return submitted >= value && !rejected.contains(value);
    }
    ComPtr<ID3D12Fence> fence;

private:
    mutable std::mutex      mutex;
    std::condition_variable cv;
    uint64                  submitted = 0;
    std::set<uint64>        rejected;
};

struct Binding {
    EPixelFormat             inferred_format  = PF_UNDEFINED;
    uint32                   argument         = UINT32_MAX;
    uint32                   root             = 0;
    uint32                   count            = 1;
    uint32                   structure_stride = 0;
    ED3D12ShaderVariableType type{};
    bool                     constants     = false;
    uint32                   constant_size = 0;
    struct ConstantCopy {
        uint32 source;
        uint32 destination;
        uint32 size;
    };
    Array<ConstantCopy> constant_copies;
};

class Pipeline final : public PipelineState {
public:
    explicit Pipeline(uint64 active) :
        PipelineState(active) {}
    ComPtr<ID3D12PipelineState> state;
    ComPtr<ID3D12RootSignature> signature;
    Array<Binding>              bindings;
    Array<uint32>               vertex_strides;
    D3D_PRIMITIVE_TOPOLOGY      topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    bool                        compute  = false;
    bool                        mesh     = false;
};

struct ConstantLayout {
    uint32                       packed_size = 0;
    uint32                       native_size = 0;
    Array<Binding::ConstantCopy> copies;
};
ConstantLayout              ReflectConstantLayout(ID3D12ShaderReflectionType*);
ConstantLayout              ReflectBufferLayout(ID3D12ShaderReflectionConstantBuffer*);
ID3D12ShaderReflectionType* FindConstantType(ID3D12ShaderReflection*, std::string_view);

PipelineHandle CreatePipeline(Context&, PipelineShaderInfo&&, const GfxPsoCreateInfo*);

// Membership is changed only by ordered update commands. Every bind snapshots
// its table into submission-owned upload memory, so later updates cannot edit
// descriptors or index data used by an older draw.
class Bindless final : public BindlessArray {
public:
    Bindless(Context&, uint32);
    uint32 AllocateTexture(const TextureView&, Sampler) override;
    uint32 AllocateBuffer(BufferView) override;
    void   UnbindTexture(uint32) override;
    void   UnbindBuffer(uint32) override;
    uint64 ArrayHandle() const override {
        return uint64(this);
    }
    UniquePtr<Command>    CreateUpdateCommand() override;
    void                  DiscardUpdateCommand(const Array<UpdateCmd>&) override;
    std::function<void()> Apply(const UpdateBindlessArrayCmd&);
    struct Entry {
        TextureRef  texture;
        BufferRef   buffer;
        TextureView texture_view{};
        BufferView  buffer_view{};
        Sampler     sampler{SF_LINEAR, SAM_REPEAT};
    };
    Array<Entry> Snapshot();

private:
    Context&         context;
    std::mutex       mutex;
    Array<Entry>     desired;
    Array<Entry>     committed;
    Array<uint32>    free_indices;
    Array<UpdateCmd> updates;
    Array<uint64>    desired_generations;
    Array<uint64>    committed_generations;
    uint32           capacity;
    uint64           generation = 0;
};

class Geometry final : public RaytracingGeometry {
public:
    Geometry(Context&, const RaytracingGeometryInfo&);
    Buffer* GetUnderlyingBuffer() const override {
        return storage.Get();
    }
    Array<D3D12_RAYTRACING_GEOMETRY_DESC>                 descriptions;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS  Inputs() const;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    BufferRef                                             storage;
    bool                                                  built = false;
};

class Tlas final : public RaytracingTlas {
public:
    Buffer* GetUnderlyingBuffer() const override {
        return storage.Get();
    }
    BufferRef storage;
};

class Scene final : public RaytracingScene {
public:
    explicit Scene(Context&);
    RaytracingInstance& AddInstance() override;
    void                FreeInstance(uint32) override;
    void                MarkModified(uint32) override;
    UniquePtr<Command>  UpdateScene() override;
    void                AdvanceFrame() override;
    RaytracingTlasRef   GetTlas() const override {
        return current;
    }
    RaytracingTlasRef GetPrevTlas() const override {
        return previous;
    }

private:
    Context&          context;
    RaytracingTlasRef current;
    RaytracingTlasRef previous;
    Array<uint32>     free_instances;
};

class Queue;
class CopyQueueAdapter;

struct Context {
    ComPtr<ID3D12InfoQueue1>          debug_callbacks;
    DWORD                             debug_callback_cookie = 0;
    ComPtr<IDXGIFactory6>             factory;
    ComPtr<IDXGIAdapter1>             adapter;
    ComPtr<ID3D12Device5>             device;
    ComPtr<D3D12MA::Allocator>        allocator;
    ComPtr<ID3D12InfoQueue>           info_queue;
    std::unique_ptr<DescriptorHeap>   views;
    std::unique_ptr<DescriptorHeap>   cpu_views;
    std::unique_ptr<DescriptorHeap>   samplers;
    std::unique_ptr<DescriptorHeap>   rtvs;
    std::unique_ptr<DescriptorHeap>   dsvs;
    std::unique_ptr<Queue>            queue;
    std::unique_ptr<CopyQueueAdapter> copy;
    bool                              raytracing   = false;
    bool                              mesh_shaders = false;
    bool                              force_sync   = false;
    std::atomic<uint32>               validation_errors{0};
    ~Context();
    BufferRef Buffer(uint64 size, EBufferUsageFlags usage = EBufferUsageFlags::NONE, D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT);
    uint32    SamplerIndex(Sampler) const;
};

struct QueryRecord {
    QueryToken token;
    uint32     index;
    bool       ended = false;
};

struct Packet {
    ComPtr<IDXGISwapChain3> presentation;
    PresentReceiptRef       present_receipt;
    explicit Packet(CmdSubmit&& submit) :
        submit(std::move(submit)) {}
    CmdSubmit                                                    submit;
    uint64                                                       timeline = 0;
    ComPtr<ID3D12CommandAllocator>                               allocator;
    ComPtr<ID3D12GraphicsCommandList6>                           list;
    Array<ComPtr<ID3D12CommandSignature>>                        signatures;
    Array<ComPtr<ID3D12Resource>>                                native_resources;
    Array<BufferRef>                                             buffers;
    Array<CountableRef<RHIResource>>                             resources;
    Array<std::pair<DescriptorHeap*, std::pair<uint32, uint32>>> descriptors;
    Array<std::function<void()>>                                 readbacks;
    Array<std::function<void()>>                                 rollback;
    bool                                                         accepted = false;
    std::string                                                  error;
    ComPtr<ID3D12QueryHeap>                                      timestamps;
    ComPtr<ID3D12QueryHeap>                                      occlusion;
    BufferRef                                                    query_readback;
    Array<QueryRecord>                                           queries;
    ~Packet();
};

class SwapchainImpl final : public Swapchain {
public:
    SwapchainImpl(Context&, const SwapchainCreateInfo&);
    bool Recreate(const SwapchainCreateInfo&) override;
    bool IsPresentationReady() const noexcept override {
        return swapchain && size.x && size.y;
    }
    WindowSurfaceIdentity GetCommittedSurfaceIdentity() const noexcept override {
        return surface.GetIdentity();
    }
    Context&                      context;
    SwapchainSurfaceInfo          surface;
    ComPtr<IDXGISwapChain3>       swapchain;
    Array<ComPtr<ID3D12Resource>> backbuffers;
    std::mutex                    mutex;
};

class Queue final : public CommandQueue {
    friend class SwapchainImpl;

public:
    explicit Queue(Context&);
    ~Queue();
    void        Wait(WaitEvent) override;
    WaitEvent   Execute(CmdSubmit&&) override;
    void        Present(SwapchainRef, TextureView, PresentReceiptRef) override;
    void        Sync() override;
    ProfileData GetProfilerEntry() override {
        return {};
    }
    ID3D12CommandQueue* Native() const {
        return queue.Get();
    }
    FenceRef FenceHandle() const {
        return timeline;
    }
    void Sync(uint64);

private:
    void                                Complete(std::stop_token);
    WaitEvent                           SubmitPacket(std::unique_ptr<Packet>);
    std::unique_lock<std::mutex>        LockIdle();
    Context&                            context;
    ComPtr<ID3D12CommandQueue>          queue;
    FenceRef                            timeline;
    uint64                              next_timeline = 0;
    std::mutex                          submission_mutex;
    std::mutex                          completion_mutex;
    std::condition_variable_any         completion_cv;
    std::deque<std::unique_ptr<Packet>> pending;
    uint64                              retired = 0;
    std::jthread                        worker;
};

class CopyQueueAdapter final : public CopyQueue {
public:
    explicit CopyQueueAdapter(Queue& queue) :
        queue(queue) {}
    WaitEvent Execute(CmdSubmit&& submit) override {
        return queue.Execute(std::move(submit));
    }
    void     CopyFrom(BufferView, BufferView) override;
    void     CopyFrom(TextureView, TextureView) override;
    void     CopyFrom(TextureView, BufferView) override;
    void     CopyFrom(BufferView, TextureView) override;
    void     CopyFrom(std::span<byte>, BufferView) override;
    void     CopyFrom(std::span<byte>, TextureView) override;
    FenceRef GetFenceHandle() override {
        return queue.FenceHandle();
    }
    void Sync(uint64 value) override {
        queue.Sync(value);
    }

private:
    Queue& queue;
};

class Recorder {
public:
    Recorder(Context&, Packet&);
    void Record();
    void Transition(GpuBuffer&, D3D12_RESOURCE_STATES);
    void Transition(GpuTexture&, D3D12_RESOURCE_STATES, uint32 mip = 0, uint32 mips = UINT32_MAX, uint32 layer = 0, uint32 layers = UINT32_MAX);

private:
    uint32                                             Descriptor(DescriptorHeap&, uint32 count = 1);
    uint32                                             View(BufferView, ED3D12ShaderVariableType, uint32 index = UINT32_MAX);
    uint32                                             View(TextureView, ED3D12ShaderVariableType, uint32 index = UINT32_MAX);
    BufferRef                                          Upload(std::span<const byte>, uint64 min_size = 0);
    void                                               Bind(PipelineHandle, const ArrayArguments&);
    void                                               RenderPass(const RenderPassInfo&);
    void                                               Mesh(const Pipeline&, const MeshDrawData&);
    void                                               Indirect(D3D12_INDIRECT_ARGUMENT_TYPE, const IndirectDrawParam&);
    void                                               Copy(const Command&);
    void                                               Barrier(const BarrierCmd&);
    void                                               Clear(const ClearResourceCmd&);
    void                                               Build(const BuildAccelerationStructuresCmd&);
    void                                               Build(const UpdateRaytracingSceneCmd&);
    void                                               Query(const QueryCmd&);
    Context&                                           context;
    Packet&                                            packet;
    ID3D12GraphicsCommandList6*                        list;
    UnorderedMap<Bindless*, D3D12_GPU_VIRTUAL_ADDRESS> bindless_tables;
};

} // namespace Moer::Render::D3D12
