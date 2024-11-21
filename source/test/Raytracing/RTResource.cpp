#include "RTResource.h"
#include "config/ConfigManager.h"
#include <cstdio>
#include <filesystem>
#include <stb_image.h>
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

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
                                Extent2D(width, height),
                                PF_R8G8B8A8_UNORM,
                                ETextureUsageFlags::SAMPLED);

                            cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4), texture);
                            cmd_list.AddCallback([data]() {
                                stbi_image_free(data);
                            });
                            register_image(texture, entry.path().filename().string());
                        }
                    }

                    else if (entry.path().extension() == ".exr") {
                        //load exr
                        int        width, height, channels;
                        float*     data    = stbi_loadf(entry.path().string().c_str(), &width, &height, &channels, 4);
                        TextureRef texture = RenderDevice::Get().CreateTexture(
                            Extent2D(width, height),
                            PF_R32G32B32A32_SFLOAT,
                            ETextureUsageFlags::SAMPLED);

                        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4 * sizeof(float)), texture);
                        cmd_list.AddCallback([data]() {
                            stbi_image_free(data);
                        });
                        register_image(texture, entry.path().filename().string());
                    }
                }
            }
            RenderDevice& device    = RenderDevice::Get();
            auto          sync_time = device.GetCopyQueue().Execute(cmd_list.Submit());

            device.GetCopyQueue().Sync(sync_time.timeline);
        }
    }

    void RTResource::UnloadResources() {
        //unload resources
    }

};// namespace Moer::Render