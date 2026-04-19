#ifndef MOER_EDITOR_ASSETS_H
#define MOER_EDITOR_ASSETS_H
#include "taskgraph/GraphTask.h"
#include <atomic>
#include <filesystem>
#include <mutex>
#include <rhi/RHI.h>
namespace Moer {

// Narrow payload carrying recorded command lists from a worker thread
// back to the main thread for submission.
struct AsyncRecordedSubmitPayload {
    Array<Render::CommandList>   command_lists;
    Array<std::function<void()>> post_submit_callbacks;
};

class RENDER_API RuntimeAssets {
public:
    struct DefaultResource {
        Render::TextureRef white;
        Render::TextureRef black;
    };
    RuntimeAssets(std::filesystem::path _assets_path, Render::RenderDevice& _device);
    ~RuntimeAssets();
    bool               IsReady() const;
    Render::TextureRef GetTexture(std::string_view _name) const;
    Render::BufferRef  GetBuffer(std::string_view _name) const;
    Render::TextureRef GetDefaultEnvMap() const;

    // Called from the main thread to submit pending recorded uploads.
    // Returns true if work was submitted (caller should sync if needed).
    bool SubmitPendingUploads();

private:
    void RecordTextureUploads();

private:
    std::filesystem::path assets_path;
    Render::RenderDevice& device;
    std::string_view      default_env_map_name;

    UnorderedMap<std::string, Render::TextureRef> textures;

    std::atomic_bool b_recorded = false;
    std::atomic_bool b_loaded   = false;
    GraphEventRef    record_event;

    std::mutex                  payload_mutex;
    AsyncRecordedSubmitPayload  pending_payload;
};

} // namespace Moer

#endif