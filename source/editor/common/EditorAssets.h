#ifndef MOER_EDITOR_ASSETS_H
#define MOER_EDITOR_ASSETS_H
#include "taskgraph/GraphTask.h"
#include <atomic>
#include <filesystem>
#include <rhi/RHI.h>
namespace Moer {
class EditorAssets {
public:
    struct DefaultResource {
        Render::TextureRef white;
        Render::TextureRef black;
    };
    EditorAssets(std::filesystem::path _assets_path, Render::RenderDevice& _device);
    bool               IsReady() const;
    Render::TextureRef GetTexture(std::string_view _name) const;
    Render::BufferRef  GetBuffer(std::string_view _name) const;
    Render::TextureRef GetDefaultEnvMap() const;
    void               WaitUntilReady() const;

private:
    void LoadTextures();
    void CompleteAndImportResources();

private:
    std::filesystem::path assets_path;
    Render::RenderDevice& device;
    std::string_view      default_env_map_name;

    UnorderedMap<std::string, Render::TextureRef> textures;

    std::atomic_bool b_loaded = false;
    GraphEventRef    load_event;
};
} // namespace Moer

#endif