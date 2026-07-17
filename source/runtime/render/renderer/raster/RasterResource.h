#pragma once

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/Scene.h"
#include "scene/RenderScene.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include <config/ConfigManager.h>
#include <shader/ShaderResourceManager.h>
#include <stb_image.h>

#include "RasterCompileTimeConstants.h"
#include "RasterGpuCullingResource.h"
#include "RasterTextures.h"
#include "ProbeVolumeResource.h"

#include <cstdint>
#include <utility>

namespace Moer::Render::Raster {

struct PointLightShadowData {
    struct ShadowCubeResource {
        TextureRef  tex; // CubeMap (ArrayLayers=6)
        uint        handle;
        std::string name;
        float       far_plane;  // 存下来，Shader 里做深度线性化时需要用到 (Far)
        float       near_plane; // 存下来 (Near)
    };

    // 支持多个点光源 (例如 4 个)
    static constexpr uint                             MAX_POINT_SHADOWS = 1;
    std::array<ShadowCubeResource, MAX_POINT_SHADOWS> shadow_cubes;
};

struct RasterContext {
public:
    // MARK: Only hold reference
    // 需要确保这些引用在整个生命周期内都是有效的
    RenderDevice&    device;
    ShaderManager&   manager;
    CommandQueue&    gfx_queue;
    BindlessArrayRef bdls;
    CommandList&     cmd_list;
    RenderScene&     render_scene;

    const SceneUpdateBatch* scene_updates = nullptr;

    float frame_time;

    uint2 GetResolution() {
        return uint2(resolution.x, resolution.y);
    }

    // MARK: Hold ownership
    RasterTextures   textures;
    BufferWithHandle lighting_data_buffer; //帧级别光照数据
    LightingData         lighting_data;
    ProbeVolumeResource  probe_volume;

    // Shadow Data
    struct CSMData {
        struct ShadowCacheConfigSnapshot {
            int                                  shadow_map_mode                       = 0;
            int                                  shadow_sampling_mode                  = 0;
            int                                  shadow_csm_num_of_cascades            = 0;
            int                                  shadow_csm_sm_size                    = 0;
            float                                shadow_csm_lerp_factor                = 0.0f;
            float                                shadow_csm_blend_percentage           = 0.0f;
            bool                                 shadow_csm_blend_option               = false;
            float                                shadow_csm_auto_max_cover_ratio_of_camera = 0.0f;
            bool                                 shadow_pcss_enabled                   = false;
            float                                shadow_pcss_light_size_world          = 0.0f;
            bool                                 shadow_cache_enabled                  = false;
            int                                  shadow_cache_disable_first_n_cascades = 0;
            StaticArray<float, CSM_MAX_CASCADES> shadow_csm_cover_ratio_of_camera{};
            StaticArray<float, CSM_MAX_CASCADES> shadow_cache_camera_move_threshold_in_texels{};
        };

        struct ShadowCacheEntry {
            bool     valid = false;
            float4x4 world2shadow_clip{};
            float4   scale_data{};
            float3   snapped_light_space_center = float3(0.f, 0.f, 0.f);
            float    world_units_per_texel      = 0.0f;
            float3   light_direction            = float3(0.f, 0.f, 0.f);
            float    absolute_light_z_min       = 0.0f;
            float    absolute_light_z_max       = 0.0f;
            uint64_t last_update_frame          = 0; // TODO: 后续接入最大陈旧帧数刷新
        };

        float3                                                      light_dir;
        StaticArray<DepthBufferWithHandleAndName, CSM_MAX_CASCADES> shadow_map_textures;
        StaticArray<float4x4, CSM_MAX_CASCADES>                     world2shadow_clip;
        StaticArray<ShadowCacheEntry, CSM_MAX_CASCADES>             shadow_cache_entries{};
        ShadowCacheConfigSnapshot                                   shadow_cache_config_snapshot{};
        bool     shadow_cache_config_snapshot_valid = false;
        uint64_t shadow_cache_frame_counter         = 0;
    } csm_data;

    struct PointShadowData {
        // 定义最大支持的点光源阴影数量
        static constexpr uint MAX_POINT_SHADOWS = 1;

        struct ShadowCube {
            TextureRef  tex;        // CubeMap 资源
            uint        handle = 0; // Bindless Handle
            std::string name;       // Debug Name

            // 存储投影参数，供 Lighting Pass 做深度线性化或 VSM 计算
            float  near_plane = 0.1f;
            float  far_plane  = 100.0f;
            float3 light_pos; // 记录生成阴影时的光源位置
        };

