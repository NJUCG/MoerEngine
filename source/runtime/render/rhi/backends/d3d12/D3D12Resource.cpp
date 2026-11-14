#include "D3D12Backend.h"
#include "PixelFormat.h"
#include <algorithm>

namespace Moer::Render::D3D12 {

DescriptorHeap::DescriptorHeap(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, uint32 capacity, bool visible) {
    const D3D12_DESCRIPTOR_HEAP_DESC desc{type, capacity, visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    DX_CHECK_HRESULT(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)));
    stride = device->GetDescriptorHandleIncrementSize(type);
    free_ranges.emplace(0, capacity);
}
uint32 DescriptorHeap::Allocate(uint32 count) {
    std::lock_guard lock(mutex);
    for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
        if (it->second >= count) {
            const auto [index, size] = *it;
            free_ranges.erase(it);
            if (size > count)
                free_ranges.emplace(index + count, size - count);
            return index;
        }
    }
    throw std::runtime_error("D3D12 descriptor heap exhausted");
}
void DescriptorHeap::Free(uint32 index, uint32 count) {
    std::lock_guard lock(mutex);
    auto            next = free_ranges.lower_bound(index);
    if (next != free_ranges.end() && index + count == next->first) {
        count += next->second;
        next = free_ranges.erase(next);
    }
    if (next != free_ranges.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == index) {
            index = prev->first;
            count += prev->second;
            free_ranges.erase(prev);
        }
    }
    free_ranges.emplace(index, count);
}
D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::Cpu(uint32 index) const {
    return {heap->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * stride};
}
D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::Gpu(uint32 index) const {
    return {heap->GetGPUDescriptorHandleForHeapStart().ptr + uint64(index) * stride};
}

GpuBuffer::GpuBuffer(DeviceContext& context, const BufferInfo& info, D3D12_HEAP_TYPE heap) :
    Buffer(info),
    heap_type(heap) {
    state                      = heap == D3D12_HEAP_TYPE_UPLOAD   ? D3D12_RESOURCE_STATE_GENERIC_READ :
                                 heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST :
                                                                    D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (EnumHasAnyFlag(info.usage, EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::ACCELERATION_STRUCTURE)) {
        flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (EnumHasAnyFlag(info.usage, EBufferUsageFlags::ACCELERATION_STRUCTURE))
        state = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    if (heap != D3D12_HEAP_TYPE_DEFAULT)
        flags = D3D12_RESOURCE_FLAG_NONE;
    const auto                     desc = CD3DX12_RESOURCE_DESC::Buffer(Align(std::max<uint64>(GetByteSize(), 4), 256), flags);
    const D3D12MA::ALLOCATION_DESC allocation_desc{.HeapType = heap};
    DX_CHECK_HRESULT(context.allocator->CreateResource(&allocation_desc, &desc, state, nullptr, &allocation, IID_PPV_ARGS(&resource)));
}
void GpuBuffer::SetName(std::string_view name) {
    debug_name = name;
    resource->SetName(Widen(name).c_str());
}

DXGI_FORMAT ResourceFormat(EPixelFormat format) {
    switch (format) {
        case PF_D16_UNORM:
            return DXGI_FORMAT_R16_TYPELESS;
        case PF_D32_SFLOAT:
            return DXGI_FORMAT_R32_TYPELESS;
        case PF_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24G8_TYPELESS;
        case PF_D32_SFLOAT_S8_UINT:
            return DXGI_FORMAT_R32G8X24_TYPELESS;
        case PF_R8G8B8A8_SRGB:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case PF_B8G8R8A8_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        default:
            return Format(format);
    }
}
DXGI_FORMAT SampleFormat(EPixelFormat format) {
    switch (format) {
        case PF_D16_UNORM:
            return DXGI_FORMAT_R16_UNORM;
        case PF_D32_SFLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case PF_D24_UNORM_S8_UINT:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case PF_D32_SFLOAT_S8_UINT:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        default:
            return Format(format);
    }
}
DXGI_FORMAT StorageFormat(EPixelFormat format) {
    if (format == PF_R8G8B8A8_SRGB)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == PF_B8G8R8A8_SRGB)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    return Format(format);
}
GpuTexture::GpuTexture(DeviceContext& context, const TextureInfo& info) :
    Texture(info) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension        = info.dimension == ETextureDimension::TEX_3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width            = info.extent.x;
    desc.Height           = info.extent.y;
    desc.DepthOrArraySize = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? info.depth : info.array_size;
    desc.MipLevels        = info.num_mips;
    desc.Format           = ResourceFormat(info.format);
    desc.SampleDesc.Count = info.num_samples;
    desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (EnumHasAnyFlag(info.usage, ETextureUsageFlags::UNORDERED_ACCESS))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (EnumHasAnyFlag(info.usage, ETextureUsageFlags::COLOR_ATTACHMENT))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (EnumHasAnyFlag(info.usage, ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT))
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    const D3D12MA::ALLOCATION_DESC allocation_desc{.HeapType = D3D12_HEAP_TYPE_DEFAULT};
    D3D12_CLEAR_VALUE              clear{};
    clear.Format     = Format(info.format);
    const bool depth = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    if (depth) {
        if (const auto* value = std::get_if<RHIClearAttachment::ClearDepthStencilValue>(&info.clear_attachment.value))
            clear.DepthStencil = {value->depth, uint8(value->stencil)};
    } else if (const auto* value = std::get_if<RHIClearAttachment::ClearColorValue>(&info.clear_attachment.value)) {
        std::copy(std::begin(value->float32), std::end(value->float32), clear.Color);
    }
    DX_CHECK_HRESULT(context.allocator->CreateResource(&allocation_desc, &desc, D3D12_RESOURCE_STATE_COMMON, (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) ? &clear : nullptr, &allocation, IID_PPV_ARGS(&resource)));
    uint32 planes = D3D12GetFormatPlaneCount(context.device.Get(), desc.Format);
    states.resize(info.num_mips * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : info.array_size) * planes, D3D12_RESOURCE_STATE_COMMON);
}
void GpuTexture::SetName(std::string_view name) {
    debug_name = name;
    resource->SetName(Widen(name).c_str());
}
uint32 GpuTexture::GetMipByteSize(uint32 mip) const {
    return GetSizeFromImageFormat(GetFormat(), {std::max(1u, GetWidth() >> mip), std::max(1u, GetHeight() >> mip), std::max(1u, GetDepth() >> mip)});
}
uint32 GpuTexture::Subresource(uint32 mip, uint32 layer, uint32 plane) const {
    return mip + layer * GetNumMips() + plane * GetNumMips() * (GetDimension() == ETextureDimension::TEX_3D ? 1 : GetNumArray());
}

