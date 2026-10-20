#include "GaussianGpuScene.h"

#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace Moer::Render {
GaussianGpuUpload
BuildGaussianGpuUpload(const GaussianSceneSnapshot& snapshot, bool should_include_geometry) {
    GaussianGpuUpload                                                   upload;
    std::unordered_map<const GaussianAsset*, std::pair<uint32, uint32>> ranges;
    uint64                                                              gaussian_count = 0;
    for (const auto& instance : snapshot.instances) {
        if (!instance.asset || instance.asset->gaussians.empty() || !instance.settings.is_enabled)
            continue;
        const auto* key = instance.asset.get();
        if (!ranges.contains(key)) {
            const auto asset_gaussian_count = instance.asset->gaussians.size();
            gaussian_count += asset_gaussian_count;
            if (gaussian_count > std::numeric_limits<uint32>::max() / sizeof(GGaussian))
                throw std::runtime_error("Gaussian GPU buffer exceeds RHI size limit");
            ranges.emplace(
                key,
                std::pair(
                    static_cast<uint32>(gaussian_count - asset_gaussian_count),
                    static_cast<uint32>(asset_gaussian_count)
                )
            );
            upload.assets.push_back(instance.asset);
            if (should_include_geometry)
                upload.gaussians.insert(
                    upload.gaussians.end(), instance.asset->gaussians.begin(), instance.asset->gaussians.end()
                );
        }
        GGaussianInstance gpu_instance{};
        gpu_instance.world_transform = instance.world_transform;
        const auto& settings         = instance.settings;
        gpu_instance.color_opacity   = float4(
            settings.color_multiplier.x,
            settings.color_multiplier.y,
            settings.color_multiplier.z,
            settings.opacity_multiplier
        );
        const auto [first_gaussian_id, asset_gaussian_count] = ranges.at(key);
        const uint32 visibility =
            (instance.is_visible_in_game ? 1u : 0u) | (instance.is_visible_in_editor ? 2u : 0u);
        gpu_instance.range =
            uint4(first_gaussian_id, asset_gaussian_count, instance.asset->sh_degree, visibility);
        upload.instances.push_back(gpu_instance);
        if (upload.instances.size() > std::numeric_limits<uint32>::max() / sizeof(GGaussianInstance))
            throw std::runtime_error("Gaussian instance buffer exceeds RHI size limit");
    }
    return upload;
}

void GaussianGpuScene::ApplyUpdate(
    std::shared_ptr<const GaussianSceneSnapshot> snapshot,
    CommandList&                                 commands
) {
    auto       upload = std::make_shared<GaussianGpuUpload>(BuildGaussianGpuUpload(*snapshot, false));
    const bool has_geometry_changed = upload->assets != m_assets;
    if (has_geometry_changed)
        *upload = BuildGaussianGpuUpload(*snapshot);

    const auto upload_buffer = [&](auto& data, BufferRef& out_buffer, const char* name) {
        if (data.empty()) {
            out_buffer = nullptr;
            return;
        }
        const auto byte_count = data.size() * sizeof(data[0]);
        // New immutable storage for changed data. RHI commands retain resource refs for in-flight use.
        out_buffer = RenderDevice::Get().CreateBuffer<byte>(
            name,
            static_cast<uint>(byte_count),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
                EBufferUsageFlags::TRANSFER_SRC
        );
        commands.CopyFrom(
            std::span<byte>(reinterpret_cast<byte*>(data.data()), byte_count), out_buffer->GetView(), name
        );
    };
    if (has_geometry_changed)
        upload_buffer(upload->gaussians, m_gaussian_buffer, "Gaussian geometry");
    upload_buffer(upload->instances, m_instance_buffer, "Gaussian instances");
    m_assets         = upload->assets;
    m_gaussian_count = 0;
    for (const auto& asset : m_assets)
        m_gaussian_count += static_cast<uint32>(asset->gaussians.size());
    m_instance_count = static_cast<uint32>(upload->instances.size());
    m_revision       = snapshot->revision;
    LOG_INFO(
        "[GaussianScene] revision={} assets={} gaussians={} instances={} geometry_upload={}",
        m_revision,
        m_assets.size(),
        m_gaussian_count,
        m_instance_count,
        has_geometry_changed
    );
    if (!commands.IsEmpty())
        commands.AddCallback([upload, snapshot]() {}); // CopyFrom holds a non-owning CPU span.
}
} // namespace Moer::Render
