#include "rhi/RHIResource.h"
#include "PixelFormat.h"
#include "misc/Crc32.h"
#include "misc/STL.h"
#include "resources/ResourceTransition.h"
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

#pragma region buffer texture initiation
// void RHITexture::SetLayout(const RHISubresourceRange& _subresource_range, ETextureLayout _layout, RHITextureBarrierInfo* barrier_info) {
//     auto old_layout = this->GetLayout(_subresource_range);
//     if (barrier_info) {
//         auto [src_access_flags, dst_access_flags, src_stage, dst_stage] = Moer::ResourceTransition::GetImageTransition(old_layout, _layout);
//         barrier_info->SetSubResourceRange(_subresource_range)
//             .SetTexture(this)
//             .SetSrcTextureLayout(old_layout)
//             .SetDstTextureLayout(_layout)
//             .SetSrcAccessFlags(src_access_flags)
//             .SetDstAccessFlags(dst_access_flags)
//             .SetSrcStage(src_stage)
//             .SetDstStage(dst_stage);
//     }
//     subresource_layouts[_subresource_range] = _layout;
// }

void RHITexture::SetTrackInfo(const RHISubresourceRange& _subresource_range, ETextureStateFlags _state, EPassType _pass_type) {
    using namespace Moer;
    uint mip_cnt = std::min(texture_info.num_mips, _subresource_range.num_mips);
    for (uint i = 0; i < mip_cnt; ++i) {
        uint cur_mip        = _subresource_range.mip_index + i;
        mip_usages[cur_mip] = std::make_tuple(_state, _pass_type);
    }
}
RHITexture::RHITexture(const RHITextureCreateInfo& _info) : RHIViewableResource(RRT_TEXTURE), texture_info(_info) {
    SetName(_info.name);
}

RHITextureReference::RHITextureReference(
    RHITexture* _texture,
    RHISRV*     _bindless_view)
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

RHIResource* GetResource(uint8_t* data, uint32_t offset) {
    return (RHIResource*)(*(uint64_t*)(data + offset));
}

AttachmentBindingSlots* GetAttachmentBindings(uint8_t* data, uint32_t offset) {
    return (AttachmentBindingSlots*)(data + offset);
}

void RHIBatchedShaderParameters::SetParameters(RHIResource* resource, uint16_t slot, uint16_t space) {
    resource_parameters.emplace_back(RHIShaderResourceParameter(resource, slot, space));
}

void RHIBatchedShaderParameters::SetParameters(RHIShader* shader, size_t _data_size, uint8_t* data_source, bool _set_constants) {
    SetParameters(shader->GetMetaShader(), _data_size, data_source, _set_constants);
}
RHIBatchedShaderParameters::~RHIBatchedShaderParameters() {
}
void RHIBatchedShaderParameters::SetParameters(const Shader* shader, size_t _data_size, uint8_t* data_source, bool _set_constants) {
    const auto& param_layout_info = shader->GetRootParametersLayoutInfo();

    //not presices since it may contains const data
    uint32_t resource_param_max_size = _data_size >> 3;
    size_t   left_size               = resource_parameters.capacity() - resource_parameters.size();

    if (left_size < resource_param_max_size) resource_parameters.reserve(left_size + resource_parameters.size());
    for (const auto& param_info : param_layout_info.GetBindingInfo()) {
        uint32_t stride = param_info.stride;
        uint32_t num    = stride / sizeof(RHIResource*);

        bool b_set = false;
        for (uint32_t i = 0; i < num; ++i) {
            RHIResource* data = GetResource(data_source, param_info.offset + i * sizeof(RHIResource*));
            b_set |= param_info.IsValid() && IsParameterResource(param_info.type) && data;
        }

        if (b_set) {
            for (uint32_t i = 0; i < num; ++i) {
                RHIResource* data = GetResource(data_source, param_info.offset + i * sizeof(RHIResource*));
                resource_parameters.emplace_back(RHIShaderResourceParameter(data, param_info.slot, param_info.space));
            }
        }
    }
    if (!_set_constants) {
        return;
    }
    if (param_layout_info.GetConstantsInfo().size() > 0) {
        raw_data.clear();
        constant_parameters.clear();
    }
    for (const auto& param_info : param_layout_info.GetConstantsInfo()) {
        uint8_t* data = data_source + param_info.offset;
        //const must set
        uint32_t origin_offset = raw_data.size();
        raw_data.resize(origin_offset + param_info.stride);
        memcpy(&raw_data[origin_offset], data, param_info.stride);
        constant_parameters.emplace_back(shader->GetShaderType(), origin_offset, (param_info.stride + sizeof(uint32_t) - 1) / 4, param_info.slot, param_info.space);
    }
}

RHIBufferRef RHIRenderPrimitive::GetVertexBuffer() const {
    return m_vertex_buffer;
}
RHIBufferRef RHIRenderPrimitive::GetIndexBuffer() const {
    return m_index_buffer;
}
EPrimitiveType RHIRenderPrimitive::GetPrimitiveType() const {
    return m_type;
}
uint32_t RHIRenderPrimitive::GetOffset() const {
    return m_offset;
}
uint32_t RHIRenderPrimitive::GetCount() const {
    return m_count;
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
