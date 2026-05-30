#pragma once

#include "RTResource.h"
#include "rendergraph/RenderGraph.h"
#include "misc/STL.h"

namespace Moer::Render::Raytracing {

static constexpr ERGPassFlags kRTGraphComputePassFlags = ERGPassFlags::Graphics | ERGPassFlags::ComputeShader;

struct RTGraphFrameResources {
    bool current_frame{};

    RGTexture* view_depth{};
    RGTexture* diffuse_albedo{};
    RGTexture* specular_roughness{};
    RGTexture* normal{};
    RGTexture* emission{};
    RGTexture* motion{};
    RGTexture* clip_depth{};
    RGTexture* prev_view_depth{};
    RGTexture* prev_diffuse_albedo{};
    RGTexture* prev_specular_roughness{};
    RGTexture* prev_normal{};
    RGTexture* normal_roughness{};
    RGTexture* diffuse_lighting{};
    RGTexture* prev_diffuse_lighting{};
    RGTexture* specular_lighting{};
    RGTexture* prev_specular_lighting{};
    RGTexture* temporal_sample_pos{};
    RGTexture* gradients{};
    RGTexture* restir_luminance{};
    RGTexture* prev_luminance{};
    RGTexture* denoised_diffuse_lighting{};
    RGTexture* denoised_specular_lighting{};
    RGTexture* debug_color{};
    RGTexture* ldr_color{};
    RGTexture* hdr_color{};
    RGTexture* feedback_color_ping{};
    RGTexture* feedback_color_pong{};
    RGTexture* resolved_color{};
    RGTexture* local_light_pdf_tex{};
    RGTexture* env_pdf_tex{};

    RGBuffer* light_mapping_buf{};
    RGBuffer* prim_light_buf{};
    RGBuffer* task_buf{};
    RGBuffer* primitive_to_light_buf{};
    RGBuffer* light_data_buf{};
    RGBuffer* ris_buf{};
    RGBuffer* ris_light_data_buf{};
    RGBuffer* neighbor_offset_buf{};
    RGBuffer* light_reservoir_buf{};