Timeline::Timeline(ID3D12Device* device) {
    DX_CHECK_HRESULT(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
}
uint64 Timeline::GetValue() const {
    const uint64 value = fence->GetCompletedValue();
    return value == UINT64_MAX ? 0 : value;
}
bool Timeline::IsRejected(uint64 value) const {
    std::lock_guard lock(mutex);
    return rejected.contains(value);
}
void Timeline::MarkSubmitted(uint64 value) {
    {
        std::lock_guard lock(mutex);
        submitted = std::max(submitted, value);
    }
    cv.notify_all();
}
bool Timeline::WaitSubmitted(uint64 value, const std::atomic_bool* continuing) {
    std::unique_lock lock(mutex);
    while (submitted < value && !rejected.contains(value) && (!continuing || continuing->load())) {
        if (fence->GetCompletedValue() == UINT64_MAX)
            return false;
        cv.wait_for(lock, std::chrono::milliseconds(20));
    }
    return submitted >= value && !rejected.contains(value);
}
void Timeline::Reject(uint64 value) noexcept {
    try {
        {
            std::lock_guard lock(mutex);
            rejected.insert(value);
        }
        cv.notify_all();
    } catch (...) {
    }
}
bool Timeline::Wait(uint64 value) {
    // Each waiter owns its event. Sharing an auto-reset event can strand a
    // concurrent waiter, and a rejected value must never be host-signalled.
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return false;
    const auto close = OnScopeExit([&] {
        CloseHandle(event);
    });
    if (FAILED(fence->SetEventOnCompletion(value, event)))
        return false;
    while (fence->GetCompletedValue() < value && !IsRejected(value))
        WaitForSingleObject(event, 20);
    return fence->GetCompletedValue() != UINT64_MAX && !IsRejected(value);
}
SubmissionPacket::~SubmissionPacket() {
    // Native execution clears the journal at handoff. Before that boundary,
    // destruction retains responsibility for undoing unsubmitted changes.
    for (auto it = rollback.rbegin(); it != rollback.rend(); ++it)
        (*it)();
    for (auto [heap, range] : descriptors)
        heap->Free(range.first, range.second);
}
} // namespace Moer::Render::D3D12
