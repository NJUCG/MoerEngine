#include "GaussianCacheExtension.h"
#include "scene/LogicalComponents.h"
#include <algorithm>
#include <cmath>

namespace Moer {
namespace {
bool WriteFloat3(SceneCacheBinaryWriter& writer, float3 value) {
    return writer.WritePod(value.x) && writer.WritePod(value.y) && writer.WritePod(value.z);
}
bool ReadFloat3(SceneCacheBinaryReader& reader, float3& value) {
    return reader.ReadPod(value.x) && reader.ReadPod(value.y) && reader.ReadPod(value.z);
}
bool IsValidInstanceSettings(const GaussianInstanceSettings& settings) {
    return std::isfinite(settings.opacity_multiplier) && settings.opacity_multiplier >= 0 &&
           std::isfinite(settings.color_multiplier.x) && settings.color_multiplier.x >= 0 &&
           std::isfinite(settings.color_multiplier.y) && settings.color_multiplier.y >= 0 &&
           std::isfinite(settings.color_multiplier.z) && settings.color_multiplier.z >= 0;
}
bool RebuildAssetBounds(GaussianAsset& asset) {
    if (asset.sh_degree > 3)
        return false;
    for (const auto& gaussian : asset.gaussians) {
        const auto is_finite = [](float4 value) {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
                   std::isfinite(value.w);
        };
        if (!is_finite(gaussian.position) || !is_finite(gaussian.scale_opacity) ||
            !is_finite(gaussian.rotation))
            return false;
        for (auto coefficient : gaussian.sh)
            if (!is_finite(coefficient))
                return false;
        const auto   scale           = gaussian.scale_opacity;
        const auto   rotation        = gaussian.rotation;
        const double rotation_length = double(rotation.x) * rotation.x + double(rotation.y) * rotation.y +
                                       double(rotation.z) * rotation.z + double(rotation.w) * rotation.w;
        if (scale.x <= 0 || scale.y <= 0 || scale.z <= 0 || scale.w < 0 || scale.w > 1 ||
            std::abs(rotation_length - 1) > 1e-4)
            return false;
        const float  radius = 3 * std::max({scale.x, scale.y, scale.z});
        const float3 center(gaussian.position.x, gaussian.position.y, gaussian.position.z);
        const auto   lower = center - float3(radius), upper = center + float3(radius);
        if (!std::isfinite(lower.x) || !std::isfinite(lower.y) || !std::isfinite(lower.z) ||
            !std::isfinite(upper.x) || !std::isfinite(upper.y) || !std::isfinite(upper.z))
            return false;
        asset.bounds.Expand(lower);
        asset.bounds.Expand(upper);
    }
    return true;
}
} // namespace

bool WriteGaussianCacheExtension(
    SceneCacheBinaryWriter&    writer,
    const entt::registry&      registry,
    const Array<entt::entity>& entities
) {
    std::vector<GaussianAssetRef>              assets;
    UnorderedMap<const GaussianAsset*, uint64> asset_ids;
    uint64                                     instance_count = 0;
    for (auto entity : entities) {
        if (const auto* instance = registry.try_get<ecs::CGaussianInstance>(entity)) {
            if (!instance->asset || !IsValidInstanceSettings(instance->settings))
                return false;
            if (asset_ids.emplace(instance->asset.get(), assets.size()).second)
                assets.push_back(instance->asset);
            ++instance_count;
        }
    }
    const auto*                context = registry.ctx().find<const ecs::CtxGaussianViewSettings>();
    const GaussianViewSettings view    = context ? context->value : GaussianViewSettings{};
    if (!IsValidGaussianViewSettings(view) || !writer.WritePod(uint64(assets.size())))
        return false;
    // Store canonical CPU data once per shared asset. No GPU buffers or external PLY reload is required.
    for (const auto& asset : assets) {
        const auto        source_utf8 = asset->source_path.generic_u8string();
        const std::string source(reinterpret_cast<const char*>(source_utf8.data()), source_utf8.size());
        if (!writer.WriteString(source) || !writer.WritePod(asset->sh_degree) ||
            !writer.WritePod(uint64(asset->gaussians.size())) ||
            !writer.WriteBytes(asset->gaussians.data(), asset->gaussians.size() * sizeof(Render::GGaussian)))
            return false;
    }
    if (!writer.WritePod(instance_count))
        return false;
    for (uint64 entity_id = 0; entity_id < entities.size(); ++entity_id) {
        if (const auto* instance = registry.try_get<ecs::CGaussianInstance>(entities[entity_id])) {
            if (!writer.WritePod(entity_id) || !writer.WritePod(asset_ids.at(instance->asset.get())) ||
                !WriteFloat3(writer, instance->settings.color_multiplier) ||
                !writer.WritePod(instance->settings.opacity_multiplier) ||
                !writer.WritePod(uint32(instance->settings.is_enabled)) ||
                !writer.WritePod(uint32(instance->settings.cast_shadows)) ||
                !writer.WritePod(uint32(instance->settings.receive_shadows)))
                return false;
        }
    }
    uint64 visibility_count = 0;
    for (auto entity : entities)
        if (registry.all_of<ecs::CVisibility>(entity))
            ++visibility_count;
    if (!writer.WritePod(visibility_count))
        return false;
    for (uint64 entity_id = 0; entity_id < entities.size(); ++entity_id) {
        if (const auto* visibility = registry.try_get<ecs::CVisibility>(entities[entity_id])) {
            const uint32 flags =
                uint32(visibility->visible_in_game) | (uint32(visibility->visible_in_editor) << 1);
            if (!writer.WritePod(entity_id) || !writer.WritePod(flags))
                return false;
        }
    }
    return WriteFloat3(writer, view.background) && writer.WritePod(view.exposure_ev) &&
           writer.WritePod(view.display_mode) && writer.WritePod(view.debug_view) &&
           writer.WritePod(view.tile_pair_capacity) && writer.WritePod(view.shadow_alpha_cutoff) &&
           writer.WritePod(view.shadow_receive_strength) && writer.WritePod(view.shadow_world_bias);
}

bool ReadGaussianCacheExtension(
    SceneCacheBinaryReader&    reader,
    entt::registry&            registry,
    const Array<entt::entity>& entities,
    uint32 version
) {
    uint64 asset_count = 0;
    if (!reader.ReadPod(asset_count) || asset_count > entities.size())
        return false;
    std::vector<GaussianAssetRef> assets;
    for (uint64 asset_id = 0; asset_id < asset_count; ++asset_id) {
        auto        asset = std::make_shared<GaussianAsset>();
        std::string source;
        uint64      gaussian_count = 0;
        if (!reader.ReadString(source) || !reader.ReadPod(asset->sh_degree) ||
            !reader.ReadPod(gaussian_count) ||
            gaussian_count > reader.RemainingBytes() / sizeof(Render::GGaussian) ||
            gaussian_count > asset->gaussians.max_size())
            return false;
        asset->source_path = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(source.data()), source.size())
        );
        asset->gaussians.resize(static_cast<size_t>(gaussian_count));
        if (!reader.ReadBytes(asset->gaussians.data(), gaussian_count * sizeof(Render::GGaussian)) ||
            !RebuildAssetBounds(*asset))
            return false;
        assets.push_back(std::move(asset));
    }
    uint64 instance_count = 0;
    if (!reader.ReadPod(instance_count) || instance_count > entities.size())
        return false;
    for (uint64 instance_id = 0; instance_id < instance_count; ++instance_id) {
        uint64                   entity_id = 0, asset_id = 0;
        uint32                   is_enabled = 0;
        GaussianInstanceSettings settings;
        if (!reader.ReadPod(entity_id) || !reader.ReadPod(asset_id) ||
            !ReadFloat3(reader, settings.color_multiplier) || !reader.ReadPod(settings.opacity_multiplier) ||
            !reader.ReadPod(is_enabled) || entity_id >= entities.size() || asset_id >= assets.size() ||
            is_enabled > 1 || !IsValidInstanceSettings(settings))
            return false;
        const auto entity = entities[entity_id];
        if (!registry.all_of<ecs::CNode>(entity) || registry.all_of<ecs::CGaussianInstance>(entity))
            return false;
        settings.is_enabled = is_enabled != 0;
        if (version >= 7) {
            uint32 cast = 0, receive = 0;
            if (!reader.ReadPod(cast) || !reader.ReadPod(receive) || cast > 1 || receive > 1) return false;
            settings.cast_shadows = cast != 0; settings.receive_shadows = receive != 0;
        }
        registry.emplace<ecs::CGaussianInstance>(entity, assets[asset_id], settings);
    }
    uint64 visibility_count = 0;
    if (!reader.ReadPod(visibility_count) || visibility_count > entities.size())
        return false;
    for (uint64 visibility_id = 0; visibility_id < visibility_count; ++visibility_id) {
        uint64 entity_id = 0;
        uint32 flags     = 0;
        if (!reader.ReadPod(entity_id) || !reader.ReadPod(flags) || entity_id >= entities.size() || flags > 3)
            return false;
        const auto entity = entities[entity_id];
        if (!registry.all_of<ecs::CNode>(entity) || registry.all_of<ecs::CVisibility>(entity))
            return false;
        registry.emplace<ecs::CVisibility>(entity, (flags & 2) != 0, (flags & 1) != 0);
    }
    GaussianViewSettings view;
    if (!ReadFloat3(reader, view.background) || !reader.ReadPod(view.exposure_ev) ||
        !reader.ReadPod(view.display_mode) || !reader.ReadPod(view.debug_view) ||
        !reader.ReadPod(view.tile_pair_capacity) || !IsValidGaussianViewSettings(view))
        return false;
    if (version >= 7 && (!reader.ReadPod(view.shadow_alpha_cutoff) ||
        !reader.ReadPod(view.shadow_receive_strength) || !reader.ReadPod(view.shadow_world_bias))) return false;
    if (!IsValidGaussianViewSettings(view)) return false;
    registry.ctx().emplace<ecs::CtxGaussianViewSettings>(view);
    return true;
}
} // namespace Moer
