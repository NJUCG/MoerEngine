#ifndef MOER_RT_RESOURCE_H
#define MOER_RT_RESOURCE_H

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
        bool IsLoadComplete() const;

        TextureRef GetTexture(std::string_view _name) const;
        BufferRef  GetBuffer(std::string_view _name) const;

    private:
        bool                  b_loaded;
        std::filesystem::path resource_path;
    };
}// namespace Moer::Render

#endif