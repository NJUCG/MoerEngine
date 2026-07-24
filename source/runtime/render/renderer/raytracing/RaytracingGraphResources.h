#pragma once

#include "RTResource.h"
#include "rendergraph/RenderGraph.h"

#include <cassert>

namespace Moer::Render::Raytracing {

struct RTGraphFrameResources {
    bool current_frame{};

    RenderGraph::TextureHandle view_depth{};
    RenderGraph::TextureHandle diffuse_albedo{};
    RenderGraph::TextureHandle specular_roughness{};
    RenderGraph::TextureHandle normal{};
    RenderGraph::TextureHandle prev_view_depth{};
    RenderGraph::TextureHandle prev_diffuse_albedo{};
    RenderGraph::TextureHandle prev_specular_roughness{};
    RenderGraph::TextureHandle prev_normal{};

    RenderGraph::TextureHandle emission{};
    RenderGraph::TextureHandle motion{};
    RenderGraph::TextureHandle clip_depth{};
    RenderGraph::TextureHandle normal_roughness{};

    RenderGraph::TextureHandle current_view_depth{};
    RenderGraph::TextureHandle current_diffuse_albedo{};
    RenderGraph::TextureHandle current_specular_roughness{};
    RenderGraph::TextureHandle current_normal{};
    RenderGraph::TextureHandle previous_view_depth{};
    RenderGraph::TextureHandle previous_diffuse_albedo{};
    RenderGraph::TextureHandle previous_specular_roughness{};
    RenderGraph::TextureHandle previous_normal{};
};

inline RenderGraph::TextureAspect RTGraphTextureAspects(const TextureRef& texture) {
    assert(texture);
    RenderGraph::TextureAspect aspects = RenderGraph::TextureAspect::None;
    const auto rhi_aspects = texture->GetAspectFlags();
    if (uint32_t(rhi_aspects & ETextureAspectFlags::COLOR) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Color;
    }
    if (uint32_t(rhi_aspects & ETextureAspectFlags::DEPTH_SLICE) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Depth;
    }
    if (uint32_t(rhi_aspects & ETextureAspectFlags::STENCIL_SLICE) != 0) {
        aspects = aspects | RenderGraph::TextureAspect::Stencil;
    }
    assert(aspects != RenderGraph::TextureAspect::None);
    return aspects;
}

inline RenderGraph::TextureHandle ImportRTGraphTexture(
    RenderGraph&      graph,
    std::string_view  name,
    const TextureRef& texture
) {
    assert(texture);
    return graph.ImportTexture(
        name,
        texture,
        RenderGraph::TextureDesc{
            .mip_count   = texture->GetNumMips(),
            .layer_count = texture->GetNumArray(),
            .aspects     = RTGraphTextureAspects(texture)
        }
    );
}

inline RenderGraph::BufferHandle ImportRTGraphBuffer(
    RenderGraph&     graph,
    std::string_view name,
    const BufferRef& buffer
) {
    assert(buffer);
    return graph.ImportBuffer(
        name,
        buffer,
        RenderGraph::BufferDesc{.byte_size = buffer->GetByteSize()}
    );
}

inline RTGraphFrameResources RegisterRTGraphFrameResources(
    RenderGraph&     graph,
    const RTContext& rt_ctx
) {
    const FrameResources& frame = rt_ctx.frame_rt;
    RTGraphFrameResources resources{
        .current_frame      = rt_ctx.b_current_frame,
        .view_depth         = ImportRTGraphTexture(graph, "RT.view_depth", frame.view_depth),
        .diffuse_albedo     = ImportRTGraphTexture(graph, "RT.diffuse_albedo", frame.diffuse_albedo),
        .specular_roughness =
            ImportRTGraphTexture(graph, "RT.specular_roughness", frame.specular_roughness),
        .normal                 = ImportRTGraphTexture(graph, "RT.normal", frame.normal),
        .prev_view_depth        = ImportRTGraphTexture(graph, "RT.prev_view_depth", frame.prev_view_depth),
        .prev_diffuse_albedo    =
            ImportRTGraphTexture(graph, "RT.prev_diffuse_albedo", frame.prev_diffuse_albedo),
        .prev_specular_roughness =
            ImportRTGraphTexture(graph, "RT.prev_specular_roughness", frame.prev_specular_roughness),
        .prev_normal      = ImportRTGraphTexture(graph, "RT.prev_normal", frame.prev_normal),
        .emission         = ImportRTGraphTexture(graph, "RT.emission", frame.emission),
        .motion           = ImportRTGraphTexture(graph, "RT.motion", frame.motion),
        .clip_depth       = ImportRTGraphTexture(graph, "RT.clip_depth", frame.clip_depth),
        .normal_roughness = ImportRTGraphTexture(graph, "RT.normal_roughness", frame.normal_roughness)
    };

    resources.current_view_depth = resources.current_frame ? resources.view_depth :
                                                             resources.prev_view_depth;
    resources.current_diffuse_albedo = resources.current_frame ? resources.diffuse_albedo :
                                                                resources.prev_diffuse_albedo;
    resources.current_specular_roughness =
        resources.current_frame ? resources.specular_roughness :
                                  resources.prev_specular_roughness;
    resources.current_normal = resources.current_frame ? resources.normal : resources.prev_normal;

    resources.previous_view_depth = resources.current_frame ? resources.prev_view_depth :
                                                              resources.view_depth;
    resources.previous_diffuse_albedo = resources.current_frame ? resources.prev_diffuse_albedo :
                                                                 resources.diffuse_albedo;
    resources.previous_specular_roughness =
        resources.current_frame ? resources.prev_specular_roughness :
                                  resources.specular_roughness;
    resources.previous_normal = resources.current_frame ? resources.prev_normal : resources.normal;
    return resources;
}

} // namespace Moer::Render::Raytracing
