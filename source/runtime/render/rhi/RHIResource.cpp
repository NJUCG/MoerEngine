#include "rhi/RHIResource.h"
#include "PixelFormat.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"

#include <algorithm>
#include <misc/Traits.h>

void RHIResource::Destroy() {
    MoerDelete(this);
}
namespace Moer::Render {
TextureView::TextureView(Texture* _texture) :
    texture(_texture),
    offset(0),
    extent(_texture->GetExtent()),
    mip_level(0),
    array_layer(0),
    num_array(_texture->GetNumArray()),
    num_mips(1),
    format(_texture->GetFormat()) {}
TextureView::TextureView(TextureRef _texture_ref) : TextureView(_texture_ref.Get()) {}
TextureView::TextureView(Texture* _tex, EPixelFormat _fmt, uint8 _mip_level, uint8 _mip_cnt) :
    texture(_tex),
    format(_fmt),
    mip_level(_mip_level),
    num_mips(_mip_cnt),
    extent(_tex->GetExtent()),
    array_layer(0),
    num_array(_tex->GetNumArray()) {
    //calculate extent
}

//in case of const TextureView, so return copy
TextureView TextureView::Slice(uint layer, uint count) const {
    TextureView copy = *this;
    copy.array_layer = layer;
    copy.num_array   = count;
    return copy;
}

TextureView Texture::GetView(uint8 _mip_level, uint8 _mip_cnt) {
    return TextureView(this, this->GetFormat(), _mip_level, _mip_cnt);
}

TextureView Texture::GetView(EPixelFormat _format, uint8 _mip_level, uint8 _mip_cnt) {
    return TextureView(this, _format, _mip_level, _mip_cnt);
}

BufferView::BufferView(Buffer* _buffer, EPixelFormat _fmt) :
    buffer(_buffer),
    byte_offset(0),
    num_elements(_buffer->GetNumElement()),
    stride(_buffer->GetStride()),
    format(_fmt) {}
BufferView Buffer::GetView(uint64_t _byte_offset, uint64_t _byte_size) {
    if (_byte_size == UINT64_MAX && _byte_offset == 0) {
        return BufferView(this, info.format);
    }
    _byte_size = std::min(_byte_size, GetByteSize() - _byte_offset);
    return BufferView(this, _byte_offset, _byte_size / GetStride(), GetStride(), info.format);
}

BufferView Buffer::GetView(EPixelFormat _fmt, uint64_t _byte_offset, uint64_t _byte_size) {
    if (_byte_size == UINT64_MAX && _byte_offset == 0) {
        return BufferView(this, _fmt);
    }
    _byte_size = std::min(_byte_size, GetByteSize() - _byte_offset);
    return BufferView(this, _byte_offset, _byte_size / GetStride(), GetStride(), _fmt);
}

BindlessArray::BindlessArray() : RHIResource(RRT_BINDLESS_ARRAY) {};

RaytracingInstance& RaytracingScene::GetInstance(uint _array_idx) {
    return instances[_array_idx];
}

const RaytracingInstance& RaytracingScene::GetInstance(uint _array_idx) const {
    return instances[_array_idx];
}

uint RaytracingScene::GetInstanceCount() const {
    return instances.size();
}

} // namespace Moer::Render
