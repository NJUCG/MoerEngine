#pragma once

#include "ProbeVolumeResource.h"
#include "renderer/common/GraphResourceBoundary.h"
#include "rendergraph/RenderGraph.h"
#include "scene/GpuScene.h"
#include <format>

namespace Moer::Render::Raster {

inline RenderGraph::BufferHandle
ImportRasterBuffer(RenderGraph& graph, std::string_view name, Buffer* buffer) {
    const auto handle = graph.ImportBuffer(
        std::format("{}@{:x}", name, reinterpret_cast<uintptr_t>(buffer)),
        BufferRef(buffer),
        {.byte_size = buffer->GetByteSize()}
    );
    graph.SetInitialState(
        handle,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        handle,
        RenderGraph::BufferAccessType::ShaderRead,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    return handle;
}

inline RenderGraph::TextureHandle
ImportRasterTexture(RenderGraph& graph, std::string_view name, Texture* texture) {
    auto       aspects = RenderGraph::TextureAspect::None;
    const auto flags   = texture->GetAspectFlags();
    if (uint32(flags & ETextureAspectFlags::COLOR)) {
        aspects = aspects | RenderGraph::TextureAspect::Color;
    }
    if (uint32(flags & ETextureAspectFlags::DEPTH_SLICE)) {
        aspects = aspects | RenderGraph::TextureAspect::Depth;
    }
    if (uint32(flags & ETextureAspectFlags::STENCIL_SLICE)) {
        aspects = aspects | RenderGraph::TextureAspect::Stencil;
    }
    const auto handle = graph.ImportTexture(
        std::format("{}@{:x}", name, reinterpret_cast<uintptr_t>(texture)),
        TextureRef(texture),
        {.mip_count = texture->GetNumMips(), .layer_count = texture->GetNumArray(), .aspects = aspects}
    );
    const auto boundary = GraphicsTextureBoundary(*texture);
    graph.SetInitialState(handle, boundary, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    graph.Export(handle, boundary, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
    return handle;
}

// A draw's GPU-selected materials may address any resident scene texture. This
// candidate set belongs to the scene, not to the bindless descriptor table, and
// never includes unrelated render targets or writable postprocessing resources.
struct RasterSceneGraphReads {
    std::vector<RenderGraph::BufferHandle>  buffers;
    std::vector<RenderGraph::TextureHandle> textures;

    RasterSceneGraphReads(RenderGraph& graph, const GpuScene::Res& scene) {
        for (const auto* resource :
             {&scene.instance_buf,
              &scene.primitive_buf,
              &scene.position_buf,
              &scene.packed_normal_buf,
              &scene.packed_tangent_buf,
              &scene.texcoord0_buf,
              &scene.material_buf}) {
            if (resource->buf) {
                buffers.push_back(ImportRasterBuffer(graph, "Scene.draw_data", resource->buf.Get()));
            }
        }
        for (const auto& resource : scene.texture_array) {
            if (resource.tex) {
                textures.push_back(ImportRasterTexture(graph, "Scene.material_texture", resource.tex.Get()));
            }
        }
    }

    void Declare(RenderGraph::PassBuilder& builder) const {
        for (auto buffer : buffers) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        }
        for (auto texture : textures) {
            builder.Read(texture, RenderGraph::TextureAccessType::Sampled);
        }
    }
};

struct RasterProbeGraphReads {
    std::vector<RenderGraph::BufferHandle>  buffers;
    std::vector<RenderGraph::TextureHandle> textures;

    RasterProbeGraphReads(RenderGraph& graph, const ProbeVolumeResource& volume) {
        for (auto view :
             {volume.GetProbeBufferView(),
              volume.GetVolumeBufferView(),
              volume.GetBrickBufferView(),
              volume.GetCellBufferView(),
              volume.GetPageTableBufferView(),
              volume.GetVisibilityAtlasBufferView(),
              volume.GetIrradianceAtlasBufferView()}) {
            buffers.push_back(ImportRasterBuffer(graph, "ProbeVolume.data", view.GetBuffer()));
        }
        for (auto view : {volume.GetVisibilityAtlasTextureView(), volume.GetIrradianceAtlasTextureView()}) {
            textures.push_back(ImportRasterTexture(graph, "ProbeVolume.atlas", view.GetTexture()));
        }
    }
    void Declare(RenderGraph::PassBuilder& builder) const {
        for (auto buffer : buffers) {
            builder.Read(buffer, RenderGraph::BufferAccessType::ShaderRead);
        }
        for (auto texture : textures) {
            builder.Read(texture, RenderGraph::TextureAccessType::Sampled);
        }
    }
};

} // namespace Moer::Render::Raster
