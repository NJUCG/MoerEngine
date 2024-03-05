#ifndef MOER_ENGINE_RENDER_RESOURCE_DEFERRED_H
#define MOER_ENGINE_RENDER_RESOURCE_DEFERRED_H

#include "misc/STL.h"
#include "rhi/RHIResource.h"
namespace Moer {
    struct RenderResourceDeferred {
        RHIBufferRef packed_vertex_buffer;
        RHIBufferRef packed_index_buffer;

        RHIBufferRef instance_buffer;
        RHIBufferRef meshlet_info_buffer;
        RHIBufferRef instance_mesh_info_buffer;
        RHIBufferRef meshlet_bounds_buffer;

        RHITextureRef g_buffer_albedo;
        RHITextureRef g_buffer_normal;
        RHITextureRef g_buffer_material;

        Moer::Array<RHITextureRef> g_buffer_depth;
        Moer::Array<RHIUAVRef>     g_buffer_depth_uav;
    };
}
#endif