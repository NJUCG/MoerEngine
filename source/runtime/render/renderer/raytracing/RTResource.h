#ifndef MOER_RT_RESOURCE_H
#define MOER_RT_RESOURCE_H

#include "Configs.h"
#include "ShaderUtils.h"
#include "misc/STL.h"
#include "renderer/common/RuntimeAssets.h"
#include "rhi/RHIResource.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/utils/ShaderParameters.h"
#include <filesystem>
#include <string_view>

#include "scene/GpuScene.h"

#ifndef WITH_NRD
#define WITH_NRD 0
#endif

namespace Moer::Render::Raytracing {

struct FrameResources {
    TextureRef view_depth;
    TextureRef diffuse_albedo;
    TextureRef specular_roughness;
    TextureRef normal;
    TextureRef emission;
    TextureRef motion;
    TextureRef clip_depth;

    TextureRef prev_view_depth;
    TextureRef prev_diffuse_albedo;
    TextureRef prev_specular_roughness;
    TextureRef prev_normal;

    TextureRef normal_roughness; // for denoising
    TextureRef diffuse_lighting;
    TextureRef prev_diffuse_lighting;
    TextureRef specular_lighting;
    TextureRef prev_specular_lighting;
    TextureRef temporal_sample_pos;
    TextureRef gradients;
    TextureRef restir_luminance;
    TextureRef prev_luminance;
    TextureRef denoised_diffuse_lighting;
    TextureRef denoised_specular_lighting;

    TextureRef debug_color;
    TextureRef ldr_color;
    TextureRef hdr_color;
    TextureRef feedback_color_ping;
    TextureRef feedback_color_pong;
    TextureRef resolved_color;
};

struct DefaultResources {
    TextureRef black_tex;
    TextureRef white_tex;
};

struct RTContext {

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

    void FillLowDiscrepancySequence(CommandList& _cmd_list);

    void CreateEnvMapResources(TextureWithHandle _env_map, CommandList& _cmd_list);

    // Create light sampling buffers
    void CreateBuffersIfNeeded(
        uint _num_emissive_meshes,
        uint _num_emissive_triangles,
        uint _num_prim_lights,
        uint _num_geom_instance
    );

    void Tick(Camera& _camera, float2 _jitter);
    void AdvanceFrame();

    void SetEnvMapInfos(float _scale, float _rotation);

    const RaytracingBindlessHandles& GetBindlessHandles() const {
        return bindless_handles;
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

    BufferRef geo_instance_to_light_buf;
    BufferRef light_mapping_buf;
    BufferRef prim_light_buf;
    BufferRef task_buf;
    // polymorphic light info
    BufferRef light_data_buf;

    BufferRef ris_buf;
    BufferRef ris_light_data_buf;

    BufferRef neighbor_offset_buf; // for spatial resampling
    bool      b_has_neighbor_offset = false;

    BufferRef light_reservoir_buf;

    TextureRef env_map = nullptr;

    TextureRef         env_pdf_tex;
    Array<TextureView> env_pdf_mips;
    TextureRef         local_light_pdf_tex;
    Array<TextureView> local_light_pdf_mips;

    uint max_emissive_meshes;
    uint max_emissive_triangles;
    uint max_geom_instance;
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