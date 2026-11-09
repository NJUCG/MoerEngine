#include "D3D12Backend.h"
#include "PixelFormat.h"
#include <cstring>

namespace Moer::Render::D3D12 {
namespace {
struct Footprint {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    uint32                             rows      = 0;
    uint64                             row_bytes = 0;
    uint64                             size      = 0;
    uint64                             TightSize() const {
        return row_bytes * rows * layout.Footprint.Depth;
    }
    uint64 Offset(uint32 z, uint32 row) const {
        return uint64(z * rows + row) * layout.Footprint.RowPitch;
    }
};
Footprint CopyFootprint(DeviceContext& context, GpuTexture& texture, uint3 extent) {
    if (!extent.x || !extent.y || !extent.z || texture.GetNumSamples() != 1)
        throw std::invalid_argument("Invalid D3D12 linear texture copy extent or sample count");
    auto desc             = texture.resource->GetDesc();
    desc.Width            = extent.x;
    desc.Height           = extent.y;
    desc.DepthOrArraySize = texture.GetDimension() == ETextureDimension::TEX_3D ? uint16(extent.z) : 1;
    desc.MipLevels        = 1;
    Footprint result;
    context.device->GetCopyableFootprints(&desc, 0, 1, 0, &result.layout, &result.rows, &result.row_bytes, &result.size);
    if (!result.rows || !result.row_bytes || result.size == UINT64_MAX)
        throw std::invalid_argument("Invalid D3D12 texture footprint");
    // Reserve the trailing row padding as well: GPU row copies can write it.
    result.size = uint64(result.layout.Footprint.RowPitch) * result.rows * result.layout.Footprint.Depth;
    return result;
}
void ValidateBufferRange(GpuBuffer& buffer, uint64 offset, uint64 size) {
    if (offset > buffer.GetByteSize() || size > buffer.GetByteSize() - offset)
        throw std::out_of_range("D3D12 buffer copy range");
}
void ReadBuffer(GpuBuffer& buffer, uint64 offset, uint64 size, void* destination) {
    void*             mapped = nullptr;
    const D3D12_RANGE range{SIZE_T(offset), SIZE_T(offset + size)};
    DX_CHECK_HRESULT(buffer.resource->Map(0, &range, &mapped));
    std::memcpy(destination, static_cast<const byte*>(mapped) + offset, SIZE_T(size));
    const D3D12_RANGE empty{0, 0};
    buffer.resource->Unmap(0, &empty);
}
void ReadTexture(GpuBuffer& buffer, const Footprint& footprint, std::span<byte> destination) {
    if (destination.size() < footprint.TightSize())
        throw std::out_of_range("D3D12 readback destination too small");
    void*             mapped = nullptr;
    const D3D12_RANGE range{0, SIZE_T(footprint.size)};
    DX_CHECK_HRESULT(buffer.resource->Map(0, &range, &mapped));
    for (uint32 z = 0; z < footprint.layout.Footprint.Depth; ++z)
        for (uint32 row = 0; row < footprint.rows; ++row)
            std::memcpy(destination.data() + uint64(z * footprint.rows + row) * footprint.row_bytes, static_cast<const byte*>(mapped) + footprint.Offset(z, row), SIZE_T(footprint.row_bytes));
    const D3D12_RANGE empty{0, 0};
    buffer.resource->Unmap(0, &empty);
}
} // namespace

void Recorder::Copy(const Command& command) {
    using Type = Command::EType;
    if (command.Type() == Type::UploadBuffer) {
        const auto& cmd = static_cast<const UploadBufferCmd&>(command);
        auto&       dst = *reinterpret_cast<GpuBuffer*>(cmd.Handle());
        ValidateBufferRange(dst, cmd.Offset(), cmd.ByteSize());
        const auto source = Upload(cmd.Data());
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(dst.resource.Get(), cmd.Offset(), static_cast<GpuBuffer*>(source.Get())->resource.Get(), 0, cmd.ByteSize());
    } else if (command.Type() == Type::BufferToBuffer) {
        const auto& cmd = static_cast<const CopyBufferCmd&>(command);
        auto&       src = *reinterpret_cast<GpuBuffer*>(cmd.SrcHandle());
        auto&       dst = *reinterpret_cast<GpuBuffer*>(cmd.DstHandle());
        ValidateBufferRange(src, cmd.SrcOffset(), cmd.ByteSize());
        ValidateBufferRange(dst, cmd.DstOffset(), cmd.ByteSize());
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(dst.resource.Get(), cmd.DstOffset(), src.resource.Get(), cmd.SrcOffset(), cmd.ByteSize());
    } else if (command.Type() == Type::CopyBackBuffer) {
        const auto& cmd = static_cast<const CopyBackBufferCmd&>(command);
        auto&       src = *reinterpret_cast<GpuBuffer*>(cmd.Handle());
        ValidateBufferRange(src, cmd.Offset(), cmd.ByteSize());
        auto readback = context.Buffer(cmd.ByteSize(), EBufferUsageFlags::NONE, D3D12_HEAP_TYPE_READBACK);
        packet.buffers.push_back(readback);
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(static_cast<GpuBuffer*>(readback.Get())->resource.Get(), 0, src.resource.Get(), cmd.Offset(), cmd.ByteSize());
        packet.readbacks.push_back([readback, &cmd] {
            auto& native = *static_cast<GpuBuffer*>(readback.Get());
            if (cmd.HasOwningReadback()) {
                struct Data {
                    GpuBuffer* buffer;
                    uint64     size;
                } data{&native, cmd.ByteSize()};
                if (!ReadbackBackendAccess::MaterializePayload(cmd.OwningReadback(), &data, [](void* context, std::span<byte> bytes) {
                        auto& data = *static_cast<Data*>(context);
                        ReadBuffer(*data.buffer, 0, data.size, bytes.data());
                    }))
                    throw std::runtime_error("D3D12 buffer payload materialization failed");
            } else
                ReadBuffer(native, 0, cmd.ByteSize(), cmd.Data());
        });
    } else if (command.Type() == Type::UploadTexture || command.Type() == Type::BufferToTexture) {
        const auto*  upload    = command.Type() == Type::UploadTexture ? &static_cast<const UploadTextureCmd&>(command) : nullptr;
        const auto*  copy      = upload ? nullptr : &static_cast<const CopyBufferToTextureCmd&>(command);
        auto&        dst       = *reinterpret_cast<GpuTexture*>(upload ? upload->Handle() : copy->DstHandle());
        const uint3  extent    = upload ? upload->Size() : copy->Size();
        const uint3  offset    = upload ? upload->Offset() : copy->DstOffset();
        const uint32 mip       = upload ? upload->MipLevel() : copy->MipLevel();
        const uint32 layer     = upload ? upload->ArrayLayer() : copy->ArrayLayer();
        const auto   footprint = CopyFootprint(context, dst, extent);
        BufferRef    staging;
        if (upload) {
            if (upload->Data().size() < footprint.TightSize())
                throw std::invalid_argument("D3D12 texture upload data too small");
            Array<byte> padded(footprint.size);
            for (uint32 z = 0; z < footprint.layout.Footprint.Depth; ++z)
                for (uint32 row = 0; row < footprint.rows; ++row)
                    std::memcpy(padded.data() + footprint.Offset(z, row), upload->Data().data() + uint64(z * footprint.rows + row) * footprint.row_bytes, SIZE_T(footprint.row_bytes));
            staging = Upload(padded);
        } else {
            auto& src = *reinterpret_cast<GpuBuffer*>(copy->SrcHandle());
            ValidateBufferRange(src, copy->SrcOffset(), footprint.TightSize());
            staging = context.Buffer(footprint.size);
            packet.buffers.push_back(staging);
            auto& native = *static_cast<GpuBuffer*>(staging.Get());
            Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(native, D3D12_RESOURCE_STATE_COPY_DEST);
            for (uint32 z = 0; z < footprint.layout.Footprint.Depth; ++z)
                for (uint32 row = 0; row < footprint.rows; ++row)
                    list->CopyBufferRegion(native.resource.Get(), footprint.Offset(z, row), src.resource.Get(), copy->SrcOffset() + uint64(z * footprint.rows + row) * footprint.row_bytes, footprint.row_bytes);
            Transition(native, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, mip, 1, layer, 1);
        const CD3DX12_TEXTURE_COPY_LOCATION destination(dst.resource.Get(), dst.Subresource(mip, layer));
        const CD3DX12_TEXTURE_COPY_LOCATION source(static_cast<GpuBuffer*>(staging.Get())->resource.Get(), footprint.layout);
        list->CopyTextureRegion(&destination, offset.x, offset.y, offset.z, &source, nullptr);
    } else if (command.Type() == Type::CopyBackTexture || command.Type() == Type::TextureToBuffer) {
        const auto*  read      = command.Type() == Type::CopyBackTexture ? &static_cast<const CopyBackTextureCmd&>(command) : nullptr;
        const auto*  copy      = read ? nullptr : &static_cast<const CopyTextureToBufferCmd&>(command);
        auto&        src       = *reinterpret_cast<GpuTexture*>(read ? read->Handle() : copy->SrcHandle());
        const uint3  extent    = read ? read->Size() : copy->Size();
        const uint3  offset    = read ? read->Offset() : copy->SrcOffset();
        const uint32 mip       = read ? read->MipLevel() : copy->MipLevel();
        const uint32 layer     = read ? read->ArrayLayer() : copy->ArrayLayer();
        const auto   footprint = CopyFootprint(context, src, extent);
        auto         staging   = context.Buffer(footprint.size, EBufferUsageFlags::NONE, read ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT);
        packet.buffers.push_back(staging);
        auto& native = *static_cast<GpuBuffer*>(staging.Get());
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, mip, 1, layer, 1);
        Transition(native, D3D12_RESOURCE_STATE_COPY_DEST);
        const CD3DX12_TEXTURE_COPY_LOCATION source(src.resource.Get(), src.Subresource(mip, layer));
        const CD3DX12_TEXTURE_COPY_LOCATION destination(native.resource.Get(), footprint.layout);
        const D3D12_BOX                     box{offset.x, offset.y, offset.z, offset.x + extent.x, offset.y + extent.y, offset.z + extent.z};
        const bool                          full_subresource = offset.x == 0 && offset.y == 0 && offset.z == 0 &&
                                      extent.x == std::max(1u, src.GetWidth() >> mip) && extent.y == std::max(1u, src.GetHeight() >> mip) && extent.z == std::max(1u, src.GetDepth() >> mip);
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, full_subresource ? nullptr : &box);
        if (read) {
            packet.readbacks.push_back([staging, footprint, read] {
                auto& buffer = *static_cast<GpuBuffer*>(staging.Get());
                if (read->HasOwningReadback()) {
                    struct Data {
                        GpuBuffer*       buffer;
                        const Footprint* footprint;
                    } data{&buffer, &footprint};
                    if (!ReadbackBackendAccess::MaterializePayload(read->OwningReadback(), &data, [](void* context, std::span<byte> bytes) {
                            const auto& data = *static_cast<Data*>(context);
                            ReadTexture(*data.buffer, *data.footprint, bytes);
                        }))
                        throw std::runtime_error("D3D12 texture payload materialization failed");
                } else
                    ReadTexture(buffer, footprint, read->Data());
            });
        } else {
            auto& dst = *reinterpret_cast<GpuBuffer*>(copy->DstHandle());
            ValidateBufferRange(dst, copy->DstOffset(), footprint.TightSize());
            Transition(native, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST);
            for (uint32 z = 0; z < footprint.layout.Footprint.Depth; ++z)
                for (uint32 row = 0; row < footprint.rows; ++row)
                    list->CopyBufferRegion(dst.resource.Get(), copy->DstOffset() + uint64(z * footprint.rows + row) * footprint.row_bytes, native.resource.Get(), footprint.Offset(z, row), footprint.row_bytes);
        }
    } else if (command.Type() == Type::TextureToTexture) {
        const auto& cmd = static_cast<const CopyTextureCmd&>(command);
        auto&       src = *reinterpret_cast<GpuTexture*>(cmd.SrcHandle());
        auto&       dst = *reinterpret_cast<GpuTexture*>(cmd.DstHandle());
        Transition(src, D3D12_RESOURCE_STATE_COPY_SOURCE, cmd.SrcMipLevel(), 1, cmd.SrcArrayLayer(), 1);
        Transition(dst, D3D12_RESOURCE_STATE_COPY_DEST, cmd.DstMipLevel(), 1, cmd.DstArrayLayer(), 1);
        const CD3DX12_TEXTURE_COPY_LOCATION source(src.resource.Get(), src.Subresource(cmd.SrcMipLevel(), cmd.SrcArrayLayer()));
        const CD3DX12_TEXTURE_COPY_LOCATION destination(dst.resource.Get(), dst.Subresource(cmd.DstMipLevel(), cmd.DstArrayLayer()));
        const auto                          start  = cmd.SrcOffset();
        const auto                          end    = start + cmd.Size();
        const auto                          offset = cmd.DstOffset();
        const D3D12_BOX                     box{start.x, start.y, start.z, end.x, end.y, end.z};
        const bool                          full_subresource = start.x == 0 && start.y == 0 && start.z == 0 &&
                                      cmd.Size().x == std::max(1u, src.GetWidth() >> cmd.SrcMipLevel()) && cmd.Size().y == std::max(1u, src.GetHeight() >> cmd.SrcMipLevel()) && cmd.Size().z == std::max(1u, src.GetDepth() >> cmd.SrcMipLevel());
        list->CopyTextureRegion(&destination, offset.x, offset.y, offset.z, &source, full_subresource ? nullptr : &box);
    }
}

void CopyQueueAdapter::CopyFrom(BufferView src, BufferView dst) {
    CommandList list;
    list.CopyFrom(src, dst);
    Execute(list.Submit());
}
void CopyQueueAdapter::CopyFrom(TextureView src, TextureView dst) {
    CommandList list;
    list.CopyFrom(src, dst);
    Execute(list.Submit());
}
void CopyQueueAdapter::CopyFrom(TextureView src, BufferView dst) {
    CommandList list;
    list.CopyFrom(src, dst);
    Execute(list.Submit());
}
void CopyQueueAdapter::CopyFrom(BufferView src, TextureView dst) {
    CommandList list;
    list.CopyFrom(src, dst);
    Execute(list.Submit());
}
void CopyQueueAdapter::CopyFrom(std::span<byte> data, BufferView dst) {
    CommandList list;
    Array<byte> owned(data.begin(), data.end());
    list.CopyFrom(std::move(owned), dst);
    Execute(list.Submit());
}
void CopyQueueAdapter::CopyFrom(std::span<byte> data, TextureView dst) {
    CommandList list;
    Array<byte> owned(data.begin(), data.end());
    list.CopyFrom(std::move(owned), dst);
    Execute(list.Submit());
}
} // namespace Moer::Render::D3D12