    RGTexture* current_view_depth{};
    RGTexture* current_diffuse_albedo{};
    RGTexture* current_specular_roughness{};
    RGTexture* current_normal{};
    RGTexture* current_restir_luminance{};
    RGTexture* previous_view_depth{};
    RGTexture* previous_diffuse_albedo{};
    RGTexture* previous_specular_roughness{};
    RGTexture* previous_normal{};
};

inline RGTexture* ImportRTTextureIfValid(RenderGraph& graph, StringView name, const TextureRef& texture) {
    return texture ? graph.ImportTexture(name, texture, EQueueType::Graphics) : nullptr;
}

inline RGBuffer* ImportRTBufferIfValid(RenderGraph& graph, StringView name, const BufferRef& buffer) {
    return buffer ? graph.ImportBuffer(name, buffer, EQueueType::Graphics) : nullptr;
}

inline BufferRef RTUnderlyingBuffer(const RaytracingGeometryRef& geometry) {
    Buffer* buffer = geometry ? geometry->GetUnderlyingBuffer() : nullptr;
    return buffer != nullptr ? BufferRef(buffer) : BufferRef{};
}

inline BufferRef RTUnderlyingBuffer(const RaytracingTlasRef& tlas) {
    Buffer* buffer = tlas ? tlas->GetUnderlyingBuffer() : nullptr;
    return buffer != nullptr ? BufferRef(buffer) : BufferRef{};
}

inline RGBuffer* ImportRTGeometryBufferIfValid(RenderGraph& graph, StringView name, const RaytracingGeometryRef& geometry) {
    return ImportRTBufferIfValid(graph, name, RTUnderlyingBuffer(geometry));
}

inline RGBuffer* ImportRTTlasBufferIfValid(RenderGraph& graph, StringView name, const RaytracingTlasRef& tlas) {
    return ImportRTBufferIfValid(graph, name, RTUnderlyingBuffer(tlas));
}

inline RGTexture* RTCurrentFrameTexture(bool current_frame, RGTexture* current, RGTexture* previous) {
    return current_frame ? current : previous;
}

inline RGTextureView RTWholeTextureView(RGTexture* texture) {
    if (texture == nullptr) {
        return {};
    }
    const RGTextureDesc& desc = texture->Desc();
    return RGTextureView{
        .texture = texture,
        .range   = RGTextureRange{
            .aspect      = desc.aspect_flags == ETextureAspectFlags::NONE ? ETextureAspectFlags::COLOR : desc.aspect_flags,
            .mip_min     = 0,
            .mip_count   = desc.num_mips == 0 ? 1u : static_cast<uint32_t>(desc.num_mips),
            .array_min   = 0,
            .array_count = desc.array_size == 0 ? 1u : static_cast<uint32_t>(desc.array_size)
        }
    };
}

inline RTGraphFrameResources ImportRTGraphFrameResources(RenderGraph& graph, const RTContext& rt_ctx) {
    const FrameResources& frame_rt = rt_ctx.frame_rt;
    RTGraphFrameResources resources{
        .current_frame = rt_ctx.b_current_frame,
        .view_depth = ImportRTTextureIfValid(graph, MOER_TEXT("RT.view_depth"), frame_rt.view_depth),
        .diffuse_albedo = ImportRTTextureIfValid(graph, MOER_TEXT("RT.diffuse_albedo"), frame_rt.diffuse_albedo),
        .specular_roughness = ImportRTTextureIfValid(graph, MOER_TEXT("RT.specular_roughness"), frame_rt.specular_roughness),
        .normal = ImportRTTextureIfValid(graph, MOER_TEXT("RT.normal"), frame_rt.normal),
        .emission = ImportRTTextureIfValid(graph, MOER_TEXT("RT.emission"), frame_rt.emission),
        .motion = ImportRTTextureIfValid(graph, MOER_TEXT("RT.motion"), frame_rt.motion),
        .clip_depth = ImportRTTextureIfValid(graph, MOER_TEXT("RT.clip_depth"), frame_rt.clip_depth),
        .prev_view_depth = ImportRTTextureIfValid(graph, MOER_TEXT("RT.prev_view_depth"), frame_rt.prev_view_depth),
        .prev_diffuse_albedo = ImportRTTextureIfValid(graph, MOER_TEXT("RT.prev_diffuse_albedo"), frame_rt.prev_diffuse_albedo),
        .prev_specular_roughness = ImportRTTextureIfValid(graph, MOER_TEXT("RT.prev_specular_roughness"), frame_rt.prev_specular_roughness),
        .prev_normal = ImportRTTextureIfValid(graph, MOER_TEXT("RT.prev_normal"), frame_rt.prev_normal),
        .normal_roughness = ImportRTTextureIfValid(graph, MOER_TEXT("RT.normal_roughness"), frame_rt.normal_roughness),
        .diffuse_lighting = ImportRTTextureIfValid(graph, MOER_TEXT("RT.diffuse_lighting"), frame_rt.diffuse_lighting),
        .prev_diffuse_lighting = ImportRTTextureIfValid(graph, MOER_TEXT("RT.prev_diffuse_lighting"), frame_rt.prev_diffuse_lighting),
        .specular_lighting = ImportRTTextureIfValid(graph, MOER_TEXT("RT.specular_lighting"), frame_rt.specular_lighting),
        .prev_specular_lighting = ImportRTTextureIfValid(graph, MOER_TEXT("RT.prev_specular_lighting"), frame_rt.prev_specular_lighting),
        .temporal_sample_pos = ImportRTTextureIfValid(graph, MOER_TEXT("RT.temporal_sample_pos"), frame_rt.temporal_sample_pos),
        .gradients = ImportRTTextureIfValid(graph, MOER_TEXT("RT.gradients"), frame_rt.gradients),
        .restir_luminance = ImportRTTextureIfValid(graph, MOER_TEXT("RT.restir_luminance"), frame_rt.restir_luminance),
        .prev_luminance = ImportRTTextureIfValid(graph, MOER_TEXT("RT.prev_luminance"), frame_rt.prev_luminance),
        .denoised_diffuse_lighting = ImportRTTextureIfValid(graph, MOER_TEXT("RT.denoised_diffuse_lighting"), frame_rt.denoised_diffuse_lighting),
        .denoised_specular_lighting = ImportRTTextureIfValid(graph, MOER_TEXT("RT.denoised_specular_lighting"), frame_rt.denoised_specular_lighting),
        .debug_color = ImportRTTextureIfValid(graph, MOER_TEXT("RT.debug_color"), frame_rt.debug_color),
        .ldr_color = ImportRTTextureIfValid(graph, MOER_TEXT("RT.ldr_color"), frame_rt.ldr_color),
        .hdr_color = ImportRTTextureIfValid(graph, MOER_TEXT("RT.hdr_color"), frame_rt.hdr_color),
        .feedback_color_ping = ImportRTTextureIfValid(graph, MOER_TEXT("RT.feedback_color_ping"), frame_rt.feedback_color_ping),
        .feedback_color_pong = ImportRTTextureIfValid(graph, MOER_TEXT("RT.feedback_color_pong"), frame_rt.feedback_color_pong),
        .resolved_color = ImportRTTextureIfValid(graph, MOER_TEXT("RT.resolved_color"), frame_rt.resolved_color),
        .local_light_pdf_tex = ImportRTTextureIfValid(graph, MOER_TEXT("RT.local_light_pdf_tex"), rt_ctx.local_light_pdf_tex),
        .env_pdf_tex = ImportRTTextureIfValid(graph, MOER_TEXT("RT.env_pdf_tex"), rt_ctx.env_pdf_tex),
        .light_mapping_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.light_mapping_buf"), rt_ctx.light_mapping_buf),
        .prim_light_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.prim_light_buf"), rt_ctx.prim_light_buf),
        .task_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.task_buf"), rt_ctx.task_buf),
        .primitive_to_light_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.primitive_to_light_buf"), rt_ctx.primitive_to_light_buf),
        .light_data_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.light_data_buf"), rt_ctx.light_data_buf),
        .ris_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.ris_buf"), rt_ctx.ris_buf),
        .ris_light_data_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.ris_light_data_buf"), rt_ctx.ris_light_data_buf),
        .neighbor_offset_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.neighbor_offset_buf"), rt_ctx.neighbor_offset_buf),
        .light_reservoir_buf = ImportRTBufferIfValid(graph, MOER_TEXT("RT.light_reservoir_buf"), rt_ctx.light_reservoir_buf)
    };

    resources.current_view_depth = RTCurrentFrameTexture(resources.current_frame, resources.view_depth, resources.prev_view_depth);
    resources.current_diffuse_albedo = RTCurrentFrameTexture(resources.current_frame, resources.diffuse_albedo, resources.prev_diffuse_albedo);
    resources.current_specular_roughness = RTCurrentFrameTexture(resources.current_frame, resources.specular_roughness, resources.prev_specular_roughness);
    resources.current_normal = RTCurrentFrameTexture(resources.current_frame, resources.normal, resources.prev_normal);
    resources.current_restir_luminance = RTCurrentFrameTexture(resources.current_frame, resources.restir_luminance, resources.prev_luminance);
    resources.previous_view_depth = RTCurrentFrameTexture(resources.current_frame, resources.prev_view_depth, resources.view_depth);
    resources.previous_diffuse_albedo = RTCurrentFrameTexture(resources.current_frame, resources.prev_diffuse_albedo, resources.diffuse_albedo);
    resources.previous_specular_roughness = RTCurrentFrameTexture(resources.current_frame, resources.prev_specular_roughness, resources.specular_roughness);
    resources.previous_normal = RTCurrentFrameTexture(resources.current_frame, resources.prev_normal, resources.normal);
    return resources;
}

inline RGTexture* ImportedRTFeedbackTexture(
    const RTGraphFrameResources& resources,
    const RTContext&             rt_ctx,
    const TextureRef&            texture
) {
    if (texture == rt_ctx.frame_rt.feedback_color_ping) {
        return resources.feedback_color_ping;
    }
    assert(texture == rt_ctx.frame_rt.feedback_color_pong);
    return resources.feedback_color_pong;
}

inline RGTexture* RTGraphTextureForFrameTexture(
    const RTGraphFrameResources& resources,
    const RTContext&             rt_ctx,
    const TextureRef&            texture
) {
    if (!texture) {
        return nullptr;
    }

    const FrameResources& frame_rt = rt_ctx.frame_rt;
    if (texture == frame_rt.view_depth) {
        return resources.view_depth;
    }
    if (texture == frame_rt.diffuse_albedo) {
        return resources.diffuse_albedo;
    }
    if (texture == frame_rt.specular_roughness) {
        return resources.specular_roughness;
    }
    if (texture == frame_rt.normal) {
        return resources.normal;
    }
    if (texture == frame_rt.emission) {
        return resources.emission;
    }
    if (texture == frame_rt.motion) {
        return resources.motion;
    }
    if (texture == frame_rt.clip_depth) {
        return resources.clip_depth;
    }
    if (texture == frame_rt.prev_view_depth) {
        return resources.prev_view_depth;
    }
    if (texture == frame_rt.prev_diffuse_albedo) {
        return resources.prev_diffuse_albedo;
    }
    if (texture == frame_rt.prev_specular_roughness) {
        return resources.prev_specular_roughness;
    }
    if (texture == frame_rt.prev_normal) {
        return resources.prev_normal;
    }
    if (texture == frame_rt.normal_roughness) {
        return resources.normal_roughness;
    }
    if (texture == frame_rt.diffuse_lighting) {
        return resources.diffuse_lighting;
    }
    if (texture == frame_rt.prev_diffuse_lighting) {
        return resources.prev_diffuse_lighting;
    }
    if (texture == frame_rt.specular_lighting) {
        return resources.specular_lighting;
    }
    if (texture == frame_rt.prev_specular_lighting) {
        return resources.prev_specular_lighting;
    }
    if (texture == frame_rt.temporal_sample_pos) {
        return resources.temporal_sample_pos;
    }
    if (texture == frame_rt.gradients) {
        return resources.gradients;
    }
    if (texture == frame_rt.restir_luminance) {
        return resources.restir_luminance;
    }
    if (texture == frame_rt.prev_luminance) {
        return resources.prev_luminance;
    }
    if (texture == frame_rt.denoised_diffuse_lighting) {
        return resources.denoised_diffuse_lighting;
    }
    if (texture == frame_rt.denoised_specular_lighting) {
        return resources.denoised_specular_lighting;
    }
    if (texture == frame_rt.debug_color) {
        return resources.debug_color;
    }
    if (texture == frame_rt.ldr_color) {
        return resources.ldr_color;
    }
    if (texture == frame_rt.hdr_color) {
        return resources.hdr_color;
    }
    if (texture == frame_rt.feedback_color_ping) {
        return resources.feedback_color_ping;
    }
    if (texture == frame_rt.feedback_color_pong) {
        return resources.feedback_color_pong;
    }
    if (texture == frame_rt.resolved_color) {
        return resources.resolved_color;
    }
    if (texture == rt_ctx.local_light_pdf_tex) {
        return resources.local_light_pdf_tex;
    }
    if (texture == rt_ctx.env_pdf_tex) {
        return resources.env_pdf_tex;
    }

    assert(false && "Texture is not part of the imported raytracing graph frame resources");
    return nullptr;
}

} // namespace Moer::Render::Raytracing
