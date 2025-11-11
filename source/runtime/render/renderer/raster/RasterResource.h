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

    // 超分Pass前的分辨率
    uint2 GetResolutionBeforeSR() {
#if WITH_CUDA && SUPER_RESOLUTION_ENABLED
        return uint2(resolution->x / 2.0f, resolution->y / 2.0f);
#else
        return uint2(resolution->x, resolution->y);
#endif
    }
    // 超分Pass后的分辨率（原始分辨率）
    uint2 GetResolutionOriginal() {
        return uint2(resolution->x, resolution->y);
    }

    // MARK: Hold ownership
    RasterTextures    textures;
    TextureWithHandle noise_tex;
    TextureWithHandle skybox_tex[6];

    // Data from scene
    uint gpu_instance_info_handle     = 0;
    uint gpu_geometry_info_handle     = 0;
    uint gpu_geometry_instance_handle = 0;
    uint gpu_material_info_handle     = 0;
    uint gpu_light_info_handle        = 0;

    // Shadow Data
    StaticArray<DepthBufferWithHandleAndName, CSM_MAX_CASCADES> shadow_map_textures;
    StaticArray<float4x4, CSM_MAX_CASCADES>                     world_to_shadow_clip;

    // RayTracing
    RaytracingSceneRef rt_scene;

private:
    SharedPtr<uint2> resolution; // Be careful, resolution is also a reference

public:
    // Constructor
    RasterContext(
        RenderDevice&    device,
        ShaderManager&   manager,
        CommandQueue&    gfx_queue,
        BindlessArrayRef bdls,
        CommandList&     cmd_list,
        Scene&           scene,
        SharedPtr<uint2> resolution
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
        LoadNoiseTexture();
        LoadSkybox();
    }

    void LoadNoiseTexture() {
        std::string filepath =
            (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" / "noise_256x256.png")
                .string();

        FILE* file = nullptr;
        fopen_s(&file, filepath.c_str(), "rb");

        if (!file) {
            LOG_ERROR("Failed to load noise texture");
            return;
        }

        int    width, height, channels;
        ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

        noise_tex.tex = device.CreateTexture(
            "noise_tex",
            Extent2D(width, height),
            PF_R8G8B8A8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
        );

        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * channels), noise_tex.tex);
        cmd_list.AddCallback([data]() {
            stbi_image_free(data);
        });

        noise_tex.handle = bdls->AllocateTexture(noise_tex.tex, Sampler(SF_LINEAR, SAM_REPEAT));
    }

    void LoadSkybox() {
        const std::array<std::string, 6> skybox_faces = {
            "skybox_posz.jpg",
            "skybox_negz.jpg",
            "skybox_posy.jpg",
            "skybox_negy.jpg",
            "skybox_posx.jpg",
            "skybox_negx.jpg"
        };
        for (size_t i = 0; i < 6; i++) {
            std::string filepath =
                (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" / skybox_faces[i])
                    .string();

            FILE* file = nullptr;
            fopen_s(&file, filepath.c_str(), "rb");

            if (!file) {
                LOG_ERROR("Failed to load skybox texture");
                return;
            }

            int    width, height, channels;
            ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

            skybox_tex[i].tex = device.CreateTexture(
                "skybox_tex" + std::to_string(i),
                Extent2D(width, height),
                PF_R8G8B8A8_UNORM,
                ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
            );

            cmd_list.CopyFrom(
                std::span<Moer::byte>((Moer::byte*)data, width * height * channels), skybox_tex[i].tex
            );
            cmd_list.AddCallback([data]() {
                stbi_image_free(data);
            });

            skybox_tex[i].handle = bdls->AllocateTexture(skybox_tex[i].tex, Sampler(SF_LINEAR, SAM_REPEAT));
        }
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