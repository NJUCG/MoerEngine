#include "GaussianGpuScene.h"

#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace Moer::Render {
GaussianGpuUpload BuildGaussianGpuUpload(const GaussianSceneSnapshot& snapshot, bool include_geometry) {
    GaussianGpuUpload                                                   upload;
    std::unordered_map<const GaussianAsset*, std::pair<uint32, uint32>> ranges;
    uint64                                                              total = 0;
    for (const auto& instance : snapshot.instances) {
        if (!instance.asset || instance.asset->gaussians.empty() || !instance.settings.enabled)
            continue;
        const auto* key = instance.asset.get();
        if (!ranges.contains(key)) {
            const auto count = instance.asset->gaussians.size();
            total += count;
            if (total > std::numeric_limits<uint32>::max() / sizeof(GGaussian))
                throw std::runtime_error("Gaussian GPU buffer exceeds RHI size limit");
            ranges.emplace(key, std::pair(static_cast<uint32>(total - count), static_cast<uint32>(count)));
            upload.assets.push_back(instance.asset);
            if (include_geometry)
                upload.gaussians.insert(
                    upload.gaussians.end(), instance.asset->gaussians.begin(), instance.asset->gaussians.end()
                );
        }
        GGaussianInstance gpu{};
        gpu.world_transform = instance.world_transform;
        const auto& s       = instance.settings;
        gpu.color_opacity =
            float4(s.color_multiplier.x, s.color_multiplier.y, s.color_multiplier.z, s.opacity_multiplier);
        const auto [first, count] = ranges.at(key);
        const uint32 visibility =
            (instance.visible_in_game ? 1u : 0u) | (instance.visible_in_editor ? 2u : 0u);
        gpu.range = uint4(first, count, instance.asset->sh_degree, visibility);
        upload.instances.push_back(gpu);
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
    const bool geometry_changed = upload->assets != m_assets;
    if (geometry_changed)
        *upload = BuildGaussianGpuUpload(*snapshot);

    const auto upload_buffer = [&](auto& data, BufferRef& buffer, const char* name) {
        if (data.empty()) {
            buffer = nullptr;
            return;
        }
        const auto bytes = data.size() * sizeof(data[0]);
        // New immutable storage for changed data. RHI commands retain resource refs for in-flight use.
        buffer = RenderDevice::Get().CreateBuffer<byte>(
            name,
            static_cast<uint>(bytes),
            EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST |
                EBufferUsageFlags::TRANSFER_SRC
        );
        commands.CopyFrom(
            std::span<byte>(reinterpret_cast<byte*>(data.data()), bytes), buffer->GetView(), name
        );
    };
    if (geometry_changed)
        upload_buffer(upload->gaussians, m_gaussians, "Gaussian geometry");
    upload_buffer(upload->instances, m_instances, "Gaussian instances");
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
        geometry_changed
    );
    if (!commands.IsEmpty())
        commands.AddCallback([upload, snapshot]() {}); // CopyFrom holds a non-owning CPU span.
}
} // namespace Moer::Render
