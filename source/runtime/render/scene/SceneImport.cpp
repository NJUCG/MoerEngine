#include "Scene.h"

#include "log/LogSystem.h"
#include "rhi/RHI.h"
#include "scene/NodeNameUtils.h"
#include "scene/loader/LoaderInterface.h"

#include <algorithm>
#include <entt/entt.hpp>
#include <filesystem>


namespace Moer {

namespace {

using EntityRemap = UnorderedMap<entt::entity, entt::entity>;

void AddUniqueEntity(Array<entt::entity>& entities, UnorderedSet<entt::entity>& seen, entt::entity entity) {
    if (seen.emplace(entity).second) {
        entities.push_back(entity);
    }
}

template<typename T>
void CollectEntities(
    const entt::registry&       registry,
    Array<entt::entity>&        entities,
    UnorderedSet<entt::entity>& seen
) {
    for (entt::entity entity : registry.view<T>()) {
        AddUniqueEntity(entities, seen, entity);
    }
}

Array<entt::entity> CollectImportEntities(const entt::registry& registry, entt::entity source_root_entt) {
    Array<entt::entity>        entities;
    UnorderedSet<entt::entity> seen;

    CollectEntities<ecs::CNode>(registry, entities, seen);
    CollectEntities<ecs::CRenderable>(registry, entities, seen);
    CollectEntities<ecs::CMesh>(registry, entities, seen);
    CollectEntities<ecs::CPrimitive>(registry, entities, seen);
    CollectEntities<ecs::CMaterial>(registry, entities, seen);
    CollectEntities<ecs::CTexture>(registry, entities, seen);
    CollectEntities<ecs::CResourceName>(registry, entities, seen);
    CollectEntities<ecs::CCamera>(registry, entities, seen);
    CollectEntities<ecs::CLight>(registry, entities, seen);
    CollectEntities<ecs::CLightDirectional>(registry, entities, seen);
    CollectEntities<ecs::CLightPoint>(registry, entities, seen);
    CollectEntities<ecs::CLightAmbient>(registry, entities, seen);
    CollectEntities<ecs::CLightEnvironment>(registry, entities, seen);

    entities.erase(
        std::remove_if(
            entities.begin(),
            entities.end(),
            [&](entt::entity entity) {
                return entity == source_root_entt || registry.all_of<ecs::CSceneMetaData>(entity);
            }
        ),
        entities.end()
    );
    return entities;
}

entt::entity RemapEntity(const EntityRemap& remap, entt::entity source_entity) {
    if (source_entity == entt::null) {
        return entt::null;
    }
    const auto it = remap.find(source_entity);
    return it == remap.end() ? entt::null : it->second;
}

ecs::CPrimitive::BufferView OffsetBufferView(ecs::CPrimitive::BufferView view, uint32 offset) {
    if (view.is_valid) {
        view.start_idx += offset;
    }
    return view;
}

void AppendMegaBuffers(entt::registry& target_registry, const entt::registry& source_registry) {
    if (!source_registry.ctx().contains<ecs::CtxMegaBuffers>()) {
        return;
    }
    if (!target_registry.ctx().contains<ecs::CtxMegaBuffers>()) {
        target_registry.ctx().emplace<ecs::CtxMegaBuffers>();
    }

    auto&       target = target_registry.ctx().get<ecs::CtxMegaBuffers>();
    const auto& source = source_registry.ctx().get<const ecs::CtxMegaBuffers>();

    target.position.insert(target.position.end(), source.position.begin(), source.position.end());
    target.packed_normal.insert(
        target.packed_normal.end(), source.packed_normal.begin(), source.packed_normal.end()
    );
    target.packed_tangent.insert(
        target.packed_tangent.end(), source.packed_tangent.begin(), source.packed_tangent.end()
    );
    target.texcoord0.insert(target.texcoord0.end(), source.texcoord0.begin(), source.texcoord0.end());
    target.index.insert(target.index.end(), source.index.begin(), source.index.end());
}

void ClearSceneSyncTags(entt::registry& registry) {
    registry.clear<ecs::CTagNeedUpdateLight>();
    registry.clear<ecs::CTagNeedUpdateMaterial>();
    registry.clear<ecs::CTagNeedUpdateTransform>();
    registry.clear<ecs::CTagNeedCreateLight>();
    registry.clear<ecs::CTagNeedCreateMaterial>();
    registry.clear<ecs::CTagNeedCreateTransform>();
    registry.clear<ecs::CTagNeedDestroyLight>();
    registry.clear<ecs::CTagNeedRebuildMesh>();
}

} // namespace

Scene::ImportSceneFromFileResult Scene::ImportSceneFromFileSync(const std::filesystem::path& file_path) {
    ImportSceneFromFileResult import_result{};

    if (!IsReady()) {
        import_result.error_message = "Cannot import scene because current scene is not ready.";
        LOG_WARNING("Scene import failed: {}", import_result.error_message);
        return import_result;
    }

    SceneLoadRequest request{};
    request.file_path                = file_path;
    request.use_state_cache          = false;
    request.use_origin_cache         = true;
    request.allow_write_origin_cache = true;

    SceneImportResult loaded_scene = LoaderInterface::LoadScene(request);
    if (!loaded_scene) {
        import_result.error_message = "Failed to load source scene.";
        LOG_WARNING("Scene import failed while loading source scene: path={}", file_path.string());
        return import_result;
    }

    auto&       target_registry = r();
    const auto& source_registry = loaded_scene.logical_scene->r();

    const entt::entity source_root_entt = loaded_scene.logical_scene->UGetRootNodeEntity();
    if (source_root_entt == entt::null || !source_registry.all_of<ecs::CNode>(source_root_entt)) {
        import_result.error_message = "Imported scene has no root node.";
        LOG_WARNING("Scene import failed: {}", import_result.error_message);
        return import_result;
    }

    const std::string        import_root_name = "Imported: " + file_path.filename().string();
    EntityWithNodeCreateInfo import_root_info{};
    import_root_info.name               = import_root_name;
    const entt::entity import_root_entt = CreateEntityWithNode(import_root_info);
    if (import_root_entt == entt::null) {
        import_result.error_message = "Failed to create import root node.";
        LOG_WARNING("Scene import failed: {}", import_result.error_message);
        return import_result;
    }

    Array<entt::entity> source_entities = CollectImportEntities(source_registry, source_root_entt);
    EntityRemap         remap;
    remap.reserve(source_entities.size() + 1);
    remap.emplace(source_root_entt, import_root_entt);

    Array<entt::entity> imported_entities;
    imported_entities.reserve(source_entities.size());
    for (entt::entity source_entity : source_entities) {
        entt::entity target_entity = target_registry.create();
        remap.emplace(source_entity, target_entity);
        imported_entities.push_back(target_entity);
    }

    uint32 position_offset       = 0;
    uint32 packed_normal_offset  = 0;
    uint32 packed_tangent_offset = 0;
    uint32 texcoord0_offset      = 0;
    uint32 index_offset          = 0;
    if (source_registry.ctx().contains<ecs::CtxMegaBuffers>()) {
        if (!target_registry.ctx().contains<ecs::CtxMegaBuffers>()) {
            target_registry.ctx().emplace<ecs::CtxMegaBuffers>();
        }
        const auto& target_mega = target_registry.ctx().get<const ecs::CtxMegaBuffers>();
        position_offset         = static_cast<uint32>(target_mega.position.size());
        packed_normal_offset    = static_cast<uint32>(target_mega.packed_normal.size());
        packed_tangent_offset   = static_cast<uint32>(target_mega.packed_tangent.size());
        texcoord0_offset        = static_cast<uint32>(target_mega.texcoord0.size());
        index_offset            = static_cast<uint32>(target_mega.index.size());
    }

    for (entt::entity source_entity : source_entities) {
        entt::entity target_entity = RemapEntity(remap, source_entity);

        if (source_registry.all_of<ecs::CNode>(source_entity)) {
            ecs::CNode node        = source_registry.get<ecs::CNode>(source_entity);
            node.parent_entt       = RemapEntity(remap, node.parent_entt);
            node.prev_sibling_entt = RemapEntity(remap, node.prev_sibling_entt);
            node.next_sibling_entt = RemapEntity(remap, node.next_sibling_entt);
            node.first_child_entt  = RemapEntity(remap, node.first_child_entt);
            node.last_child_entt   = RemapEntity(remap, node.last_child_entt);
            node.depth += target_registry.get<ecs::CNode>(import_root_entt).depth;
            node.is_dirty = true;
            target_registry.emplace<ecs::CNode>(target_entity, node);
        }
        if (source_registry.all_of<ecs::CCamera>(source_entity)) {
            target_registry.emplace<ecs::CCamera>(
                target_entity, source_registry.get<ecs::CCamera>(source_entity)
            );
        }
        if (source_registry.all_of<ecs::CLight>(source_entity)) {
            target_registry.emplace<ecs::CLight>(
                target_entity, source_registry.get<ecs::CLight>(source_entity)
            );
        }
        if (source_registry.all_of<ecs::CLightDirectional>(source_entity)) {
            ecs::CLightDirectional light = source_registry.get<ecs::CLightDirectional>(source_entity);
            light.is_dirty               = true;
            target_registry.emplace<ecs::CLightDirectional>(target_entity, light);
        }
        if (source_registry.all_of<ecs::CLightPoint>(source_entity)) {
            ecs::CLightPoint light = source_registry.get<ecs::CLightPoint>(source_entity);
            light.is_dirty         = true;
            target_registry.emplace<ecs::CLightPoint>(target_entity, light);
        }
        if (source_registry.all_of<ecs::CLightAmbient>(source_entity)) {
            target_registry.emplace<ecs::CLightAmbient>(
                target_entity, source_registry.get<ecs::CLightAmbient>(source_entity)
            );
        }
        if (source_registry.all_of<ecs::CLightEnvironment>(source_entity)) {
            ecs::CLightEnvironment environment = source_registry.get<ecs::CLightEnvironment>(source_entity);
            environment.env_map_entt           = RemapEntity(remap, environment.env_map_entt);
            target_registry.emplace<ecs::CLightEnvironment>(target_entity, environment);
        }
        if (source_registry.all_of<ecs::CMaterial>(source_entity)) {
            ecs::CMaterial material              = source_registry.get<ecs::CMaterial>(source_entity);
            material.normal_map_entt             = RemapEntity(remap, material.normal_map_entt);
            material.ao_map_entt                 = RemapEntity(remap, material.ao_map_entt);
            material.albedo_map_entt             = RemapEntity(remap, material.albedo_map_entt);
            material.emissive_map_entt           = RemapEntity(remap, material.emissive_map_entt);
            material.metallic_roughness_map_entt = RemapEntity(remap, material.metallic_roughness_map_entt);
            target_registry.emplace<ecs::CMaterial>(target_entity, material);
        }
        if (source_registry.all_of<ecs::CTexture>(source_entity)) {
            target_registry.emplace<ecs::CTexture>(
                target_entity, source_registry.get<ecs::CTexture>(source_entity)
            );
        }
        if (source_registry.all_of<ecs::CResourceName>(source_entity)) {
            target_registry.emplace<ecs::CResourceName>(
                target_entity, source_registry.get<ecs::CResourceName>(source_entity)
            );
        }
        if (source_registry.all_of<ecs::CPrimitive>(source_entity)) {
            ecs::CPrimitive primitive = source_registry.get<ecs::CPrimitive>(source_entity);
            primitive.position        = OffsetBufferView(primitive.position, position_offset);
            primitive.packed_normal   = OffsetBufferView(primitive.packed_normal, packed_normal_offset);
            primitive.packed_tangent  = OffsetBufferView(primitive.packed_tangent, packed_tangent_offset);
            primitive.texcoord0       = OffsetBufferView(primitive.texcoord0, texcoord0_offset);
            primitive.index           = OffsetBufferView(primitive.index, index_offset);
            primitive.material_entt   = RemapEntity(remap, primitive.material_entt);
            target_registry.emplace<ecs::CPrimitive>(target_entity, primitive);
        }
        if (source_registry.all_of<ecs::CMesh>(source_entity)) {
            ecs::CMesh mesh = source_registry.get<ecs::CMesh>(source_entity);
            for (entt::entity& primitive_entt : mesh.primitive_entts) {
                primitive_entt = RemapEntity(remap, primitive_entt);
            }
            target_registry.emplace<ecs::CMesh>(target_entity, std::move(mesh));
        }
        if (source_registry.all_of<ecs::CRenderable>(source_entity)) {
            ecs::CRenderable renderable = source_registry.get<ecs::CRenderable>(source_entity);
            renderable.mesh_entt        = RemapEntity(remap, renderable.mesh_entt);
            target_registry.emplace<ecs::CRenderable>(target_entity, renderable);
        }

        ecs::EnsureNodeName(target_registry, target_entity);
    }

    auto&       import_root_node      = target_registry.get<ecs::CNode>(import_root_entt);
    const auto& source_root_node      = source_registry.get<ecs::CNode>(source_root_entt);
    import_root_node.first_child_entt = RemapEntity(remap, source_root_node.first_child_entt);
    import_root_node.last_child_entt  = RemapEntity(remap, source_root_node.last_child_entt);
    import_root_node.child_count      = source_root_node.child_count;
    import_root_node.is_dirty         = true;

    AppendMegaBuffers(target_registry, source_registry);

    logical_scene().SBuildPrimitiveHash();
    logical_scene().SBuildMeshHash();
    logical_scene().SBuildMeshAABB();
    logical_scene().SUpdateAllNodeTransformAndAABB();
    logical_scene().SUpdateAllLightData();

    // Import 会带来新的 texture/material entities，但当前增量同步链路不会在 GpuScene 中创建这些纹理。
    // 这里直接按最新 logical scene 重建一次 runtime scene，确保材质纹理句柄立即有效。
    Render::RenderDevice::Get().WaitIdle();
    m_cpu_scene = MakeUnique<CpuScene>(*m_logical_scene);
    m_gpu_scene = MakeUnique<Render::GpuScene>(*m_cpu_scene, bindless_array());
    m_has_pending_gpu_scene_commands = true;
    ClearSceneSyncTags(target_registry);

    import_result.success               = true;
    import_result.import_root_entt      = import_root_entt;
    import_result.imported_entity_count = static_cast<uint64>(imported_entities.size());

    LOG_INFO(
        "Scene imported: path={}, import_root={}, entities={}",
        file_path.string(),
        static_cast<uint32>(entt::to_integral(import_root_entt)),
        import_result.imported_entity_count
    );
    return import_result;
}

} // namespace Moer