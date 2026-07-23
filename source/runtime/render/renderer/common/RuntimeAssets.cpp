// 实现 CPU 异步加载，以及从复制队列到图形队列的显式资源所有权转移。

#include "RuntimeAssets.h"

#include "misc/ScopedLogTimer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIExecutor.h"
#include "rhi/RHIResource.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include "tinyexr.h"
#include <cassert>
#include <utility>

#include <stb_image.h>

namespace Moer {

RuntimeAssets::RuntimeAssets(std::filesystem::path asset_root, Render::RenderDevice& device) :
    assets_path(std::move(asset_root)),
    device(device) {
    assert(std::filesystem::exists(assets_path) && "RuntimeAssets path not exists");

    const GraphEventRef texture_load_event = LambdaTask::Create([this]() {
                                                 LoadTextures();
                                             }).Dispatch();
    load_event =
        LambdaTask::Create([this, texture_load_event]() {
            TaskGraph::GetInterface().WaitUntilTaskComplete(texture_load_event, EThread::UNKNOWN_THREAD);
            CompleteAndImportResources();
        }).Dispatch();
}

Render::TextureRef RuntimeAssets::GetTexture(std::string_view name) const {
    const auto it = textures.find(std::string(name));
    if (it != textures.end()) {
        return it->second;
    }
    return nullptr;
}

Render::BufferRef RuntimeAssets::GetBuffer(std::string_view /*name*/) const {
    // 该资源加载器目前尚未注册运行时 Buffer。
    return nullptr;
}

Render::TextureRef RuntimeAssets::GetDefaultEnvMap() const {
    return GetTexture(default_env_map_name);
}

void RuntimeAssets::LoadTextures() {
    ScopedLogTimer startup_timer("[Startup][RuntimeAssets] LoadTextures total");

    using namespace Render;
    Array<ExportTexture> exported_textures;
    {
        const auto texture_path = assets_path / "textures";

        auto register_image = [this](TextureRef texture, const std::string& name) {
            textures[name] = std::move(texture);
        };

        CommandList cmd_list(EQueueType::Copy);
        if (std::filesystem::exists(texture_path)) {
            for (const auto& entry : std::filesystem::directory_iterator(texture_path)) {
                if (entry.path().extension() == ".png") {
                    FILE* file = nullptr;
                    fopen_s(&file, entry.path().string().c_str(), "rb");
                    int width, height, channels;
                    if (file) {
                        ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

                        TextureRef texture = device.CreateTexture(
                            entry.path().filename().string(),
                            Extent2D(width, height),
                            PF_R8G8B8A8_UNORM,
                            ETextureUsageFlags::SAMPLED
                        );
                        exported_textures.emplace_back(texture, ETextureState::SAMPLE);
                        cmd_list.CopyFrom(
                            std::span<Moer::byte>(reinterpret_cast<Moer::byte*>(data), width * height * 4),
                            texture
                        );
                        cmd_list.AddCallback([data]() {
                            stbi_image_free(data);
                        });
                        register_image(texture, entry.path().filename().string());
                    }
                }

                else if (entry.path().extension() == ".exr") {
                    int         width = 0, height = 0;
                    float*      data   = nullptr;
                    const char* err    = nullptr;
                    const auto  result = LoadEXR(&data, &width, &height, entry.path().string().c_str(), &err);
                    if (result != TINYEXR_SUCCESS) {
                        if (err) {
                            fprintf(stderr, "ERR : %s\n", err);
                            FreeEXRErrorMessage(err);
                        }
                    }
                    TextureRef texture = device.CreateTexture(
                        entry.path().filename().string(),
                        Extent2D(width, height),
                        PF_R32G32B32A32_SFLOAT,
                        ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                        1 // 该加载器目前尚不支持为 EXR 生成 mip。
                    );
                    exported_textures.emplace_back(texture, ETextureState::SAMPLE);

                    cmd_list.CopyFrom(
                        std::span<Moer::byte>(
                            reinterpret_cast<Moer::byte*>(data), width * height * 4 * sizeof(float)
                        ),
                        texture
                    );
                    cmd_list.AddCallback([data]() {
                        free(data);
                    });
                    register_image(texture, entry.path().filename().string());
                    default_env_map_name = textures.find(entry.path().filename().string())->first;
                }
            }
        }
        cmd_list.ExportResourcesToQueue(EQueueType::Graphics, std::move(exported_textures), {});

        RHIExecutor::Get().Submit(
            EQueueType::Copy,
            cmd_list.Submit(),
            ERHIExecSubmitFlags::FlushGPU
        );
        RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    }
}

void RuntimeAssets::CompleteAndImportResources() {
    using namespace Render;
    Array<ImportTexture> import_textures;
    import_textures.reserve(textures.size());
    for (const auto& texture_entry : textures) {
        const auto& texture = texture_entry.second;
        import_textures.emplace_back(
            ImportTexture(texture->GetView(0, texture->GetNumMips()), ETextureState::SAMPLE)
        );
    }

    CommandList cmd_list{};
    cmd_list.ImportResourcesFromQueue(EQueueType::Copy, std::move(import_textures), {});

    CopyQueue& copy_queue = device.GetCopyQueue();

    const auto copy_queue_timeline = copy_queue.GetFenceHandle();
    RHIExecutor::Get().Submit(
        EQueueType::Graphics,
        cmd_list.Submit().Wait(copy_queue_timeline, copy_queue_timeline->GetValue()),
        ERHIExecSubmitFlags::FlushGPU
    );
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    is_loaded.store(true, std::memory_order_seq_cst);
}

bool RuntimeAssets::IsReady() const {
    return is_loaded.load(std::memory_order_relaxed);
}

void RuntimeAssets::WaitUntilReady() const {
    if (!IsReady()) {
        load_event->Wait();
    }
}
} // namespace Moer
