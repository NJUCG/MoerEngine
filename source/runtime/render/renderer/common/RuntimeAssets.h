#ifndef MOER_EDITOR_ASSETS_H
#define MOER_EDITOR_ASSETS_H

// 加载渲染器启动阶段使用的只读资源，并将其转移到图形队列。

#include "taskgraph/GraphTask.h"

#include <atomic>
#include <filesystem>
#include <string_view>

#include "rhi/RHI.h"

namespace Moer {

class RENDER_API RuntimeAssets {
public:
    RuntimeAssets(std::filesystem::path asset_root, Render::RenderDevice& device);

    bool               IsReady() const;
    Render::TextureRef GetTexture(std::string_view name) const;
    Render::BufferRef  GetBuffer(std::string_view name) const;
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

    std::atomic_bool is_loaded = false;
    GraphEventRef    load_event;
};

} // namespace Moer

#endif