        StaticArray<ShadowCube, MAX_POINT_SHADOWS> shadow_cubes;

        uint active_count = 1;
    } point_shadow_data;

    GpuCullingBuffers gpu_culling_buffers;

    struct HiZData {
        bool     previous_valid = false;
        uint     mip_count      = 0;
        float4x4 previous_view_proj{};
    } hiz_data;

    // RayTracing
    RaytracingSceneRef rt_scene() {
        return render_scene.GetGpuSceneRes().rt_scene;
    }

    const GpuScene::Res& GetGpuSceneRes() const {
        return render_scene.GetGpuSceneRes();
    }

private:
    ubyte* LoadImageData(const std::string& path, int& width, int& height) const {
        FILE* file = nullptr;
        fopen_s(&file, path.c_str(), "rb");

        if (!file) {
            LOG_ERROR("Failed to load texture file: {}", path);
            return nullptr;
        }

        int    channels;
        ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

        if (!data) {
            LOG_ERROR("Failed to decode texture data: {}", path);
            fclose(file);
            return nullptr;
        }

        fclose(file);
        return data;
    }

    void UploadTextureData(
        TextureView        target,
        ubyte*             data,
        int                width,
        int                height,
        const std::string& debug_name
    ) const {
        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4), target, debug_name);
        cmd_list.AddCallback([data]() {
            stbi_image_free(data);
        });
    }

    uint2 resolution;

public:
    // Constructor
    RasterContext(
        RenderDevice&    device,
        ShaderManager&   manager,
        CommandQueue&    gfx_queue,
        BindlessArrayRef bdls,
        CommandList&     cmd_list,
        RenderScene&     render_scene,
        uint2            resolution
    ) :
        device(device),
        manager(manager),
        gfx_queue(gfx_queue),
        bdls(bdls),
        cmd_list(cmd_list),
        render_scene(render_scene),
        resolution(resolution) {

        // textures
        textures = RasterTextures{};

        // other resources
        CreateLightingData();
        probe_volume.Create(device, bdls);
    }

    void Update(float delta_time) {
        frame_time = delta_time;
    }

    void SetResolution(uint2 new_resolution) {
        resolution = new_resolution;
    }

    void BeginSceneFrame(const SceneUpdateBatch& updates) {
        scene_updates = &updates;
    }

    void EndSceneFrame() {
        scene_updates = nullptr;
    }

    const SceneUpdateBatch& GetSceneUpdates() const {
        assert(scene_updates && "Scene frame data is only valid during RasterRenderer::RenderFrame().");
        return *scene_updates;
    }

    void CreateLightingData() {
        //CPU Side 在Render循环中填充数据

        //GPU Side
        lighting_data_buffer.buf = device.CreateBuffer<byte>(
            "Raster::LightData",
            sizeof(LightingData),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::CONSTANT_BUFFER
        );
        lighting_data_buffer.hdl = bdls->AllocateBuffer(lighting_data_buffer.buf->GetView());
    }

    // MARK: Frame Buffers

    void CreateFrameBuffers() {
        textures.CreateFrameBuffers(device, resolution);
        hiz_data.previous_valid = false;
        hiz_data.mip_count      = textures.hiz_current.tex ? textures.hiz_current.tex->GetNumMips() : 0;
    }

    //功能：加载外部纹理并在这一步Create它们的buffer
    void UploadExternalFrameBuffers() {
        textures.LoadAndUploadAssets(device, cmd_list);
    }

    void AllocateFrameBuffers() {
        textures.AllocateFrameBuffers(cmd_list, bdls);
    }

    void FreeFrameBuffers(bool is_free_external_assets) {
        textures.FreeFrameBuffers(bdls, is_free_external_assets);
        hiz_data.previous_valid = false;
        hiz_data.mip_count      = 0;
    }

    void InvalidateHiZHistory() {
        hiz_data.previous_valid = false;
    }

    void CommitHiZHistory(const float4x4& current_view_proj) {
        std::swap(textures.hiz_current, textures.hiz_previous);
        hiz_data.previous_valid   = true;
        hiz_data.previous_view_proj = current_view_proj;
        hiz_data.mip_count        = textures.hiz_previous.tex ? textures.hiz_previous.tex->GetNumMips() : 0;
    }

    Array<TextureView> GetDisplayableFrameBuffersView() {
        return textures.GetDisplayableFrameBuffersView();
    }

    TextureView GetSelectedFrameBufferView(uint selected_frame_buffer_index) {
        return textures.GetDisplayableFrameBuffersView()[selected_frame_buffer_index];
    }
};

} // namespace Moer::Render::Raster
