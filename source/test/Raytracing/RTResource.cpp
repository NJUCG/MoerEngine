#include "RTResource.h"
#include "PixelFormat.h"
#include "PreprocessLightPass.h"
#include "config/ConfigManager.h"
#include <cstdio>
#include <filesystem>
#include <stb_image.h>
#include "math/Function.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

#include "rhi/RHICommon.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "tinyexr.h"

namespace Moer::Render {
    RTResource::RTResource(const std::filesystem::path& _resouce_path)
        : b_loaded(false), resource_path(_resouce_path) {
        //check valid path
        if (!std::filesystem::exists(_resouce_path)) {
            resource_path = ConfigManager::GetInstance().GetEditorResourcePath();
        }
    }

    RTResource::~RTResource() {
    }

    void RTResource::LoadResources() {
        //load resources

        //textures
        Array<ExportTexture> exp_textures;
        {
            auto texture_path = resource_path / "textures";

            auto register_image = [this](TextureRef _tex, const std::string& _name) {
                //register image
                auto& image = this->textures[_name];
                image       = _tex;
            };

            CommandList cmd_list;
            if (std::filesystem::exists(texture_path)) {
                for (auto& entry : std::filesystem::directory_iterator(texture_path)) {
                    LOG_INFO("Load texture {}", entry.path().string());
                    if (entry.path().extension() == ".png") {
                        FILE* file = nullptr;
                        fopen_s(&file, entry.path().string().c_str(), "rb");
                        int width, height, channels;
                        if (file) {
                            ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

                            TextureRef texture = RenderDevice::Get().CreateTexture(
                                entry.path().filename().string(),
                                Extent2D(width, height),
                                PF_R8G8B8A8_UNORM,
                                ETextureUsageFlags::SAMPLED);
                            exp_textures.emplace_back(texture, ETextureState::SAMPLE);
                            cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4), texture);
                            cmd_list.AddCallback([data]() {
                                stbi_image_free(data);
                            });
                            register_image(texture, entry.path().filename().string());
                        }
                    }

                    else if (entry.path().extension() == ".exr") {
                        //load exr
                        int         width = 0, height = 0, channels = 0;
                        float*      data = nullptr;
                        const char* err  = nullptr;
                        auto        ret  = LoadEXR(&data, &width, &height, entry.path().string().c_str(), &err);
                        if (ret != TINYEXR_SUCCESS) {
                            if (err) {
                                fprintf(stderr, "ERR : %s\n", err);
                                FreeEXRErrorMessage(err);// release memory of error message.
                            }
                        }
                        TextureRef texture = RenderDevice::Get().CreateTexture(
                            entry.path().filename().string(),
                            Extent2D(width, height),
                            PF_R32G32B32A32_SFLOAT,
                            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                            10);
                        exp_textures.emplace_back(texture, ETextureState::SAMPLE);

                        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4 * sizeof(float)), texture);
                        cmd_list.AddCallback([data]() {
                            free(data);
                        });
                        register_image(texture, entry.path().filename().string());
                        default_env_map_name = textures.find(entry.path().filename().string())->first;
                    }
                }
            }
            RenderDevice& device = RenderDevice::Get();
            // auto          sync_time = device.GetCopyQueue().Execute(cmd_list.Submit());
            // device.GetCopyQueue().Sync(sync_time.timeline);

            cmd_list.ExportTextureToQueue(EQueueType::Graphics, std::move(exp_textures));

            auto sync_time = device.GetCopyQueue().Execute(cmd_list.Submit());
            device.GetCopyQueue().Sync(sync_time.timeline);
        }
    }

    TextureRef RTResource::GetDefaultEnvMap() {
        return GetTexture(default_env_map_name);
    }

    void RTResource::UnloadResources() {
        //unload resources
    }

    TextureRef RTResource::GetTexture(std::string_view _name) const {
        auto it = textures.find(std::string(_name));
        if (it != textures.end()) {
            return it->second;
        }
        return nullptr;
    }

    BufferRef RTResource::GetBuffer(std::string_view _name) const {
        return nullptr;
    }

    RTContext::RTContext(uint  _num_emissive_meshes,
                         uint  _num_emissive_triangles,
                         uint  _num_prim_lights,
                         uint  _num_geom_instance,
                         uint2 _env_map_extent) : max_emissive_meshes(_num_emissive_meshes),
                                                  max_emissive_triangles(_num_emissive_triangles),
                                                  max_geom_instance(_num_geom_instance),
                                                  max_prim_lights(_num_prim_lights) {
        RenderDevice& device = RenderDevice::Get();
        task_buf             = device.CreateBuffer<PrepareLightsTask>(max_emissive_meshes + max_prim_lights, EBufferUsageFlags::UNORDERED_ACCESS);

        geo_instance_to_light_buf = device.CreateBuffer<uint>(max_geom_instance, EBufferUsageFlags::UNORDERED_ACCESS);

        uint max_local_lights  = max_emissive_triangles + max_prim_lights;
        uint light_buf_element = max_local_lights * 2;

        light_mapping_buf = device.CreateBuffer<uint>(light_buf_element, EBufferUsageFlags::UNORDERED_ACCESS);
        light_data_buf    = device.CreateBuffer<PolymorphicLightInfo>(light_buf_element, EBufferUsageFlags::UNORDERED_ACCESS);
        prim_light_buf    = device.CreateBuffer<PolymorphicLightInfo>(max_prim_lights, EBufferUsageFlags::UNORDERED_ACCESS);

        env_pdf_tex = device.CreateTexture("env_pdf_tex",
                                           Extent2D(_env_map_extent.x, _env_map_extent.y),
                                           PF_R32_UINT,
                                           ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED,
                                           uint(ceilf(log2f(float(std::max(_env_map_extent.x, _env_map_extent.y))))));
        //
        {
            uint texture_width  = RoundUpToPowerOf2(uint(ceil(sqrt(double(light_buf_element)))));
            uint texture_height = RoundUpToPowerOf2(uint(ceil(double(light_buf_element) / texture_width)));
            uint mips           = Max(1u, uint(log2(Max(texture_width, texture_height))) + 1u);

            local_light_pdf_tex = device.CreateTexture("local_light_pdf_tex",
                                                       Extent2D(texture_width, texture_height),
                                                       PF_R32_SFLOAT,
                                                       ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED,
                                                       mips);
        }

        task_buf->SetName("task_buf");
        geo_instance_to_light_buf->SetName("geo_instance_to_light_buf");
        light_mapping_buf->SetName("light_mapping_buf");
        light_data_buf->SetName("light_data_buf");
        prim_light_buf->SetName("prim_light_buf");
    }

    void RTContext::SetBindlessHandles(uint _geom_data_buf_handle, uint _instance_data_buf_handle, uint _material_data_buf_handle) {
        geom_data_buf_handle     = _geom_data_buf_handle;
        instance_data_buf_handle = _instance_data_buf_handle;
        material_data_buf_handle = _material_data_buf_handle;
    }

    void RTContext::FillGBufferResources(uint2 _resolution) {
        RenderDevice& device           = RenderDevice::Get();
        gbuffer_res.view_depth         = device.CreateTexture("view_depth", Extent2D(_resolution), PF_R32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        gbuffer_res.diffuse_albedo     = device.CreateTexture("diffuse_albedo", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        gbuffer_res.specular_roughness = device.CreateTexture("specular_roughness", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        gbuffer_res.normal             = device.CreateTexture("normal", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        gbuffer_res.emission           = device.CreateTexture("emission", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        gbuffer_res.motion             = device.CreateTexture("motion", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        gbuffer_res.clip_depth         = device.CreateTexture("clip_depth", Extent2D(_resolution), PF_R32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
    }

};// namespace Moer::Render