#pragma once

#include "rhi/RHI.h"
#include "scene/Scene.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"

#include "RasterTextures.h"

#include <stb_image.h>

namespace Moer::Render::Raster {

struct RasterContext {
    // MARK: Only hold reference
    // 需要确保这些引用在整个生命周期内都是有效的
    RenderDevice&    device;
    ShaderManager&   manager;
    BindlessArrayRef bdls;
    CommandList&     cmd_list;
    Scene&           scene;
    uint2&           resolution; // Be careful, resolution is also a reference

    // MARK: Hold ownership
    RasterTextures    textures;
    TextureWithHandle noise_tex;

    // Data from scene
    uint gpu_instance_info_handle     = 0;
    uint gpu_geometry_info_handle     = 0;
    uint gpu_geometry_instance_handle = 0;
    uint gpu_material_info_handle     = 0;
    uint gpu_light_info_handle        = 0;

    RasterContext(
        RenderDevice&    device,
        ShaderManager&   manager,
        BindlessArrayRef bdls,
        CommandList&     cmd_list,
        Scene&           scene,
        uint2&           resolution
    ) :
        device(device),
        manager(manager),
        bdls(bdls),
        cmd_list(cmd_list),
        scene(scene),
        resolution(resolution) {

        // textures
        textures = RasterTextures{};

        // other resources
        LoadNoiseTexture();
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
            PF_R8_UNORM,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST
        );

        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * channels), noise_tex.tex);
        cmd_list.AddCallback([data]() { stbi_image_free(data); });

        noise_tex.handle = bdls->AllocateTexture(noise_tex.tex, Sampler(SF_LINEAR, SAM_REPEAT));
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

        static bool first_load = true;

        if (first_load) {
            first_load = false;

            // Textures
            Array<ImportTexture> sampled_textures;
            sampled_textures.reserve((scene.GetGpuScene().material_textures.size()));
            for (auto& [name, tex] : scene.GetGpuScene().material_textures) {
                sampled_textures.emplace_back(
                    ImportTexture(tex.texture->GetView(0, tex.texture->GetNumMips()), ETextureState::SAMPLE)
                );
            }
            cmd_list.ImportTextureFromQueue(EQueueType::Copy, std::move(sampled_textures));
        }

        // Bindless
        cmd_list.UpdateBindlessArray(bdls);
    }

    // MARK: Frame Buffers

    void CreateFrameBuffers() { textures.CreateFrameBuffers(device, resolution); }

    void AllocateFrameBuffers() { textures.AllocateFrameBuffers(cmd_list, bdls); }

    void FreeFrameBuffers() { textures.FreeFrameBuffers(bdls); }

    Array<TextureView> GetDisplayableFrameBuffersView() { return textures.GetDisplayableFrameBuffersView(); }
};

} // namespace Moer::Render