#include "RuntimeAssets.h"
#include "Core.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "tinyexr.h"
#include <atomic>
#include <cassert>
#include <fstream>
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

Render::TextureRef RuntimeAssets::GetTexture(StringView _name) const {
    auto it = textures.find(String(_name));
    if (it != textures.end()) {
        return it->second;
    }
    return nullptr;
}

Render::BufferRef RuntimeAssets::GetBuffer(StringView _name) const {
    (void)_name;
    return nullptr;
}

Render::TextureRef RuntimeAssets::GetDefaultEnvMap() const {
    return GetTexture(default_env_map_name);
}

Array<std::byte> RuntimeAssets::LoadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return {};
    }

    const std::streamoff file_size = file.tellg();
    if (file_size <= 0) {
        return {};
    }

    Array<std::byte> bytes(static_cast<size_t>(file_size));
    file.seekg(0, std::ios::beg);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        return {};
    }
    return bytes;
}

void RuntimeAssets::RecordTextureUploads() {
    // Record-only: decode files and build upload command list. No GPU submission.
    using namespace Render;
    {
        auto texture_path = assets_path / "textures";

        auto register_image = [this](TextureRef _tex, StringView _name) {
            //register image
            auto& image = this->textures[_name];
            image       = _tex;
        };

        auto make_texture_name = [](const std::filesystem::path& path) {
            const auto& native_name = path.filename().native();
            return String(StringView(native_name.data(), native_name.size()));
        };

        CommandList                   cmd_list(EQueueType::Graphics);
        Array<std::function<void()>> deferred_callbacks{};
        auto upload_texture = [&](std::span<Moer::byte> data, TextureView view) {
            cmd_list.Barriers(
                {BarrierCreateInfo::Transition(
                    view,
                    ETextureState::UNDEFINED,
                    ETextureState::TRANSFER_DST,
                    EPassType::Copy
                )},
                EQueueType::Graphics,
                EQueueType::Graphics,
                ETrackedStateUpdateMode::Update
            );
            cmd_list.CopyFrom(data, view);
            cmd_list.Barriers(
                {BarrierCreateInfo::Transition(
                    view,
                    MakeBarrierState(ETextureState::TRANSFER_DST, EPassType::Copy),
                    BarrierState{
                        .stage = ERHIPipelineStageFlags::PS_ALL_COMMANDS,
                        .access = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_SAMPLED_READ
                    }
                )},
                EQueueType::Graphics,
                EQueueType::Graphics,
                ETrackedStateUpdateMode::Update
            );
        };
        if (std::filesystem::exists(texture_path)) {
            {
                for (auto& entry : std::filesystem::directory_iterator(texture_path)) {
                    const String texture_path_text = String(entry.path().native());
                    LOG_INFO(MOER_TEXT("Load texture {}"), texture_path_text);
                    if (entry.path().extension() == MOER_TEXT(".png")) {
                        const Array<std::byte> file_bytes = LoadFileBytes(entry.path());
                        if (file_bytes.empty()) {
                            continue;
                        }

                        int width = 0;
                        int height = 0;
                        int channels = 0;
                        ubyte* data = stbi_load_from_memory(
                            reinterpret_cast<const stbi_uc*>(file_bytes.data()),
                            static_cast<int>(file_bytes.size()),
                            &width,
                            &height,
                            &channels,
                            4
                        );
                        if (!data) {
                            continue;
                        }

                        const String texture_name = make_texture_name(entry.path());

                        TextureRef texture = RenderDevice::Get().CreateTexture(
                            texture_name,
                            Extent2D(width, height),
                            PF_R8G8B8A8_UNORM,
                            ETextureUsageFlags::SAMPLED
                        );
                        upload_texture(
                            std::span<Moer::byte>((Moer::byte*)data, width * height * 4), texture->GetView()
                        );
                        deferred_callbacks.emplace_back([data]() {
                            stbi_image_free(data);
                        });
                        register_image(texture, texture_name);
                    } else if (entry.path().extension() == MOER_TEXT(".exr")) {
                        const Array<std::byte> file_bytes = LoadFileBytes(entry.path());
                        if (file_bytes.empty()) {
                            continue;
                        }

                        int         width = 0;
                        int         height = 0;
                        float*      data = nullptr;
                        const char* err  = nullptr;
                        const int ret = LoadEXRFromMemory(
                            &data,
                            &width,
                            &height,
                            reinterpret_cast<const unsigned char*>(file_bytes.data()),
                            static_cast<size_t>(file_bytes.size()),
                            &err
                        );
                        if (ret != TINYEXR_SUCCESS) {
                            if (err) {
                                LOG_ERROR(MOER_TEXT("Failed to decode EXR {}: {}"), texture_path_text, err);
                                FreeEXRErrorMessage(err);
                            }
                            continue;
                        }

                        const String texture_name = make_texture_name(entry.path());
                        TextureRef texture = RenderDevice::Get().CreateTexture(
                            texture_name,
                            Extent2D(width, height),
                            PF_R32G32B32A32_SFLOAT,
                            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                            // CalcMaxMipCount(uint2(width, height)) // TODO: 缁檈xr鐢熸垚mipmap
                            1 // 涓存椂瑙ｅ喅鏂规锛屼笉鐢熸垚mipmap
                        );
                        upload_texture(
                            std::span<Moer::byte>(
                                (Moer::byte*)data, width * height * 4 * sizeof(float)
                            ),
                            texture->GetView()
                        );
                        deferred_callbacks.emplace_back([data]() {
                            free(data);
                        });
                        register_image(texture, texture_name);
                        default_env_map_name = texture_name;
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
