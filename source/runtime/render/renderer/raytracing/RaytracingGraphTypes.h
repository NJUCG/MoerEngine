#pragma once

#include "misc/STL.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHIResource.h"

namespace Moer::Render::Raytracing {

struct RTGraphSceneResources {
    RenderGraph::BufferHandle light{};
    RenderGraph::BufferHandle material{};
    RenderGraph::BufferHandle primitive{};
    RenderGraph::BufferHandle instance{};
    RenderGraph::BufferHandle position{};
    RenderGraph::BufferHandle packed_normal{};
    RenderGraph::BufferHandle packed_tangent{};
    RenderGraph::BufferHandle texcoord0{};
    RenderGraph::BufferHandle index{};
    RenderGraph::BufferHandle rt_instance{};
    RenderGraph::BufferHandle rt_primitive_table{};

    BufferRef light_resource{};
    BufferRef material_resource{};
    BufferRef primitive_resource{};
    BufferRef instance_resource{};
    BufferRef position_resource{};
    BufferRef packed_normal_resource{};
    BufferRef packed_tangent_resource{};
    BufferRef texcoord0_resource{};
    BufferRef index_resource{};
    BufferRef rt_instance_resource{};
    BufferRef rt_primitive_table_resource{};

    Array<RenderGraph::TextureHandle> material_textures;
    Array<TextureRef>                 material_texture_resources;
};

struct RTGraphFrameSetupResources {
    RenderGraph::TokenHandle  ready{};
    RenderGraph::TokenHandle  bindless{};
    RenderGraph::BufferHandle current_tlas{};
    RenderGraph::BufferHandle previous_tlas{};

    RTGraphSceneResources scene{};

    RenderGraph::PassHandle update_bindless{};
    RenderGraph::PassHandle normalize_scene{};
    RenderGraph::PassHandle build_tlas{};
    RenderGraph::PassHandle finalize{};

    [[nodiscard]] bool IsValid() const {
        return ready.IsValid() && bindless.IsValid() &&
               current_tlas.IsValid() && previous_tlas.IsValid() &&
               update_bindless.IsValid() && normalize_scene.IsValid() &&
               finalize.IsValid();
    }
};

} // namespace Moer::Render::Raytracing
