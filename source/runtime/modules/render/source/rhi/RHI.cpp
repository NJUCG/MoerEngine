#include "rhi/RHI.h"
#include "PixelFormat.h"
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "Core.h"

RHI* g_rhi = nullptr;

extern LockFreeQueueBase<RHIResource, 64> pending_deletings;
// global shader
void MTest() {

    EnqueueRenderTask([] {
        // do something
        LOG_INFO("Render System Tick");
    });
}

void RHI::RHIFlushPendingDeletes() {
    Moer::Array<RHIResource*> resources_to_delete;

    pending_deletings.PopAll(resources_to_delete);

    uint32_t num_deletes = resources_to_delete.size();

    //test if its ok
    for (uint32_t i = 0; i < num_deletes; ++i) {
        // LOG_INFO("deleted resource type:{}", uint32_t(resources_to_delete[i]->GetResourceType()));
        //resources_to_delete[i]->GetResourceType() == RRT_VIEWPORT ||
        // if (resources_to_delete[i]->GetResourceType() == RRT_GPU_FENCE) {
        //     continue;
        // }
        MoerDelete(resources_to_delete[i]);
    }
    if (num_deletes > 0) {
        LOG_INFO("{} resources to delete", num_deletes);
    }
}
RHISRVRef RHI::RHICreateBufferSRV(
    RHIBuffer* _resource,
    uint32_t   stride,
    uint64_t   _byte_size,
    uint64_t   _byte_offset) {
#ifdef _DEBUG
    assert(_resource != nullptr);
#endif
    auto true_stride = stride == 0 ? _resource->GetStride() : stride;
    auto true_size   = _byte_size == 0 ? _resource->GetByteSize() : _byte_size;
    auto create_info = RHIViewInfo::CreateBufferSRVInfo()
                           .SetByteOffset(_byte_offset)
                           .SetStride(true_stride)
                           .SetNumElements((true_size - _byte_offset) / true_stride);
    return RHICreateSRVInner(_resource, create_info);
};
RHIUAVRef RHI::RHICreateBufferUAV(
    RHIBuffer* _resource,
    uint32_t   stride,
    uint64_t   _byte_size,
    uint64_t   _byte_offset) {

    auto true_stride = stride == 0 ? _resource->GetStride() : stride;
    auto true_size   = _byte_size == 0 ? _resource->GetByteSize() : _byte_size;

    auto create_info = RHIViewInfo::CreateBufferUAVInfo()
                           .SetByteOffset(_byte_offset)
                           .SetStride(true_stride)
                           .SetNumElements((true_size - _byte_offset) / true_stride);
    return RHICreateUAVInner(_resource, create_info);
};

RHISRVRef RHI::RHICreateTextureSRV(RHITexture*  _texture,
                                   EPixelFormat _format,
                                   uint32_t     _mip_level,
                                   uint32_t     _mip_levels,
                                   uint32_t     _array_index,
                                   uint32_t     _array_size) {

    auto true_format = _format == PF_UNDEFINED ? _texture->GetFormat() : _format;
    return RHICreateSRVInner(_texture,
                             RHIViewInfo::CreateTextureSRVInfo()
                                 .SetArrayRange(_array_index, _array_size)
                                 .SetMipRange(_mip_level, _mip_levels)
                                 .SetDimension(_texture->GetDimension())
                                 .SetFormat(true_format));
}

RHIUAVRef RHI::RHICreateTextureUAV(RHITexture*  _texture,
                                   EPixelFormat _format,
                                   uint32_t     _mip_level,
                                   uint32_t     _array_index,
                                   uint32_t     _array_size) {
    return RHICreateUAVInner(_texture,
                             RHIViewInfo::CreateTextureUAVInfo()
                                 .SetArrayRange(_array_index, _array_size)
                                 .SetMipLevel(_mip_level)
                                 .SetDimension(_texture->GetDimension())
                                 .SetFormat(_format));
}