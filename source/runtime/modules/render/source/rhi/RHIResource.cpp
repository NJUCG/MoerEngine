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
RHIViewInfo::EBufferType RHIViewInfo::GetBufferType(RHIBuffer* buffer) {
    const auto& info = buffer->GetInfo();
    if (info.IsNull()) return EBufferType::RAW;
    if (EnumHasAnyFlag(info.usage, EBufferUsageFlags::STORAGE_BUFFER)) return EBufferType::STRUCTURED;
    if (EnumHasAnyFlag(info.usage, EBufferUsageFlags::ACCELERATION_STRUCTURE)) return EBufferType::ACCELERATION_STRUCTURE;
    return EBufferType::RAW;
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

void RHIBatchedShaderParameters::SetParameters(RHIShader* shader, size_t _data_size, uint8_t* data_source) {
    SetParameters(shader->GetMetaShader(), _data_size, data_source);
}
RHIBatchedShaderParameters::~RHIBatchedShaderParameters() {
}
void RHIBatchedShaderParameters::SetParameters(const Shader* shader, size_t _data_size, uint8_t* data_source) {
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
    if (param_layout_info.GetConstantsInfo().size() > 0) {
        raw_data.clear();
        constant_parameters.clear();
    }
    for (const auto& param_info : param_layout_info.GetConstantsInfo()) {
        uint8_t* data = data_source + param_info.offset;
        //const must set
        uint32_t origin_offset = raw_data.size();
        raw_data.resize(origin_offset + param_info.stride);
        memcpy(&raw_data[origin_offset], data_source, param_info.stride);
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