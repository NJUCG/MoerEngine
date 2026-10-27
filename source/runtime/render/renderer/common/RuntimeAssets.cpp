// 实现 CPU 异步加载，以及从复制队列到图形队列的显式资源所有权转移。

#include "RuntimeAssets.h"
#include "rendergraph/RenderGraph.h"
#include <stdexcept>
#include <format>

#include "misc/ScopedLogTimer.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/executor/RHIExecutor.h"
#include "rhi/RHIResource.h"
#include "taskgraph/GraphTask.h"
#include "taskgraph/TaskGraph.h"
#include "taskgraph/ThreadManager.h"
#include "tinyexr.h"
#include <cassert>
#include <cstdio>
#include <utility>

#include <stb_image.h>

namespace Moer {

RuntimeAssets::RuntimeAssets(std::filesystem::path asset_root, Render::RenderDevice& device) :
    assets_path(std::move(asset_root)),
    device(device) {
    assert(std::filesystem::exists(assets_path) && "RuntimeAssets path not exists");

    load_event = LambdaTask::Create([this]() {
        try { LoadTextures(); }
        catch (const std::exception& error) {
            LOG_ERROR("Runtime asset loading failed: {}", error.what());
            throw;
        }
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
    RenderGraph graph("RuntimeAssets.Upload", RenderGraph::QueueTopology::FromRHI());
    Array<RenderGraph::TextureHandle> resources;
    const auto upload = [&](const TextureRef& texture, std::span<const byte> bytes) {
        const auto handle = graph.ImportTexture(texture->GetName(), texture, {});
        graph.SetInitialState(handle, RenderGraph::TextureState::Undefined,
                              RenderGraph::QueueRole::None, RenderGraph::AccessMode::None);
        graph.Export(handle, RenderGraph::TextureState::Sampled,
                     RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read);
        graph.AddRecordPass(std::format("Upload.{}", texture->GetName()), [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Copy, RenderGraph::PipelineType::Copy)
                .Write(handle, RenderGraph::TextureState::TransferDestination);
        }, [texture, data = Array<byte>(bytes.begin(), bytes.end())](CommandList& commands) {
            commands.CopyFrom(std::span<const byte>(data.data(), data.size()), texture);
        });
        resources.push_back(handle);
    };
    const auto texture_path = assets_path / "textures";
    if (std::filesystem::exists(texture_path)) {
        for (const auto& entry : std::filesystem::directory_iterator(texture_path)) {
            const auto name = entry.path().filename().string();
            if (entry.path().extension() == ".png") {
                int width = 0, height = 0, channels = 0;
                auto* data = stbi_load(entry.path().string().c_str(), &width, &height, &channels, 4);
                if (!data) { throw std::runtime_error("Cannot load runtime image: " + name); }
                auto texture = device.CreateTexture(name, Extent2D(width, height), PF_R8G8B8A8_UNORM,
                                                     ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST);
                upload(texture, std::span<const byte>(reinterpret_cast<const byte*>(data), width * height * 4));
                stbi_image_free(data);
                textures[name] = std::move(texture);
            } else if (entry.path().extension() == ".exr") {
                int width = 0, height = 0;
                float* data = nullptr;
                const char* error = nullptr;
                if (LoadEXR(&data, &width, &height, entry.path().string().c_str(), &error) != TINYEXR_SUCCESS) {
                    std::string message = error ? error : "unknown EXR error";
                    if (error) { FreeEXRErrorMessage(error); }
                    throw std::runtime_error("Cannot load runtime image: " + name + ": " + message);
                }
                auto texture = device.CreateTexture(name, Extent2D(width, height), PF_R32G32B32A32_SFLOAT,
                                                     ETextureUsageFlags::SAMPLED | ETextureUsageFlags::TRANSFER_DST);
                upload(texture, std::span<const byte>(reinterpret_cast<const byte*>(data), width * height * 4 * sizeof(float)));
                free(data);
                textures[name] = std::move(texture);
                default_env_map_name = textures.find(name)->first;
            }
        }
    }
    auto accepted = device.CreateFence();
    graph.AddRecordPass("PublishToGraphics", [=](RenderGraph::PassBuilder& builder) {
        builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics).SideEffect();
        for (auto texture : resources) { builder.Read(texture, RenderGraph::TextureState::Sampled); }
    }, [accepted](CommandList& commands) { commands.Signal(accepted, 1); });
    if (!graph.Compile() || !graph.ExecuteFrontendRecordingPlan({}, {}, false, {},
            {.enabled = true, .state_ownership = ERHIResourceStateOwnership::Compiled})) {
        throw std::runtime_error("Runtime asset graph: " + graph.GetCompileError());
    }
    RHIExecutor::Get().Sync(ERHISyncDepth::RHI);
    if (!accepted->WaitSubmitted(1)) { throw std::runtime_error("Runtime asset graph submission rejected"); }
    is_loaded.store(true, std::memory_order_release);
}

bool RuntimeAssets::IsReady() const {
    return is_loaded.load(std::memory_order_acquire);
}

void RuntimeAssets::WaitUntilReady() const {
    if (!IsReady()) {
        load_event->Wait();
    }
}
} // namespace Moer
