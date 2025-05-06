#include "rhi/RHIResource.h"
#include "PixelFormat.h"
#include "misc/Crc32.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResourceInitilizer.h"
#include "shader/Shader.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderParameterMacros.h"
#include <algorithm>
#include <misc/Traits.h>
#include <tuple>

//need a mpmc linked list to delete resource
LockFreeQueueBase<RHIResource, false> pending_deletings;

void RHIResource::Destroy() {
    //mark resource to be deleted
    //deferred delete
    // if (!flags.MarkToDelete(std::memory_order_release)) {
    //     //TODO: pending_deletings actual delete on render thread
    //     pending_deletings.Push(this);
    // }
    MoerDelete(this);
    // delete this;
}
namespace Moer::Render {
    TextureView::TextureView(Texture* _texture) : texture(_texture),
                                                  offset(0),
                                                  extent(_texture->GetExtent()),
                                                  mip_level(0),
                                                  array_index(0),
                                                  num_array(_texture->GetNumArray()),
                                                  num_mips(1),
                                                  format(_texture->GetFormat()) {
    }
    TextureView::TextureView(TextureRef _texture_ref) : TextureView(_texture_ref.Get()) {
    }
    TextureView::TextureView(Texture* _tex, EPixelFormat _fmt, uint8 _mip_level, uint8 _mip_cnt) : texture(_tex), format(_fmt), mip_level(_mip_level), num_mips(_mip_cnt), extent(_tex->GetExtent()), array_index(0), num_array(_tex->GetNumArray()) {
        //calculate extent
    }

    TextureView Texture::GetView(uint8 _mip_level, uint8 _mip_cnt) {
        return TextureView(this, this->GetFormat(), _mip_level, _mip_cnt);
    }

    TextureView Texture::GetView(EPixelFormat _format, uint8 _mip_level, uint8 _mip_cnt) {
        return TextureView(this, _format, _mip_level, _mip_cnt);
    }

    BufferView::BufferView(Buffer* _buffer, EPixelFormat _fmt) : buffer(_buffer), byte_offset(0), num_elements(_buffer->GetNumElement()), stride(_buffer->GetStride()), format(_fmt) {
    }
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

    // void BindlessArray::FreeBufferFrameEnd() {
    //     for (uint slot : buffers_freed) {
    //         free_buffer_slots.push_back(slot);
    //     }
    //     buffers_freed.clear();
    // }

    // void BindlessArray::FreeTextureFrameEnd() {
    //     for (uint slot : textures_freed) {
    //         free_texture_slots.push_back(slot);
    //     }
    //     textures_freed.clear();
    // }

    BindlessArray::BindlessArray() : RHIResource(RRT_BINDLESS_ARRAY){};

    RaytracingInstance& RaytracingScene::GetInstance(uint _array_idx) {
        return instances[_array_idx];
    }

    const RaytracingInstance& RaytracingScene::GetInstance(uint _array_idx) const {
        return instances[_array_idx];
    }

}// namespace Moer::Render
