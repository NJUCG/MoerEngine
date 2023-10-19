#include "rhi/RHIResource.h"
#include "rhi/RHI.h"
#include "rhi/RHICommandList.h"

void RHIResource::Destroy() const {
    //mark resource to be deleted
    if (!flags.MarkToDelete(std::memory_order_release)) {
        //        pending_deletings.pr
    }
}

#pragma region buffer texture initiation
RHITexture::RHITexture(const RHITextureCreateInfo& _info) : RHIViewableResource(RRT_TEXTURE), texture_info(_info) {
    SetName(_info.name);
}

RHIViewInfo::Buffer::ViewInfo RHIViewInfo::Buffer::GetViewInfo(RHIBuffer* target) const {
    assert(target && "invalid buffer.");
    const auto& info = target->GetInfo();
    if (info.IsNull()) return {.b_null_view = true};
    uint32_t     _byte_stride  = 0;
    EPixelFormat _format       = format;
    EBufferType  _buffer_type  = bufferType;
    uint32_t     _byte_offset  = 0;
    uint32_t     _byte_size    = 0;
    uint32_t     _num_elements = 0;

    switch (bufferType) {
        case EBufferType::STRUCTURED:
            assert(EnumHasAnyFlag(info.usage, EBufferUsageFlags::STRUCTURED_BUFFER) && "the buffer is not a structured buffer.");
            assert(format == PF_UNDEFINED && "structured buffer should not have a pixel format.");
            _byte_stride = stride == 0 ? info.stride : stride;
            break;
        case EBufferType::ACCELERATION_STRUCTURE:
            assert(EnumHasAnyFlag(info.usage, EBufferUsageFlags::ACCELERATION_STRUCTURE) && "the buffer is not a ray-tracing acceleration buffer");
            assert(format == PF_UNDEFINED && "acceleration structure views should not specify pixel format.");
            _byte_stride = 1;
            break;
        case EBufferType::RAW:
            break;
            //todo: query support for byte address buffer
            assert(format == PF_UNDEFINED && "byte addressed buffer should not specify pixel format");
            assert(stride == 0 && "should not specify stride of raw buffer");
            assert(byte_offset % 16 && "");
            _byte_stride = sizeof(uint32_t);
            _format      = PF_UNDEFINED;
        default: assert(false && "unrecognized buffer type");
    }
    assert(byte_offset < info.size && "offset out of bounds of buffer size");
    assert(byte_offset % _byte_stride == 0 && "offset must be a multiple of stride");
    _byte_offset = byte_offset;
    assert(_buffer_type == EBufferType::ACCELERATION_STRUCTURE || (byte_offset == 0 || num_elements > 0) && "");
    _num_elements = num_elements == 0 ? (info.size - byte_offset) / _byte_stride : num_elements;
    _byte_size    = _num_elements * _byte_stride;

    assert(_byte_offset + _byte_size < info.size && "total size out of bound");

    //todo platform support query
    assert(_format == PF_UNDEFINED || true && "platform not support such pixel format");
    return {
        _byte_offset,
        _byte_stride,
        _num_elements,
        _byte_size,
        _buffer_type,
        _format};
}

RHIViewInfo::BufferSRV::ViewInfo RHIViewInfo::BufferSRV::GetViewInfo(RHIBuffer* target) const {
    assert(view_type == EViewType::BUFFER_SRV);
    return ViewInfo{Buffer::GetViewInfo(target)};
}
RHIViewInfo::Texture::ViewInfo RHIViewInfo::Texture::GetViewInfo(RHITexture* target) const {
    assert(target && "invalid target texture.");
    const auto& info = target->GetInfo();

    assert(mip_num > 0 || mip_min == 0 && "num mips cannot be 0 except creating entire range");
    assert(mip_min + mip_num <= info.num_mips && "mip range out of bound");
    EPixelFormat      _format    = format == PF_UNDEFINED ? info.format : format;
    ETextureDimension _dimension = dimension;
    //todo check platform support
    assert(_format == PF_UNDEFINED || true && "pixel format not supported");

    //b_all_mip is not set, it varies between view types
    return {
        array_min,
        array_num == 0 ? info.array_size : array_num,
        _format,
        dimension,
        false,
        mip_num == 0 && mip_min == 0};
}
RHIViewInfo::BufferUAV::ViewInfo RHIViewInfo::BufferUAV::GetViewInfo(RHIBuffer* target) const {
    assert(view_type == EViewType::BUFFER_UAV);
    ViewInfo _info         = {Buffer::GetViewInfo(target)};
    _info.b_atomic_counter = b_is_atomic_counter;
    _info.b_append_buffer  = b_is_append_buffer;
    return _info;
}
RHIViewInfo::TextureSRV::ViewInfo RHIViewInfo::TextureSRV::GetViewInfo(RHITexture* target) const {
    assert(view_type == EViewType::TEXTURE_SRV);
    assert(target);
    const auto& tex_info = target->GetInfo();

    ViewInfo _info   = {Texture::GetViewInfo(target)};
    _info.mip_min    = mip_min;
    _info.mip_num    = mip_num == 0 ? tex_info.num_mips : mip_num;
    _info.b_all_mips = mip_min == 0 && _info.mip_num == tex_info.num_mips;

    return _info;
}
RHIViewInfo::TextureUAV::ViewInfo RHIViewInfo::TextureUAV::GetViewInfo(RHITexture* target) const {
    assert(view_type == EViewType::TEXTURE_UAV);
    assert(target);
    const auto& tex_info = target->GetInfo();

    ViewInfo _info = {Texture::GetViewInfo(target)};
    assert(tex_info.num_samples == 1 && "cannot create uav on multi-sampled texture");

    _info.mip_level = mip_min;
    assert(_info.mip_level <= tex_info.num_mips && "mip level out of bounds");

    _info.b_all_mips = tex_info.num_mips == 1;
    return _info;
}

RHITextureReference::RHITextureReference(
    RHITexture*            _texture,
    RHIShaderResourceView* _bindless_view)
    : RHITexture(RRT_TEXTURE_REFERENCE),
      texture_ref(_texture),
      bindless_view(_bindless_view){

      };

RHITextureReference::~RHITextureReference() = default;

RHITextureReference* RHITextureReference::GetTextureRef() {
    return this;
};

void* RHITextureReference::GetNativeResource() const {
    return texture_ref->GetNativeResource();
}
void* RHITextureReference::GetNativeShaderResourceView() const {
    return texture_ref->GetNativeShaderResourceView();
}
const RHITextureInfo& RHITextureReference::GetInfo() const {
    return texture_ref->GetInfo();
}

void RHIUploadBuffer(const uint8_t* data, uint32_t size, RHIBuffer* _target) {
    //todo: not implemented
    g_rhi->RHIUploadBuffer(_target, data, size);
};
#pragma endregion

#pragma region rende query
RHIPooledRenderQuery::~RHIPooledRenderQuery() {
    Release();
}

void RHIPooledRenderQuery::Release() {
    if (pool != nullptr && IsValid()) {
        pool->ReleaseQuery(std::move(query_ref));
        pool = nullptr;
    }
    assert(!IsValid() && "Query not successfully released");
}
#pragma endregion