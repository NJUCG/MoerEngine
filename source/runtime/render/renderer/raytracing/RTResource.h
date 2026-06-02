#ifndef MOER_RT_RESOURCE_H
#define MOER_RT_RESOURCE_H

#include "Configs.h"
#include "ShaderUtils.h"
#include "misc/STL.h"
#include "renderer/common/RuntimeAssets.h"
#include "rendergraph/RenderGraphResourcePool.h"
#include "rhi/RHIResource.h"
#include "scene/Scene.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/utils/ShaderParameters.h"
#include <filesystem>
#include <string_view>

#include "scene/GpuScene.h"

#include <cassert>

#ifndef WITH_NRD
#define WITH_NRD 0
#endif

namespace Moer::Render::Raytracing {

struct FrameResources {
    PooledTextureRef view_depth;
    PooledTextureRef diffuse_albedo;
    PooledTextureRef specular_roughness;
    PooledTextureRef normal;
    PooledTextureRef emission;
    PooledTextureRef motion;
    PooledTextureRef clip_depth;

    PooledTextureRef prev_view_depth;
    PooledTextureRef prev_diffuse_albedo;
    PooledTextureRef prev_specular_roughness;
    PooledTextureRef prev_normal;

    PooledTextureRef normal_roughness;
    PooledTextureRef diffuse_lighting;
    PooledTextureRef prev_diffuse_lighting;
    PooledTextureRef specular_lighting;
    PooledTextureRef prev_specular_lighting;
    PooledTextureRef temporal_sample_pos;
    PooledTextureRef gradients;
    PooledTextureRef restir_luminance;
    PooledTextureRef prev_luminance;
    PooledTextureRef denoised_diffuse_lighting;
    PooledTextureRef denoised_specular_lighting;

    PooledTextureRef debug_color;
    PooledTextureRef ldr_color;
    PooledTextureRef hdr_color;
    PooledTextureRef feedback_color_ping;
    PooledTextureRef feedback_color_pong;
    PooledTextureRef resolved_color;
};

inline const TextureRef& RTRHI(const PooledTextureRef& texture) {
    assert(texture && texture->IsAllocated());
    return texture->RHI();
}

inline const BufferRef& RTRHI(const PooledBufferRef& buffer) {
    assert(buffer && buffer->IsAllocated());
    return buffer->RHI();
}

struct DefaultResources {
    TextureRef black_tex;
    TextureRef white_tex;
};

struct RTContext {

    struct LowDiscrepancySequenceCommand {
        bool                            enabled = false;
        GenLowDiscrepancySequenceParam param{};
        BufferView                      output{};
    };

    struct Config {
        float4      reblur_diffuse_hit_dist_params;
        float4      reblur_specular_hit_dist_params;
        EFinalColor final_color;
        uint        denoiser_mode;
    };

public:
    RTContext(ShaderUtils& _sd_utils, ImportanceSamplingContext& _is_ctx, BindlessArrayRef _bindless_array);

    void SetBindlessHandles(const GpuScene::Res& gpu_scene_res);

    void FillFrameResources(uint2 _resolution);

    void SetResolution(uint2 _resolution);

    void SetRaytracingScene(RaytracingSceneRef _rt_scene) {
        rt_scene = _rt_scene;
    }

    LowDiscrepancySequenceCommand PrepareLowDiscrepancySequence();
    void RecordLowDiscrepancySequence(CommandList& _cmd_list, const LowDiscrepancySequenceCommand& _command);

    void CreateEnvMapResources(TextureWithHandle _env_map, CommandList& _cmd_list);

    // Create light sampling buffers
    void CreateBuffersIfNeeded(
        uint _num_emissive_meshes,
        uint _num_emissive_triangles,
        uint _num_prim_lights,
        uint _max_primitives
    );

    void Tick(Camera& _camera, float2 _jitter);
    void AdvanceFrame();

    void SetEnvMapInfos(float _scale, float _rotation);

    const RaytracingBindlessHandles& GetBindlessHandles() const {
        return bindless_handles;
    }

    BindlessArrayRef GetBindlessArray() const {
        return bdls;
    }

    void LoadDefaultResources(RuntimeAssets& _rt_res);

    const UnorderedSet<uint>& GetAllocatedBdlsBuf() {
        return allocated_bdls_buf;
    }
    const UnorderedSet<uint>& GetAllocatedBdlsTex() {
        return allocated_bdls_tex;
    }

private:
    void AllocateAndFreeBdlsIfNeeded(uint& _target, const TextureView& _view, Sampler _sampler);
    void AllocateAndFreeBdlsIfNeeded(uint& _target, const BufferView& _view);

    UnorderedSet<uint> allocated_bdls_buf;
    UnorderedSet<uint> allocated_bdls_tex;

public:
    Config            config;
    SceneGlobalParams scene_params{};

    ViewParam main_view{};
    ViewParam prev_view{};

    PooledBufferRef light_mapping_buf;
    PooledBufferRef prim_light_buf;
    PooledBufferRef task_buf;
    PooledBufferRef primitive_to_light_buf;
    // polymorphic light info
    PooledBufferRef light_data_buf;

    PooledBufferRef ris_buf;
    PooledBufferRef ris_light_data_buf;

    PooledBufferRef neighbor_offset_buf;
    bool            b_has_neighbor_offset = false;

    PooledBufferRef light_reservoir_buf;

    TextureRef env_map = nullptr;

    PooledTextureRef   env_pdf_tex;
    Array<TextureView> env_pdf_mips;
    PooledTextureRef   local_light_pdf_tex;
    Array<TextureView> local_light_pdf_mips;

    uint max_emissive_meshes;
    uint max_emissive_triangles;
    uint max_prim_lights;

    FrameResources   frame_rt;
    DefaultResources default_res;

    ImportanceSamplingContext& is_ctx;
    ShaderUtils&               sd_utils;
    bool                       b_parallel_process_light = false;
    uint                       num_threads              = 4;

    RaytracingSceneRef rt_scene;
    bool               b_current_frame = true;

private:
    RaytracingBindlessHandles bindless_handles{};
    BindlessArrayRef          bdls;
};
} // namespace Moer::Render::Raytracing

#endif