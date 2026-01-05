#pragma once

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/Scene.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include <config/ConfigManager.h>
#include <shader/ShaderResourceManager.h>
#include <stb_image.h>

#include "RasterCompileTimeConstants.h"
#include "RasterTextures.h"

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
    Scene&           scene;

    float frame_time;

    // 超分Pass前的分辨率
    uint2 GetResolutionBeforeSR() {
#if WITH_CUDA && SUPER_RESOLUTION_ENABLED
        return uint2(resolution.x / 2.0f, resolution.y / 2.0f);
#else
        return uint2(resolution.x, resolution.y);
#endif
    }
    // 超分Pass后的分辨率（原始分辨率）
    uint2 GetResolutionOriginal() {
        return uint2(resolution.x, resolution.y);
    }

    // MARK: Hold ownership
    RasterTextures    textures;
    TextureWithHandle lut_ggx_emu;  // games202 hw4 Emu
    TextureWithHandle lut_ggx_eavg; // games202 hw4 Eavg
    TextureWithHandle noise_tex;
    TextureWithHandle cubemap_tex;

    // Data from scene
    uint gpu_instance_info_handle     = 0;
    uint gpu_geometry_info_handle     = 0;
    uint gpu_geometry_instance_handle = 0;
    uint gpu_material_info_handle     = 0;
    uint gpu_light_info_handle        = 0;

    // Shadow Data
    struct CSMData {
        float3                                light_dir;
        StaticArray<float4, CSM_MAX_CASCADES> scaleDatas; // x: Width, y: Height, z: ZRange, w: NearPlane
        StaticArray<DepthBufferWithHandleAndName, CSM_MAX_CASCADES> shadow_map_textures;
        StaticArray<float4x4, CSM_MAX_CASCADES>                     world_to_shadow_clip;
        StaticArray<float, CSM_MAX_CASCADES>
            cascade_split_points; //actual split points between near_clip and far_clip
        StaticArray<float, CSM_MAX_CASCADES>
            cascade_split_ratios; //ratios between 0.0 and 1.0 according to near_clip and far_clip
        StaticArray<float, CSM_MAX_CASCADES>
            cascade_blend_start_ratios; //calculated in linear space, then converted to clip space
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

    // RayTracing
    RaytracingSceneRef rt_scene;

private:
    uint2& resolution; // Be careful, resolution is also a reference

public:
    // Constructor
    RasterContext(
        RenderDevice&    device,
        ShaderManager&   manager,
        CommandQueue&    gfx_queue,
        BindlessArrayRef bdls,
        CommandList&     cmd_list,
        Scene&           scene,
        uint2&           resolution
    ) :
        device(device),
        manager(manager),
        gfx_queue(gfx_queue),
        bdls(bdls),
        cmd_list(cmd_list),
        scene(scene),
        resolution(resolution) {

        // rt scene
        rt_scene = device.CreateRaytracingScene();

        // textures
        textures = RasterTextures{};

        // other resources
        LoadLUT();
        LoadNoiseTexture();
        LoadCubemap();
    }

    void Update(float delta_time) {
        frame_time = delta_time;
    }

    // TODO: 考虑统一Skybox，因为写这段代码的时候，LoadSkybox()正在被dragonk修改，所以没有更新LoadSkybox()（如果不需要更新，则删掉本注释）
    void LoadExternalTexture(TextureWithHandle& out_tex, const std::string& path, const char* name) const {
        FILE* file = nullptr;
        fopen_s(&file, path.c_str(), "rb");

        if (!file) {
            LOG_ERROR("Failed to load texture in RasterPipeline");
            return;
        }

        int    width, height, channels;
        ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

        out_tex.tex = device.CreateTexture(
            name,
            Extent2D(width, height),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
        );

        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * channels), out_tex.tex);
        cmd_list.AddCallback([data]() {
            stbi_image_free(data);
        });

        out_tex.handle = bdls->AllocateTexture(out_tex.tex, Sampler(SF_LINEAR, SAM_REPEAT));
    }

    void LoadLUT() {
        {
            std::string filepath =
                (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" / "LUT" / "GGX_E_LUT.png")
                    .string();

            LoadExternalTexture(lut_ggx_emu, filepath, "lut_ggx_emu");
        }
        {
            std::string filepath = (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" /
                                    "LUT" / "GGX_Eavg_LUT.png")
                                       .string();

            LoadExternalTexture(lut_ggx_eavg, filepath, "lut_ggx_eavg");
        }
    }

    void LoadNoiseTexture() {
        {
            std::string filepath =
                (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" / "noise_256x256.png")
                    .string();

            LoadExternalTexture(noise_tex, filepath, "noise_tex");
        }
    }

    void LoadCubemap() {
        const std::array<std::string, 6> skybox_faces = {
            "posx.jpg", "negx.jpg", "posy.jpg", "negy.jpg", "posz.jpg", "negz.jpg"
        };

        // TODO: GUI和Config切换天空盒，而不是现在硬编码
        // const std::string skybox_path = "Shibuya";
        const std::string skybox_path = "WaterScene";

        TextureView skybox_view;

        for (size_t i = 0; i < 6; i++) {
            std::string filepath = (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" /
                                    "Skybox" / skybox_path / skybox_faces[i])
                                       .string();

            FILE* file = nullptr;
            fopen_s(&file, filepath.c_str(), "rb");

            if (!file) {
                LOG_ERROR("Failed to load skybox texture");
                return;
            }

            int    width, height, channels;
            ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

            if (i == 0) { // first time
                // 延迟创建，这样可以读取width & height
                cubemap_tex.tex = device.CreateCubeMap(
                    "Skybox Cubemap",
                    Extent2D(width, height),
                    PF_R8G8B8A8_UNORM,
                    ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
                );
                skybox_view = TextureView(cubemap_tex.tex);
            }

            cmd_list.CopyFrom(
                std::span<Moer::byte>((Moer::byte*)data, width * height * channels),
                skybox_view.Slice(i),
                std::format("Skybox Cubemap #{}", i)
            );

            cmd_list.AddCallback([data]() {
                stbi_image_free(data);
            });
        }
        cubemap_tex.handle = bdls->AllocateTexture(cubemap_tex.tex, Sampler(SF_LINEAR, SAM_REPEAT));
    }

    // Called from `FirstLoad`
    void LoadSceneData() {
        assert(
            Scene::GetCurrentSceneLoadInfo().Get() && Scene::GetCurrentSceneLoadInfo()->IsReady() &&
            "Scene not ready, but called LoadSceneData"
        );

        gpu_instance_info_handle =
            bdls->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::InstanceInfo)->GetView());
        gpu_geometry_info_handle =
            bdls->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::GeometryInfo)->GetView());
        gpu_geometry_instance_handle =
            bdls->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::GeometryInstance)->GetView());
        gpu_material_info_handle =
            bdls->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::MaterialInfo)->GetView());
        gpu_light_info_handle =
            bdls->AllocateBuffer(scene.GetBuffer(EGpuSceneResource::LightInfo)->GetView());

        // Bindless
        cmd_list.UpdateBindlessArray(bdls);
    }

    // MARK: Frame Buffers

    void CreateFrameBuffers() {
        textures.CreateFrameBuffers(device, resolution);
    }

    void AllocateFrameBuffers() {
        textures.AllocateFrameBuffers(cmd_list, bdls);
    }

    void FreeFrameBuffers() {
        textures.FreeFrameBuffers(bdls);
    }

    Array<TextureView> GetDisplayableFrameBuffersView() {
        return textures.GetDisplayableFrameBuffersView();
    }

    TextureView GetSelectedFrameBufferView(uint selected_frame_buffer_index) {
        return textures.GetDisplayableFrameBuffersView()[selected_frame_buffer_index];
    }
};

} // namespace Moer::Render::Raster