#include "RuntimeAssets.h"
#include "Core.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "string/StringConvert.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include "tinyexr.h"
#include <atomic>
#include <cassert>
#include <functional>
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

    // Worker thread: decode files and record upload commands (no GPU submission)
    record_event = LambdaTask::Create([this]() {
                       RecordTextureUploads();
                   }).Dispatch();
}

RuntimeAssets::~RuntimeAssets() {
    if (record_event) {
        record_event->Wait();
    }
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

void RuntimeAssets::RecordTextureUploads() {
    // Record-only: decode files and build upload command list. No GPU submission.
    using namespace Render;
    {
        auto texture_path = assets_path / "textures";

        auto register_image = [this](TextureRef _tex, const std::string& _name) {
            //register image
            auto& image = this->textures[_name];
            image       = _tex;
        };

        CommandList                   cmd_list(EQueueType::Graphics);
        Array<std::function<void()>> deferred_callbacks{};
        if (std::filesystem::exists(texture_path)) {
            {
                CopyCommandScope copy_scope = cmd_list.BeginCopyScope();
                for (auto& entry : std::filesystem::directory_iterator(texture_path)) {
                    LOG_INFO(MOER_TEXT("Load texture {}"), entry.path().string());
                    if (entry.path().extension() == ".png") {
                        FILE* file = nullptr;
                        fopen_s(&file, entry.path().string().c_str(), "rb");
                        int width, height, channels;
                        if (file) {
                            ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

                            std::string texture_name_utf8 = entry.path().filename().string();
                            String      texture_name = Utf8ToPlatform(
                                Utf8StringView(texture_name_utf8.data(), texture_name_utf8.size())
                            );
                            TextureRef texture = RenderDevice::Get().CreateTexture(
                                texture_name,
                                Extent2D(width, height),
                                PF_R8G8B8A8_UNORM,
                                ETextureUsageFlags::SAMPLED
                            );
                            copy_scope.CopyFrom(
                                std::span<Moer::byte>((Moer::byte*)data, width * height * 4), texture
                            );
                            deferred_callbacks.emplace_back([data]() {
                                stbi_image_free(data);
                            });
                            register_image(texture, texture_name_utf8);
                        }
                    } else if (entry.path().extension() == ".exr") {
                        //load exr
                        int         width = 0, height = 0, channels = 0;
                        float*      data = nullptr;
                        const char* err  = nullptr;
                        auto        ret =
                            LoadEXR(&data, &width, &height, entry.path().string().c_str(), &err);
                        if (ret != TINYEXR_SUCCESS) {
                            if (err) {
                                fprintf(stderr, "ERR : %s\n", err);
                                FreeEXRErrorMessage(err); // release memory of error message.
                            }
                        }
                        std::string texture_name_utf8 = entry.path().filename().string();
                        String      texture_name = Utf8ToPlatform(
                            Utf8StringView(texture_name_utf8.data(), texture_name_utf8.size())
                        );
                        TextureRef texture = RenderDevice::Get().CreateTexture(
                            texture_name,
                            Extent2D(width, height),
                            PF_R32G32B32A32_SFLOAT,
                            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                            // CalcMaxMipCount(uint2(width, height)) // TODO: 缁檈xr鐢熸垚mipmap
                            1 // 涓存椂瑙ｅ喅鏂规锛屼笉鐢熸垚mipmap
                        );
                        copy_scope.CopyFrom(
                            std::span<Moer::byte>(
                                (Moer::byte*)data, width * height * 4 * sizeof(float)
                            ),
                            texture
                        );
                        deferred_callbacks.emplace_back([data]() {
                            free(data);
                        });
                        register_image(texture, texture_name_utf8);
                        default_env_map_name = textures.find(entry.path().filename().string())->first;
                    }
                }
            }

            for (auto& callback : deferred_callbacks) {
                cmd_list.AddCallback(std::move(callback));
            }
        }
        if (!cmd_list.IsEmpty()) {
            {
                std::lock_guard<std::mutex> lock(payload_mutex);
                pending_payload.command_lists.emplace_back(std::move(cmd_list));
            }
        }
    }
    b_recorded.store(true, std::memory_order_release);
}

bool RuntimeAssets::SubmitPendingUploads() {
    assert(Moer::IsCurrentlyGameThread());

    if (b_loaded.load(std::memory_order_acquire)) {
        return false;
    }

    if (!b_recorded.load(std::memory_order_acquire)) {
        return false;
    }

    AsyncRecordedSubmitPayload payload;
    {
        std::lock_guard<std::mutex> lock(payload_mutex);
        payload = std::move(pending_payload);
    }

    if (!payload.command_lists.empty()) {
        Render::RHIExecutor::Get().Submit(
            std::move(payload.command_lists), Render::ERHIExecSubmitFlags::FlushGPU
        );
    }

    for (auto& cb : payload.post_submit_callbacks) {
        cb();
    }

    b_loaded.store(true, std::memory_order_release);
    return true;
}

bool RuntimeAssets::IsReady() const {
    return b_loaded.load(std::memory_order_acquire);
}
} // namespace Moer
