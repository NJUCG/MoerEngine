#include "Scene.h"
#include "loader/GaussianPlyLoader.h"
#include "math/Transform.h"
#include "gaussian/GaussianSceneFile.h"

#include <algorithm>
#include <cmath>

namespace Moer {
namespace {
bool Same(const GaussianInstanceSnapshot& lhs, const GaussianInstanceSnapshot& rhs) {
    if (lhs.key != rhs.key || lhs.asset != rhs.asset || lhs.is_visible_in_game != rhs.is_visible_in_game ||
        lhs.is_visible_in_editor != rhs.is_visible_in_editor ||
        lhs.settings.is_enabled != rhs.settings.is_enabled ||
        lhs.settings.opacity_multiplier != rhs.settings.opacity_multiplier ||
        lhs.settings.color_multiplier.x != rhs.settings.color_multiplier.x ||
        lhs.settings.color_multiplier.y != rhs.settings.color_multiplier.y ||
        lhs.settings.color_multiplier.z != rhs.settings.color_multiplier.z)
        return false;
    return std::equal(
        std::begin(lhs.world_transform.e), std::end(lhs.world_transform.e), std::begin(rhs.world_transform.e)
    );
}
} // namespace

Scene::ImportSceneFromFileResult Scene::ImportGaussianPly(const std::filesystem::path& path) {
    ImportSceneFromFileResult result;
    if (!IsReady()) {
        result.error_message = "Scene is not ready";
        return result;
    }
    const auto loaded = LoadGaussianPly(path);
    if (!loaded) {
        result.error_message = loaded.error_message;
        return result;
    }
    EntityWithNodeCreateInfo info;
    info.name                    = path.stem().string();
    result.import_root_entt      = CreateGaussianInstance(loaded.asset, info);
    result.success               = result.import_root_entt != entt::null;
    result.imported_entity_count = result.success ? 1 : 0;
    if (!result.success)
        result.error_message = "Cannot create Gaussian instance";
    return result;
}

entt::entity Scene::CreateGaussianInstance(GaussianAssetRef asset, const EntityWithNodeCreateInfo& info) {
    if (!IsReady() || !asset)
        return entt::null;
    if (!r().ctx().contains<ecs::CtxGaussianViewSettings>()) r().ctx().emplace<ecs::CtxGaussianViewSettings>();
    const auto entity = logical_scene().UCreateEntityWithNode(info);
    if (entity != entt::null)
        r().emplace<ecs::CGaussianInstance>(entity, std::move(asset), GaussianInstanceSettings{});
    return entity;
}

GaussianAssetRef Scene::GetGaussianAsset(entt::entity entity) const {
    if (!IsReady() || !r().valid(entity))
        return {};
    const auto* instance = r().try_get<ecs::CGaussianInstance>(entity);
    return instance ? instance->asset : GaussianAssetRef{};
}

std::optional<GaussianInstanceSettings> Scene::GetGaussianInstanceSettings(entt::entity entity) const {
    if (!GetGaussianAsset(entity))
        return std::nullopt;
    return r().get<ecs::CGaussianInstance>(entity).settings;
}

bool Scene::SetGaussianInstanceSettings(entt::entity entity, const GaussianInstanceSettings& settings) {
    if (!GetGaussianAsset(entity) || !std::isfinite(settings.opacity_multiplier) ||
        settings.opacity_multiplier < 0.f || !std::isfinite(settings.color_multiplier.x) ||
        settings.color_multiplier.x < 0.f || !std::isfinite(settings.color_multiplier.y) ||
        settings.color_multiplier.y < 0.f || !std::isfinite(settings.color_multiplier.z) ||
        settings.color_multiplier.z < 0.f)
        return false;
    r().get<ecs::CGaussianInstance>(entity).settings = settings;
    return true;
}

bool Scene::DestroyGaussianInstance(entt::entity entity) {
    if (!GetGaussianAsset(entity) || !r().all_of<ecs::CNode>(entity) ||
        r().get<ecs::CNode>(entity).child_count != 0)
        return false;
    // A dedicated leaf deletion avoids the Mesh full-rebuild subtree API.
    return logical_scene().UDestroyEntity(entity);
}

entt::entity Scene::DuplicateGaussianInstance(entt::entity entity) {
    const auto asset = GetGaussianAsset(entity);
    const auto local_transform = TryGetNodeLocalTransform(entity);
    if (!asset || !local_transform)
        return entt::null;
    EntityWithNodeCreateInfo info;
    info.parent_node_entt = r().get<ecs::CNode>(entity).parent_entt;
    info.name = GetNodeDisplayName(entity) + " (copy)";
    info.translation = local_transform->translation;
    info.rotation = local_transform->rotation;
    info.scale = local_transform->scale;
    const auto visibility = GetNodeVisibility(entity);
    const auto settings = *GetGaussianInstanceSettings(entity);
    const auto duplicate_entity = CreateGaussianInstance(asset, info);
    if (duplicate_entity != entt::null) {
        SetGaussianInstanceSettings(duplicate_entity, settings);
        SetNodeVisibleInGame(duplicate_entity, visibility.visible_in_game);
        SetNodeVisibleInEditor(duplicate_entity, visibility.visible_in_editor);
    }
    return duplicate_entity;
}

EGaussianSceneContent Scene::GetGaussianSceneContent() const {
    return GetGaussianSceneContentForView(false);
}

bool Scene::SaveGaussianDocument(const std::filesystem::path& path, std::string& out_error) const {
    if (!IsReady()) { out_error = "Scene is not ready"; return false; }
    return SaveGaussianSceneFile(path, *m_logical_scene, out_error);
}

GaussianViewSettings Scene::GetGaussianViewSettings() const {
    return IsReady() && r().ctx().contains<ecs::CtxGaussianViewSettings>() ? r().ctx().get<const ecs::CtxGaussianViewSettings>().value : GaussianViewSettings{};
}
bool Scene::SetGaussianViewSettings(const GaussianViewSettings& settings) {
    if (!IsReady() || !IsValidGaussianViewSettings(settings)) return false;
    if (!r().ctx().contains<ecs::CtxGaussianViewSettings>()) r().ctx().emplace<ecs::CtxGaussianViewSettings>();
    r().ctx().get<ecs::CtxGaussianViewSettings>().value = settings;
    return true;
}
bool Scene::HasGaussianSceneData() const {
    return IsReady() && (r().ctx().contains<ecs::CtxGaussianViewSettings>() || m_last_gaussian_snapshot || !r().view<const ecs::CGaussianInstance>().empty());
}

EGaussianSceneContent Scene::GetGaussianSceneContentForView(bool is_editor_view) const {
    if (!IsReady())
        return EGaussianSceneContent::Empty;
    const auto is_visible = [&](entt::entity entity) {
        const auto visibility = GetNodeVisibility(entity);
        return is_editor_view ? visibility.effectively_visible_in_editor : visibility.effectively_visible_in_game;
    };
    bool has_mesh = false, has_gaussian = false;
    for (auto entity : r().view<const ecs::CRenderable, const ecs::CNode>()) {
        if (is_visible(entity)) {
            has_mesh = true;
            break;
        }
    }
    for (auto entity : r().view<const ecs::CGaussianInstance, const ecs::CNode>()) {
        const auto& instance = r().get<ecs::CGaussianInstance>(entity);
        if (instance.asset && !instance.asset->gaussians.empty() && instance.settings.is_enabled &&
            is_visible(entity)) {
            has_gaussian = true;
            break;
        }
    }
    if (has_mesh && has_gaussian)
        return EGaussianSceneContent::Mixed;
    if (has_gaussian)
        return EGaussianSceneContent::GaussianOnly;
    return has_mesh ? EGaussianSceneContent::MeshOnly : EGaussianSceneContent::Empty;
}

std::shared_ptr<const GaussianSceneSnapshot> Scene::CaptureGaussianUpdate(bool should_force_update) {
    const auto view = r().view<const ecs::CGaussianInstance, const ecs::CNode>();
    if (view.size_hint() == 0 && !m_last_gaussian_snapshot)
        return {};
    auto snapshot = std::make_shared<GaussianSceneSnapshot>();
    for (auto entity : view) {
        const auto& instance = view.get<const ecs::CGaussianInstance>(entity);
        if (!instance.asset)
            continue;
        GaussianInstanceSnapshot item;
        item.key      = static_cast<uint64>(entt::to_integral(entity));
        item.asset    = instance.asset;
        item.settings = instance.settings;
        // Compute from authoritative local transforms. Gaussian edits do not require Mesh GPU updates.
        auto current_entt = entity;
        while (current_entt != entt::null) {
            const auto& node = r().get<ecs::CNode>(current_entt);
            item.world_transform =
                Transform(node.translation, node.scale, node.rotation).GetMatrix4x4() * item.world_transform;
            current_entt = node.parent_entt;
        }
        const auto& local_bounds = instance.asset->bounds;
        if (local_bounds.IsValid()) {
            const Transform transform(item.world_transform);
            for (int corner_id = 0; corner_id < 8; ++corner_id) {
                item.world_bounds.Expand(
                    transform * float3(
                                    (corner_id & 1) ? local_bounds.max.x : local_bounds.min.x,
                                    (corner_id & 2) ? local_bounds.max.y : local_bounds.min.y,
                                    (corner_id & 4) ? local_bounds.max.z : local_bounds.min.z
                                )
                );
            }
        }
        const auto visibility     = GetNodeVisibility(entity);
        item.is_visible_in_game   = visibility.effectively_visible_in_game;
        item.is_visible_in_editor = visibility.effectively_visible_in_editor;
        if (item.settings.is_enabled && (item.is_visible_in_game || item.is_visible_in_editor) &&
            item.world_bounds.IsValid()) {
            snapshot->bounds.Expand(item.world_bounds);
        }
        auto& node = r().get<ecs::CNode>(entity);
        if (node.child_count == 0 && !r().any_of<ecs::CRenderable, ecs::CLight, ecs::CCamera>(entity)) {
            node.d_world_transform = item.world_transform;
            node.d_aabb            = item.world_bounds;
            node.is_dirty          = false;
        }
        snapshot->instances.push_back(std::move(item));
    }
    std::sort(snapshot->instances.begin(), snapshot->instances.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.key < rhs.key;
    });
    if (!should_force_update && m_last_gaussian_snapshot &&
        snapshot->instances.size() == m_last_gaussian_snapshot->instances.size() &&
        std::equal(
            snapshot->instances.begin(),
            snapshot->instances.end(),
            m_last_gaussian_snapshot->instances.begin(),
            Same
        ))
        return {};
    snapshot->revision       = ++m_gaussian_revision;
    m_last_gaussian_snapshot = snapshot;
    return snapshot;
}
} // namespace Moer
