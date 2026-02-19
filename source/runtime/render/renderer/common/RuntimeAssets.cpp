#include "RuntimeAssets.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include "tinyexr.h"
#include <atomic>
#include <cassert>
#include <stb_image.h>

namespace Moer {

static uint CalcMaxMipCount(uint2 _extent) {
    uint max_dim = std::max(_extent.x, _extent.y);
    return 1 + static_cast<uint>(std::floor(std::log2(max_dim)));
}
RuntimeAssets::RuntimeAssets(std::filesystem::path _assets_path, Render::RenderDevice& _device) :
    assets_path(_assets_path),
    device(_device) {
    assert(std::filesystem::exists(assets_path) && "RuntimeAssets path not exists");

    GraphEventRef evt = LambdaTask::Create([this]() {
                            LoadTextures();
                        }).Dispatch();
    load_event        = LambdaTask::Create([this, evt]() {
                     TaskGraph::GetInterface().WaitUntilTaskComplete(evt, EThread::UNKNOWN_THREAD);
                     CompleteAndImportResources();
                 }).Dispatch();
}

Render::TextureRef RuntimeAssets::GetTexture(std::string_view _name) const {
    auto it = textures.find(std::string(_name));
    if (it != textures.end()) {
        return it->second;
    }
    return nullptr;
}

Render::BufferRef RuntimeAssets::GetBuffer(std::string_view _name) const {
    return nullptr;
}

Render::TextureRef RuntimeAssets::GetDefaultEnvMap() const {
    return GetTexture(default_env_map_name);
}

void RuntimeAssets::LoadTextures() {
    // Load textures
    using namespace Render;
    Array<ExportTexture> exp_textures;
    {
        auto texture_path = assets_path / "textures";

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
                            ETextureUsageFlags::SAMPLED
                        );
                        exp_textures.emplace_back(texture, ETextureState::SAMPLE);
                        cmd_list.CopyFrom(
                            std::span<Moer::byte>((Moer::byte*)data, width * height * 4), texture
                        );
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
                            FreeEXRErrorMessage(err); // release memory of error message.
                        }
                    }
                    TextureRef texture = RenderDevice::Get().CreateTexture(
                        entry.path().filename().string(),
                        Extent2D(width, height),
                        PF_R32G32B32A32_SFLOAT,
                        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                        // CalcMaxMipCount(uint2(width, height)) // TODO: 给exr生成mipmap
                        1 // 临时解决方案，不生成mipmap
                    );
                    exp_textures.emplace_back(texture, ETextureState::SAMPLE);

                    cmd_list.CopyFrom(
                        std::span<Moer::byte>((Moer::byte*)data, width * height * 4 * sizeof(float)), texture
                    );
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

        cmd_list.ExportResourcesToQueue(EQueueType::Graphics, std::move(exp_textures), {});

        auto sync_time = device.GetCopyQueue().Execute(cmd_list.Submit());
        device.GetCopyQueue().Sync(sync_time.timeline);
    }
}

void RuntimeAssets::CompleteAndImportResources() {
    using namespace Render;
    auto& gfx_queue = device.GetCommandQueue(EQueueType::Graphics);

    Array<ImportTexture> import_textures;
    import_textures.reserve(textures.size());
    for (auto& [name, tex] : textures) {
        import_textures.emplace_back(
            ImportTexture(tex->GetView(0, tex->GetNumMips()), ETextureState::SAMPLE)
        );
    }

    CommandList cmd_list{};
    cmd_list.ImportResourcesFromQueue(EQueueType::Copy, std::move(import_textures), {});

    CopyQueue& copy_queue = device.GetCopyQueue();

    auto copy_queue_timeline = copy_queue.GetFenceHandle();
    gfx_queue.Execute(cmd_list.Submit().Wait(copy_queue_timeline, copy_queue_timeline->GetValue()));
    gfx_queue.Sync();
    b_loaded.store(true, std::memory_order_seq_cst);
}

bool RuntimeAssets::IsReady() const {
    return b_loaded.load(std::memory_order_relaxed);
}
void RuntimeAssets::WaitUntilReady() const {
    if (!IsReady()) {
        load_event->Wait();
    }
}
} // namespace Moer