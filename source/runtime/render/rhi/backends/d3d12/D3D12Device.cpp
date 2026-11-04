#include "D3D12Device.h"
#include "D3D12Backend.h"
#include <d3d12sdklayers.h>

namespace Moer::Render {
using namespace D3D12;
namespace {
std::atomic<uint32> validation_error_count{0};
}
uint32 GetD3D12ValidationErrorCount() noexcept {
    return validation_error_count.load();
}

struct D3D12Device::Native : Context {};

D3D12Device::D3D12Device(D3D12RHIConfig config) :
    native(std::make_unique<Native>()) {
    auto& ctx      = *native;
    ctx.force_sync = config.force_sync;
    ComPtr<ID3D12Debug> debug;
    if (config.debug && SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
    DX_CHECK_HRESULT(CreateDXGIFactory2(0, IID_PPV_ARGS(&ctx.factory)));
    for (uint32 index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (ctx.factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 description{};
        adapter->GetDesc1(&description);
        if (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&ctx.device)))) {
            ctx.adapter = adapter;
            LOG_INFO("D3D12 adapter selected, vendor={:#x}, device={:#x}", description.VendorId, description.DeviceId);
            break;
        }
    }
    if (!ctx.device) {
        throw std::runtime_error("No D3D12 hardware adapter with feature level 12_0");
    }
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_8};
    DX_CHECK_HRESULT(ctx.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));
    if (model.HighestShaderModel < D3D_SHADER_MODEL_6_8) {
        throw std::runtime_error("Moer D3D12 shaders require Shader Model 6.8 for base vertex/instance indices");
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21{};
    DX_CHECK_HRESULT(ctx.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS21, &options21, sizeof(options21)));
    if (!options21.ExtendedCommandInfoSupported)
        throw std::runtime_error("D3D12 adapter does not support ExtendedCommandInfo");
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    if (SUCCEEDED(ctx.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
        ctx.raytracing = options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7{};
    if (SUCCEEDED(ctx.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &options7, sizeof(options7)))) {
        ctx.mesh_shaders = options7.MeshShaderTier != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3{};
    if (SUCCEEDED(ctx.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options3, sizeof(options3))))
        ctx.view_instancing = options3.ViewInstancingTier != D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
    ctx.device.As(&ctx.info_queue);
    if (SUCCEEDED(ctx.device.As(&ctx.debug_callbacks))) {
        DX_CHECK_HRESULT(ctx.debug_callbacks->RegisterMessageCallback(
            [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR description, void*) {
                if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                    validation_error_count.fetch_add(1);
                    LOG_ERROR("D3D12 validation {}: {}", uint32(id), description);
                }
            },
            D3D12_MESSAGE_CALLBACK_FLAG_NONE,
            nullptr,
            &ctx.debug_callback_cookie
        ));
    }
    const D3D12MA::ALLOCATOR_DESC allocator_info{.pDevice = ctx.device.Get(), .pAdapter = ctx.adapter.Get()};
    DX_CHECK_HRESULT(D3D12MA::CreateAllocator(&allocator_info, &ctx.allocator));
    ctx.views     = std::make_unique<DescriptorHeap>(ctx.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1000000, true);
    ctx.cpu_views = std::make_unique<DescriptorHeap>(ctx.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 8192, false);
    ctx.samplers  = std::make_unique<DescriptorHeap>(ctx.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2048, true);
    ctx.rtvs      = std::make_unique<DescriptorHeap>(ctx.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 8192, false);
    ctx.dsvs      = std::make_unique<DescriptorHeap>(ctx.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 8192, false);
    // Sampler indices are part of the shared packed texture-handle contract.
    ctx.samplers->Allocate(SF_Num * SAM_Num * SCF_Num);
    for (uint32 compare = 0; compare < SCF_Num; ++compare) {
        for (uint32 address = 0; address < SAM_Num; ++address) {
            for (uint32 filter = 0; filter < SF_Num; ++filter) {
                const Sampler      sampler{ESamplerFilter(filter), ESamplerAddressMode(address), ESamplerCompareFunction(compare)};
                D3D12_SAMPLER_DESC desc{};
                const bool         comparison  = compare != SCF_NEVER;
                const bool         anisotropic = filter == SF_ANISOTROPIC_LINEAR || filter == SF_ANISOTROPIC_NEAREST;
                desc.Filter                    = anisotropic ? (comparison ? D3D12_FILTER_COMPARISON_ANISOTROPIC : D3D12_FILTER_ANISOTROPIC) :
                                                               (filter == SF_NEAREST ? (comparison ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT : D3D12_FILTER_MIN_MAG_MIP_POINT) :
                                                                                       (comparison ? D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_LINEAR));
                desc.AddressU = desc.AddressV = desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE(address + 1);
                desc.MaxAnisotropy                            = 16;
                desc.ComparisonFunc                           = D3D12_COMPARISON_FUNC(compare + 1);
                desc.MaxLOD                                   = D3D12_FLOAT32_MAX;
                ctx.device->CreateSampler(&desc, ctx.samplers->Cpu(ctx.SamplerIndex(sampler)));
            }
        }
    }
    ctx.queue = std::make_unique<Queue>(ctx);
    ctx.copy  = std::make_unique<CopyQueueAdapter>(*ctx.queue);
    LOG_INFO("D3D12 initialized: SM6.8 bindless, DXR={}, mesh shaders={}", ctx.raytracing, ctx.mesh_shaders);
}

