#include "rhi/RHIResource.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "shader/Shader.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderParameterMacros.h"

//need a mpmc linked list to delete resource
LockFreeQueueBase<RHIResource> pending_deletings;

void RHIResource::Destroy() {
    //mark resource to be deleted
    //deferred delete
    if (!flags.MarkToDelete(std::memory_order_release)) {
        //TODO: pending_deletings actual delete on render thread
        pending_deletings.Push(this);
    }
    // delete this;
}

#pragma region buffer texture initiation
RHITexture::RHITexture(const RHITextureCreateInfo& _info) : RHIViewableResource(RRT_TEXTURE), texture_info(_info) {
    SetName(_info.name);
}

RHIViewInfo::Buffer::ViewInfo RHIViewInfo::Buffer::GetViewInfo(RHIBuffer* target) const {
    assert(target && "invalid buffer.");
    const auto& info = target->GetInfo();
    if (info.IsNull()) return {.b_null_view = true};
    uint32_t     temp_byte_stride  = 0;
    EPixelFormat temp_format       = format;
    EBufferType  temp_buffer_type  = buffer_type;
    uint32_t     temp_byte_offset  = 0;
    uint32_t     temp_byte_size    = 0;
    uint32_t     temp_num_elements = 0;

    switch (temp_buffer_type) {
        case EBufferType::STRUCTURED:
            assert(EnumHasAnyFlag(info.usage, EBufferUsageFlags::STRUCTURED_BUFFER) && "the buffer is not a structured buffer.");
            assert(format == PF_UNDEFINED && "structured buffer should not have a pixel format.");
            temp_byte_stride = stride == 0 ? info.stride : stride;
            break;
        case EBufferType::ACCELERATION_STRUCTURE:
            assert(EnumHasAnyFlag(info.usage, EBufferUsageFlags::ACCELERATION_STRUCTURE) && "the buffer is not a ray-tracing acceleration buffer");
            assert(format == PF_UNDEFINED && "acceleration structure views should not specify pixel format.");
            temp_byte_stride = 1;
            break;
        case EBufferType::RAW:
            break;
            //todo: query support for byte address buffer
            assert(format == PF_UNDEFINED && "byte addressed buffer should not specify pixel format");
            assert(stride == 0 && "should not specify stride of raw buffer");
            assert(byte_offset % 16 && "");
            temp_byte_stride = sizeof(uint32_t);
            temp_format      = PF_UNDEFINED;
        default: assert(false && "unrecognized buffer type");
    }
    assert(byte_offset < info.size && "offset out of bounds of buffer size");
    assert(byte_offset % temp_byte_stride == 0 && "offset must be a multiple of stride");
    temp_byte_offset = byte_offset;
    assert(temp_buffer_type == EBufferType::ACCELERATION_STRUCTURE || (byte_offset == 0 || num_elements > 0) && "");
    temp_num_elements = num_elements == 0 ? (info.size - byte_offset) / temp_byte_stride : num_elements;
    temp_byte_size    = temp_num_elements * temp_byte_stride;

    assert(temp_byte_offset + temp_byte_size < info.size && "total size out of bound");

    //todo platform support query
    assert(temp_format == PF_UNDEFINED || true && "platform not support such pixel format");
    return {
        temp_byte_offset,
        temp_byte_stride,
        temp_num_elements,
        temp_byte_size,
        temp_buffer_type,
        temp_format};
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
    EPixelFormat      temp_format    = format == PF_UNDEFINED ? info.format : format;
    ETextureDimension temp_dimension = dimension;
    //todo check platform support
    assert(temp_format == PF_UNDEFINED || true && "pixel format not supported");

    //b_all_mip is not set, it varies between view types
    return {
        array_min,
        array_num == 0 ? info.array_size : array_num,
        temp_format,
        dimension,
        false,
        mip_num == 0 && mip_min == 0};
}
RHIViewInfo::BufferUAV::ViewInfo RHIViewInfo::BufferUAV::GetViewInfo(RHIBuffer* target) const {
    assert(view_type == EViewType::BUFFER_UAV);
    ViewInfo temp_info         = {Buffer::GetViewInfo(target)};
    temp_info.b_atomic_counter = b_is_atomic_counter;
    temp_info.b_append_buffer  = b_is_append_buffer;
    return temp_info;
}
RHIViewInfo::TextureSRV::ViewInfo RHIViewInfo::TextureSRV::GetViewInfo(RHITexture* target) const {
    assert(view_type == EViewType::TEXTURE_SRV);
    assert(target);
    const auto& tex_info = target->GetInfo();

    ViewInfo temp_info   = {Texture::GetViewInfo(target)};
    temp_info.mip_min    = mip_min;
    temp_info.mip_num    = mip_num == 0 ? tex_info.num_mips : mip_num;
    temp_info.b_all_mips = mip_min == 0 && temp_info.mip_num == tex_info.num_mips;

    return temp_info;
}
RHIViewInfo::TextureUAV::ViewInfo RHIViewInfo::TextureUAV::GetViewInfo(RHITexture* target) const {
    assert(view_type == EViewType::TEXTURE_UAV);
    assert(target);
    const auto& tex_info = target->GetInfo();

    ViewInfo info = {Texture::GetViewInfo(target)};
    assert(tex_info.num_samples == 1 && "cannot create uav on multi-sampled texture");

    info.mip_level = mip_min;
    assert(info.mip_level <= tex_info.num_mips && "mip level out of bounds");

    info.b_all_mips = tex_info.num_mips == 1;
    return info;
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

void RHIBatchedShaderParameters::SetParameters(RHIShader* shader, size_t _data_size, uint8_t* data_source) {
    SetParameters(shader->GetMetaShader(), _data_size, data_source);
}
void RHIBatchedShaderParameters::SetParameters(const Shader* shader, size_t _data_size, uint8_t* data_source) {
    const auto& param_layout_info = shader->GetRootParametersLayoutInfo();

    //not presices since it may contains const data
    uint32_t resource_param_max_size = _data_size >> 3;
    size_t   left_size               = resource_parameters.capacity() - resource_parameters.size();

    if (left_size < resource_param_max_size) resource_parameters.reserve(left_size + resource_parameters.size());
    for (const auto& param_info : param_layout_info.GetLayoutInfos()) {
        uint32_t     stride = param_info.stride;
        uint32_t     num    = stride / sizeof(RHIResource*);

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
    Moer::Array<RHIShaderConstantParameter> temp_constant;
    //copy constant parameters to raw data

    //for now only support one constant struct
    // uint32_t last_offset    = 0;
    // uint32_t current_offset = 0;
    // for (uint32_t i = 0; i < param_layout_info.GetConstantsInfos().size(); ++i) {
    //     const auto& param_info = param_layout_info.GetConstantsInfos()[i];
    //     if (i == 0) {
    //         //first const param should have no gap
    //         current_offset = last_offset = param_info.offset;
    //     } else {
    //         current_offset = param_info.offset;
    //     }
    //     bool b_last_constant = i == param_layout_info.GetConstantsInfos().size() - 1;
    //     bool b_gap           = current_offset != last_offset;

    //     last_offset = current_offset;
    //     //means there's a gap for reserved data
    //     if (b_gap || b_last_constant) {
    //         //get reserved word
    //         uint32_t* reserved = (uint32_t*)&raw_data[b_gap ? last_offset : (current_offset + param_info.stride)];
    //         if (*reserved | STRUCT_DIRTY_BIT) {
    //             //actually dirty, clear temp_constant
    //             temp_constant.clear();
    //         }
    //         uint32_t origin_offset = constant_parameters.size();
    //         temp_constant.resize(origin_offset + current_offset - last_offset);
    //         memcpy(&temp_constant[origin_offset], data_source + last_offset, current_offset - last_offset);
    //     }
    //     //current ptr
    //     uint8_t* data = data_source + current_offset;
    //     //const must set
    //     uint32_t origin_offset = constant_parameters.size();
    //     raw_data.resize(origin_offset + param_info.stride);
    //     memcpy(&raw_data[origin_offset], data_source, param_info.stride);
    //     constant_parameters.emplace_back(shader->GetShaderType(), origin_offset, (param_info.stride + sizeof(uint32_t) - 1) / 4, param_info.slot, param_info.space);
    // }
    for (const auto& param_info : param_layout_info.GetConstantsInfos()) {
        uint8_t* data = data_source + param_info.offset;
        //const must set
        uint32_t origin_offset = raw_data.size();
        raw_data.resize(origin_offset + param_info.stride);
        memcpy(&raw_data[origin_offset], data_source, param_info.stride);
        constant_parameters.emplace_back(shader->GetShaderType(), origin_offset, (param_info.stride + sizeof(uint32_t) - 1) / 4, param_info.slot, param_info.space);
    }
}

RHIBufferRef   RHIRenderPrimitive::GetVertexBuffer() const {
    return m_vertex_buffer;
}
RHIBufferRef   RHIRenderPrimitive::GetIndexBuffer() const {
    return m_index_buffer;
}
EPrimitiveType RHIRenderPrimitive::GetPrimitiveType() const {
    return m_type;
}
uint32_t       RHIRenderPrimitive::GetOffset() const {
    return m_offset;
}
uint32_t       RHIRenderPrimitive::GetCount() const {
    return m_count;
}