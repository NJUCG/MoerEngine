#ifndef MOER_RT_RESOURCE_H
#define MOER_RT_RESOURCE_H

#include "misc/STL.h"
#include "rhi/RHIResource.h"
#include <filesystem>
#include <string_view>
namespace Moer::Render {
    class RTResource {
    public:
        RTResource(const std::filesystem::path& _resouce_path);
        ~RTResource();
        void LoadResources();
        void UnloadResources();

        TextureRef                                   GetTexture(std::string_view _name) const;
        BufferRef                                    GetBuffer(std::string_view _name) const;
        TextureRef                                   GetDefaultEnvMap();
        const UnorderedMap<std::string, TextureRef>& GetTextures() const { return textures; }

    private:
        bool                  b_loaded;
        std::filesystem::path resource_path;
        std::string_view      default_env_map_name;

        UnorderedMap<std::string, TextureRef> textures;
    };
}// namespace Moer::Render

#endif