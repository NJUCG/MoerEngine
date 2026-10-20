#include "Scene.h"
#include "loader/GaussianPlyLoader.h"
#include "math/Transform.h"

#include <algorithm>
#include <cmath>

namespace Moer {
namespace {
bool Same(const GaussianInstanceSnapshot& a, const GaussianInstanceSnapshot& b) {
    if (a.key != b.key || a.asset != b.asset || a.visible_in_game != b.visible_in_game ||
        a.visible_in_editor != b.visible_in_editor || a.settings.enabled != b.settings.enabled ||
        a.settings.opacity_multiplier != b.settings.opacity_multiplier ||
        a.settings.color_multiplier.x != b.settings.color_multiplier.x ||
        a.settings.color_multiplier.y != b.settings.color_multiplier.y ||
        a.settings.color_multiplier.z != b.settings.color_multiplier.z)
        return false;
    return std::equal(
        std::begin(a.world_transform.e), std::end(a.world_transform.e), std::begin(b.world_transform.e)
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

EGaussianSceneContent Scene::GetGaussianSceneContent() const {
    if (!IsReady())
        return EGaussianSceneContent::Empty;
    bool mesh = false, gaussian = false;
    for (auto entity : r().view<const ecs::CRenderable, const ecs::CNode>()) {
        if (GetNodeVisibility(entity).effectively_visible_in_game) {
            mesh = true;
            break;
        }
    }
    for (auto entity : r().view<const ecs::CGaussianInstance, const ecs::CNode>()) {
        const auto& instance = r().get<ecs::CGaussianInstance>(entity);
        if (instance.asset && !instance.asset->gaussians.empty() && instance.settings.enabled &&
            GetNodeVisibility(entity).effectively_visible_in_game) {
            gaussian = true;
            break;
        }
    }
    if (mesh && gaussian)
        return EGaussianSceneContent::Mixed;
    if (gaussian)
        return EGaussianSceneContent::GaussianOnly;
    return mesh ? EGaussianSceneContent::MeshOnly : EGaussianSceneContent::Empty;
}

std::shared_ptr<const GaussianSceneSnapshot> Scene::CaptureGaussianUpdate(bool force) {
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
        auto current = entity;
        while (current != entt::null) {
            const auto& node = r().get<ecs::CNode>(current);
            item.world_transform =
                Transform(node.translation, node.scale, node.rotation).GetMatrix4x4() * item.world_transform;
            current = node.parent_entt;
        }
        const auto& local_bounds = instance.asset->bounds;
        if (local_bounds.IsValid()) {
            const Transform transform(item.world_transform);
            for (int corner = 0; corner < 8; ++corner) {
                item.world_bounds.Expand(
                    transform * float3(
                                    (corner & 1) ? local_bounds.max.x : local_bounds.min.x,
                                    (corner & 2) ? local_bounds.max.y : local_bounds.min.y,
                                    (corner & 4) ? local_bounds.max.z : local_bounds.min.z
                                )
                );
            }
        }
        const auto visibility  = GetNodeVisibility(entity);
        item.visible_in_game   = visibility.effectively_visible_in_game;
        item.visible_in_editor = visibility.effectively_visible_in_editor;
        if (item.settings.enabled && (item.visible_in_game || item.visible_in_editor) &&
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
    std::sort(snapshot->instances.begin(), snapshot->instances.end(), [](const auto& a, const auto& b) {
        return a.key < b.key;
    });
    if (!force && m_last_gaussian_snapshot &&
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