D3D12Device::~D3D12Device() = default;
FenceRef D3D12Device::CreateFence() {
    return MoerNew(Timeline)(native->device.Get());
}
BufferRef D3D12Device::CreateBuffer(std::string_view name, uint32 count, uint32 stride, EBufferUsageFlags usage, EPixelFormat format) {
    auto*     buffer = MoerNew(GpuBuffer)(*native, BufferInfo{count, stride, usage, format});
    BufferRef owner{buffer};
    buffer->SetName(name);
    return owner;
}
TextureRef D3D12Device::CreateTexture(std::string_view name, const TextureInfo& info) {
    auto*      texture = MoerNew(GpuTexture)(*native, info);
    TextureRef owner{texture};
    texture->SetName(info.debug_name ? *info.debug_name : name);
    return owner;
}
BindlessArrayRef D3D12Device::CreateBindlessArray(uint32 count) {
    return MoerNew(Bindless)(count);
}
RaytracingGeometryRef D3D12Device::CreateRaytracingGeometry(const RaytracingGeometryInfo& info) {
    if (!native->raytracing)
        throw std::runtime_error("Adapter does not support DXR 1.1");
    return MoerNew(Geometry)(*native, info);
}
RaytracingSceneRef D3D12Device::CreateRaytracingScene() {
    if (!native->raytracing)
        throw std::runtime_error("Adapter does not support DXR 1.1");
    return MoerNew(Scene)(*native);
}
CommandQueue& D3D12Device::GetCommandQueue(EQueueType type) {
    if (type != EQueueType::Graphics && type != EQueueType::Compute && type != EQueueType::Copy) {
        throw std::invalid_argument("Invalid D3D12 logical queue");
    }
    return *native->queue;
}
CopyQueue& D3D12Device::GetCopyQueue() {
    return *native->copy;
}
RHIQueueTopology D3D12Device::GetQueueTopology() const {
    // All logical queues alias the same native Direct Queue. RDG must observe
    // this alias rather than assume asynchronous queue-family ownership.
    return {.graphics = {EQueueType::Graphics, 0, 0, true}, .compute = {EQueueType::Compute, 0, 0, true}, .copy = {EQueueType::Copy, 0, 0, true}};
}
SwapchainRef D3D12Device::CreateSwapchain(const SwapchainCreateInfo& info) {
    return MoerNew(SwapchainImpl)(*native, info);
}
PipelineHandle D3D12Device::CreatePipeline(GfxPsoCreateInfo&& info, PipelineShaderInfo&& shaders) {
    return D3D12::CreatePipeline(*native, std::move(shaders), &info);
}
PipelineHandle D3D12Device::CreatePipeline(PipelineShaderInfo&& shaders) {
    return D3D12::CreatePipeline(*native, std::move(shaders), nullptr);
}
void D3D12Device::WaitIdle() {
    native->queue->Sync();
    FlushDebugMessages();
}
bool D3D12Device::SupportsTessellation() const {
    return true;
}
bool D3D12Device::SupportsMultiview(uint32 view_count) const {
    return native->view_instancing && view_count > 0 && view_count <= D3D12_MAX_VIEW_INSTANCE_COUNT;
}
uint32 D3D12Device::GetMaxTessellationFactor() const {
    return 64;
}
bool D3D12Device::SupportsRaytracingAccelerationStructures() const {
    return native->raytracing;
}
void D3D12Device::FlushDebugMessages() const {
    if (!native->info_queue)
        return;
    const uint64 count = native->info_queue->GetNumStoredMessages();
    for (uint64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        native->info_queue->GetMessage(i, nullptr, &size);
        Array<byte> storage(size);
        auto*       message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(native->info_queue->GetMessage(i, message, &size))) {
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR && !native->debug_callbacks) {
                validation_error_count.fetch_add(1);
                LOG_ERROR("D3D12 validation {}: {}", uint32(message->ID), message->pDescription);
            } else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
                LOG_WARNING("D3D12 validation {}: {}", uint32(message->ID), message->pDescription);
            }
        }
    }
    native->info_queue->ClearStoredMessages();
}
} // namespace Moer::Render

namespace Moer::Render::D3D12 {
Context::~Context() {
    copy.reset();
    queue.reset();
    if (debug_callbacks)
        debug_callbacks->UnregisterMessageCallback(debug_callback_cookie);
}
BufferRef Context::Buffer(uint64 size, EBufferUsageFlags usage, D3D12_HEAP_TYPE heap) {
    return MoerNew(GpuBuffer)(*this, BufferInfo{size, 1, usage}, heap);
}
uint32 Context::SamplerIndex(Sampler sampler) const {
    return uint32(sampler.filter) + SF_Num * (uint32(sampler.address_mode) + SAM_Num * uint32(sampler.compare_function));
}
} // namespace Moer::Render::D3D12
