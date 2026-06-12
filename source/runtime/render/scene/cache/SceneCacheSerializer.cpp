#include "scene/cache/SceneCacheSerializer.h"

#include "log/LogSystem.h"
#include "scene/LogicalScene.h"
#include "scene/NodeNameUtils.h"

#include <entt/entt.hpp>
#include <filesystem>
#include <limits>
#include <utility>

namespace Moer {

namespace {

constexpr uint32 k_logical_scene_payload_version = 5; // v5: parent_group_id in ClusterGroupInfo
constexpr uint64 k_null_entity_id                = std::numeric_limits<uint64>::max();

enum ESceneCacheComponentFlag : uint64 {
    SceneCacheComponentName          = 1ull << 0,
    SceneCacheComponentNode          = 1ull << 1,
    SceneCacheComponentSceneMeta     = 1ull << 2,
    SceneCacheComponentCamera        = 1ull << 3,
    SceneCacheComponentLight         = 1ull << 4,
    SceneCacheComponentDirectional   = 1ull << 5,
    SceneCacheComponentPoint         = 1ull << 6,
    SceneCacheComponentAmbient       = 1ull << 7,
    SceneCacheComponentEnvironment   = 1ull << 8,
    SceneCacheComponentMaterial      = 1ull << 9,
    SceneCacheComponentTexture       = 1ull << 10,
    SceneCacheComponentPrimitive     = 1ull << 11,
    SceneCacheComponentMesh          = 1ull << 12,
    SceneCacheComponentRenderable    = 1ull << 13,
    SceneCacheComponentRootNodeTag   = 1ull << 14,
    SceneCacheComponentMainCameraTag = 1ull << 15,
    SceneCacheComponentMainLightTag  = 1ull << 16,
    SceneCacheComponentResourceName  = 1ull << 17,
};

struct LogicalScenePayloadHeader {
    uint32 version          = k_logical_scene_payload_version;
    uint32 has_mega_buffers = 0;
    uint64 entity_count     = 0;
    uint64 reserved[4]      = {};
};

struct SerializedEntityHeader {
    uint64 local_id        = 0;
    uint64 component_flags = 0;
};

using EntityToLocalIdMap = UnorderedMap<entt::entity, uint64>;

// 判断组件标记里是否包含指定 bit
bool HasFlag(uint64 flags, ESceneCacheComponentFlag flag) {
    return (flags & static_cast<uint64>(flag)) != 0;
}

// 向实体列表追加一个尚未收集过的实体
void AddUniqueEntity(
    Array<entt::entity>&        entities,
    UnorderedSet<entt::entity>& seen_entities,
    entt::entity                entity
) {
    if (seen_entities.emplace(entity).second) {
        entities.emplace_back(entity);
    }
}

// 把某类组件上的实体收集到待序列化列表
template<typename T>
void CollectComponentEntities(
    const entt::registry&       registry,
    Array<entt::entity>&        entities,
    UnorderedSet<entt::entity>& seen_entities
) {
    const auto view = registry.view<T>();
    for (entt::entity entity : view) {
        AddUniqueEntity(entities, seen_entities, entity);
    }
}

void CollectNamedResourceEntities(
    const entt::registry&       registry,
    Array<entt::entity>&        entities,
    UnorderedSet<entt::entity>& seen_entities
) {
    for (entt::entity entity : registry.view<ecs::CResourceName>()) {
        const auto& resource_name = registry.get<ecs::CResourceName>(entity);
        if (!ecs::IsBlankName(resource_name.name)) {
            AddUniqueEntity(entities, seen_entities, entity);
        }
    }
}

// 汇总所有需要写入 cache 的实体
Array<entt::entity> CollectSerializableEntities(const entt::registry& registry) {
    Array<entt::entity>        entities;
    UnorderedSet<entt::entity> seen_entities;

    CollectComponentEntities<ecs::CSceneMetaData>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CTagRootNode>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CNode>(registry, entities, seen_entities);
    CollectNamedResourceEntities(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CRenderable>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CMesh>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CPrimitive>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CMaterial>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CTexture>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CCamera>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CLight>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CLightDirectional>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CLightPoint>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CLightAmbient>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CLightEnvironment>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CTagMainCamera>(registry, entities, seen_entities);
    CollectComponentEntities<ecs::CTagMainLight>(registry, entities, seen_entities);

    return entities;
}

// 直接写入一个原始值
template<typename T>
bool WriteRawValue(SceneCacheBinaryWriter& writer, const T& value) {
    return writer.WriteBytes(&value, sizeof(T));
}

// 直接读取一个原始值
template<typename T>
bool ReadRawValue(SceneCacheBinaryReader& reader, T& out_value) {
    return reader.ReadBytes(&out_value, sizeof(T));
}

// 以长度前缀写入原始数组
template<typename T>
bool WriteRawArray(SceneCacheBinaryWriter& writer, const Array<T>& values) {
    const uint64 count = static_cast<uint64>(values.size());
    if (!writer.WritePod(count)) {
        return false;
    }
    if (values.empty()) {
        return true;
    }
    return writer.WriteBytes(values.data(), static_cast<uint64>(values.size() * sizeof(T)));
}

// 以长度前缀读取原始数组
template<typename T>
bool ReadRawArray(SceneCacheBinaryReader& reader, Array<T>& out_values) {
    uint64 count = 0;
    if (!reader.ReadPod(count)) {
        return false;
    }
    if (count > static_cast<uint64>(std::numeric_limits<size_t>::max())) {
        LOG_WARNING("Scene Cache load failed: raw array is too large for this platform.");
        return false;
    }

    out_values.resize(static_cast<size_t>(count));
    if (out_values.empty()) {
        return true;
    }
    return reader.ReadBytes(out_values.data(), static_cast<uint64>(out_values.size() * sizeof(T)));
}

// 记录一次序列化阶段的语义错误
bool LogSceneCacheSaveFailure(std::string_view reason) {
    LOG_WARNING("Scene Cache save failed: {}", reason);
    return false;
}

// 记录一次反序列化阶段的语义错误
bool LogSceneCacheLoadFailure(std::string_view reason) {
    LOG_WARNING("Scene Cache load failed: {}", reason);
    return false;
}

// 把实体引用映射为本地 id 并写入
bool WriteEntityRef(
    SceneCacheBinaryWriter&   writer,
    const EntityToLocalIdMap& entity_to_local_id,
    entt::entity              entity
) {
    uint64 local_id = k_null_entity_id;
    if (entity != entt::null) {
        const auto it = entity_to_local_id.find(entity);
        if (it == entity_to_local_id.end()) {
            return LogSceneCacheSaveFailure(
                "Scene cache tried to serialize an entity reference that is not in the scene."
            );
        }
        local_id = it->second;
    }
    return writer.WritePod(local_id);
}

// 读取本地 id 并还原为实体引用
bool ReadEntityRef(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    entt::entity&              out_entity
) {
    uint64 local_id = k_null_entity_id;
    if (!reader.ReadPod(local_id)) {
        return false;
    }
    if (local_id == k_null_entity_id) {
        out_entity = entt::null;
        return true;
    }
    if (local_id >= local_id_to_entity.size()) {
        return LogSceneCacheLoadFailure(
            "Scene cache contains an entity reference outside the local entity table."
        );
    }
    out_entity = local_id_to_entity[static_cast<size_t>(local_id)];
    return true;
}

// 根据实体当前组件生成序列化标记
uint64 BuildComponentFlags(const entt::registry& registry, entt::entity entity) {
    uint64 flags = 0;
    if (registry.all_of<ecs::CNode>(entity) && !ecs::IsBlankName(registry.get<ecs::CNode>(entity).name)) {
        flags |= SceneCacheComponentName;
    }
    if (registry.all_of<ecs::CResourceName>(entity) &&
        !ecs::IsBlankName(registry.get<ecs::CResourceName>(entity).name)) {
        flags |= SceneCacheComponentResourceName;
    }
    if (registry.all_of<ecs::CNode>(entity)) {
        flags |= SceneCacheComponentNode;
    }
    if (registry.all_of<ecs::CSceneMetaData>(entity)) {
        flags |= SceneCacheComponentSceneMeta;
    }
    if (registry.all_of<ecs::CCamera>(entity)) {
        flags |= SceneCacheComponentCamera;
    }
    if (registry.all_of<ecs::CLight>(entity)) {
        flags |= SceneCacheComponentLight;
    }
    if (registry.all_of<ecs::CLightDirectional>(entity)) {
        flags |= SceneCacheComponentDirectional;
    }
    if (registry.all_of<ecs::CLightPoint>(entity)) {
        flags |= SceneCacheComponentPoint;
    }
    if (registry.all_of<ecs::CLightAmbient>(entity)) {
        flags |= SceneCacheComponentAmbient;
    }
    if (registry.all_of<ecs::CLightEnvironment>(entity)) {
        flags |= SceneCacheComponentEnvironment;
    }
    if (registry.all_of<ecs::CMaterial>(entity)) {
        flags |= SceneCacheComponentMaterial;
    }
    if (registry.all_of<ecs::CTexture>(entity)) {
        flags |= SceneCacheComponentTexture;
    }
    if (registry.all_of<ecs::CPrimitive>(entity)) {
        flags |= SceneCacheComponentPrimitive;
    }
    if (registry.all_of<ecs::CMesh>(entity)) {
        flags |= SceneCacheComponentMesh;
    }
    if (registry.all_of<ecs::CRenderable>(entity)) {
        flags |= SceneCacheComponentRenderable;
    }
    if (registry.all_of<ecs::CTagRootNode>(entity)) {
        flags |= SceneCacheComponentRootNodeTag;
    }
    if (registry.all_of<ecs::CTagMainCamera>(entity)) {
        flags |= SceneCacheComponentMainCameraTag;
    }
    if (registry.all_of<ecs::CTagMainLight>(entity)) {
        flags |= SceneCacheComponentMainLightTag;
    }
    return flags;
}

// 写入节点组件及其层级引用
bool WriteNode(
    SceneCacheBinaryWriter&   writer,
    const EntityToLocalIdMap& entity_to_local_id,
    const ecs::CNode&         node
) {
    return WriteEntityRef(writer, entity_to_local_id, node.parent_entt) &&
           WriteEntityRef(writer, entity_to_local_id, node.prev_sibling_entt) &&
           WriteEntityRef(writer, entity_to_local_id, node.next_sibling_entt) &&
           WriteEntityRef(writer, entity_to_local_id, node.first_child_entt) &&
           WriteEntityRef(writer, entity_to_local_id, node.last_child_entt) &&
           writer.WritePod(node.child_count) && writer.WritePod(node.depth) &&
           WriteRawValue(writer, node.translation) && WriteRawValue(writer, node.rotation) &&
           WriteRawValue(writer, node.scale);
}

// 读取节点组件及其层级引用
bool ReadNode(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    ecs::CNode&                out_node
) {
    return ReadEntityRef(reader, local_id_to_entity, out_node.parent_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_node.prev_sibling_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_node.next_sibling_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_node.first_child_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_node.last_child_entt) &&
           reader.ReadPod(out_node.child_count) && reader.ReadPod(out_node.depth) &&
           ReadRawValue(reader, out_node.translation) && ReadRawValue(reader, out_node.rotation) &&
           ReadRawValue(reader, out_node.scale);
}

// 写入场景元数据组件
bool WriteSceneMetaData(
    SceneCacheBinaryWriter&    writer,
    const EntityToLocalIdMap&  entity_to_local_id,
    const ecs::CSceneMetaData& meta_data
) {
    return WriteEntityRef(writer, entity_to_local_id, meta_data.root_node_entt) &&
           writer.WriteString(meta_data.scene_path);
}

// 读取场景元数据组件
bool ReadSceneMetaData(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    ecs::CSceneMetaData&       out_meta_data
) {
    return ReadEntityRef(reader, local_id_to_entity, out_meta_data.root_node_entt) &&
           reader.ReadString(out_meta_data.scene_path);
}

// 写入方向光组件
bool WriteLightDirectional(SceneCacheBinaryWriter& writer, const ecs::CLightDirectional& light) {
    return WriteRawValue(writer, light.color) && writer.WritePod(light.intensity);
}

// 读取方向光组件
bool ReadLightDirectional(SceneCacheBinaryReader& reader, ecs::CLightDirectional& out_light) {
    out_light.is_dirty = true;
    return ReadRawValue(reader, out_light.color) && reader.ReadPod(out_light.intensity);
}

// 写入点光组件
bool WriteLightPoint(SceneCacheBinaryWriter& writer, const ecs::CLightPoint& light) {
    return WriteRawValue(writer, light.color) && writer.WritePod(light.intensity);
}

// 读取点光组件
bool ReadLightPoint(SceneCacheBinaryReader& reader, ecs::CLightPoint& out_light) {
    out_light.is_dirty = true;
    return ReadRawValue(reader, out_light.color) && reader.ReadPod(out_light.intensity);
}

// 写入环境光组件
bool WriteLightAmbient(SceneCacheBinaryWriter& writer, const ecs::CLightAmbient& light) {
    return WriteRawValue(writer, light.color) && writer.WritePod(light.intensity);
}

// 读取环境光组件
bool ReadLightAmbient(SceneCacheBinaryReader& reader, ecs::CLightAmbient& out_light) {
    return ReadRawValue(reader, out_light.color) && reader.ReadPod(out_light.intensity);
}

// 写入环境贴图引用
bool WriteLightEnvironment(
    SceneCacheBinaryWriter&       writer,
    const EntityToLocalIdMap&     entity_to_local_id,
    const ecs::CLightEnvironment& environment
) {
    return WriteEntityRef(writer, entity_to_local_id, environment.env_map_entt);
}

// 读取环境贴图引用
bool ReadLightEnvironment(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    ecs::CLightEnvironment&    out_environment
) {
    return ReadEntityRef(reader, local_id_to_entity, out_environment.env_map_entt);
}

// 写入材质组件及其贴图引用
bool WriteMaterial(
    SceneCacheBinaryWriter&   writer,
    const EntityToLocalIdMap& entity_to_local_id,
    const ecs::CMaterial&     material
) {
    return WriteEntityRef(writer, entity_to_local_id, material.normal_map_entt) &&
           WriteEntityRef(writer, entity_to_local_id, material.ao_map_entt) &&
           WriteEntityRef(writer, entity_to_local_id, material.albedo_map_entt) &&
           WriteEntityRef(writer, entity_to_local_id, material.emissive_map_entt) &&
           WriteEntityRef(writer, entity_to_local_id, material.metallic_roughness_map_entt) &&
           WriteRawValue(writer, material.albedo_factor) && WriteRawValue(writer, material.emissive_factor) &&
           writer.WritePod(material.metallic_factor) && writer.WritePod(material.roughness_factor) &&
           writer.WritePod(material.alpha_mode) && writer.WritePod(material.alpha_cutoff);
}

// 读取材质组件及其贴图引用
bool ReadMaterial(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    ecs::CMaterial&            out_material
) {
    return ReadEntityRef(reader, local_id_to_entity, out_material.normal_map_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_material.ao_map_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_material.albedo_map_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_material.emissive_map_entt) &&
           ReadEntityRef(reader, local_id_to_entity, out_material.metallic_roughness_map_entt) &&
           ReadRawValue(reader, out_material.albedo_factor) &&
           ReadRawValue(reader, out_material.emissive_factor) &&
           reader.ReadPod(out_material.metallic_factor) && reader.ReadPod(out_material.roughness_factor) &&
           reader.ReadPod(out_material.alpha_mode) && reader.ReadPod(out_material.alpha_cutoff);
}

// 写入纹理像素数据与描述
bool WriteTexture(SceneCacheBinaryWriter& writer, const ecs::CTexture& texture) {
    return writer.WriteArray(texture.data) && writer.WritePod(texture.format) &&
           writer.WritePod(texture.width) && writer.WritePod(texture.height) &&
           writer.WritePod(texture.mip_level_count) && writer.WritePod(texture.array_layer_count);
}

// 读取纹理像素数据与描述
bool ReadTexture(SceneCacheBinaryReader& reader, ecs::CTexture& out_texture) {
    return reader.ReadArray(out_texture.data) && reader.ReadPod(out_texture.format) &&
           reader.ReadPod(out_texture.width) && reader.ReadPod(out_texture.height) &&
           reader.ReadPod(out_texture.mip_level_count) && reader.ReadPod(out_texture.array_layer_count);
}

bool WriteResourceName(SceneCacheBinaryWriter& writer, const ecs::CResourceName& resource_name) {
    return writer.WriteString(resource_name.name);
}

bool ReadResourceName(SceneCacheBinaryReader& reader, ecs::CResourceName& out_resource_name) {
    return reader.ReadString(out_resource_name.name);
}

// 写入 primitive 组件及材质引用
bool WritePrimitive(
    SceneCacheBinaryWriter&   writer,
    const EntityToLocalIdMap& entity_to_local_id,
    const ecs::CPrimitive&    primitive
) {
    return writer.WritePod(primitive.vertex_count) && writer.WritePod(primitive.position) &&
           writer.WritePod(primitive.packed_normal) && writer.WritePod(primitive.packed_tangent) &&
           writer.WritePod(primitive.texcoord0) && writer.WritePod(primitive.index_count) &&
           writer.WritePod(primitive.index) && WriteRawValue(writer, primitive.aabb) &&
           WriteEntityRef(writer, entity_to_local_id, primitive.material_entt) &&
           writer.WritePod(primitive.cluster_group_id) &&
           writer.WritePod(primitive.cluster_refined_id);
}

// 读取 primitive 组件及材质引用
bool ReadPrimitive(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    ecs::CPrimitive&           out_primitive
) {
    return reader.ReadPod(out_primitive.vertex_count) && reader.ReadPod(out_primitive.position) &&
           reader.ReadPod(out_primitive.packed_normal) && reader.ReadPod(out_primitive.packed_tangent) &&
           reader.ReadPod(out_primitive.texcoord0) && reader.ReadPod(out_primitive.index_count) &&
           reader.ReadPod(out_primitive.index) && ReadRawValue(reader, out_primitive.aabb) &&
           ReadEntityRef(reader, local_id_to_entity, out_primitive.material_entt) &&
           reader.ReadPod(out_primitive.cluster_group_id) &&
           reader.ReadPod(out_primitive.cluster_refined_id);
}

// 写入 mesh 的 primitive 引用列表
bool WriteMesh(
    SceneCacheBinaryWriter&   writer,
    const EntityToLocalIdMap& entity_to_local_id,
    const ecs::CMesh&         mesh
) {
    const uint64 count = static_cast<uint64>(mesh.primitive_entts.size());
    if (!writer.WritePod(count)) {
        return false;
    }
    for (entt::entity primitive_entt : mesh.primitive_entts) {
        if (!WriteEntityRef(writer, entity_to_local_id, primitive_entt)) {
            return false;
        }
    }
    // Cluster LOD data
    if (!writer.WritePod(mesh.num_leaf_clusters)) return false;
    const uint64 group_count = static_cast<uint64>(mesh.cluster_groups.size());
    if (!writer.WritePod(group_count)) return false;
    for (const auto& g : mesh.cluster_groups) {
        if (!WriteRawValue(writer, g.simplified_center) ||
            !writer.WritePod(g.simplified_radius) ||
            !writer.WritePod(g.simplified_error) ||
            !writer.WritePod(g.depth) ||
            !writer.WritePod(g.parent_group_id)) {
            return false;
        }
    }
    return true;
}

// 读取 mesh 的 primitive 引用列表
bool ReadMesh(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    ecs::CMesh&                out_mesh
) {
    uint64 count = 0;
    if (!reader.ReadPod(count)) {
        return false;
    }
    if (count > static_cast<uint64>(std::numeric_limits<size_t>::max())) {
        return LogSceneCacheLoadFailure("Scene cache mesh primitive list is too large for this platform.");
    }

    out_mesh.primitive_entts.resize(static_cast<size_t>(count));
    for (entt::entity& primitive_entt : out_mesh.primitive_entts) {
        if (!ReadEntityRef(reader, local_id_to_entity, primitive_entt)) {
            return false;
        }
    }
    // Cluster LOD data
    if (!reader.ReadPod(out_mesh.num_leaf_clusters)) return false;
    uint64 group_count = 0;
    if (!reader.ReadPod(group_count)) return false;
    out_mesh.cluster_groups.resize(static_cast<size_t>(group_count));
    for (auto& g : out_mesh.cluster_groups) {
        if (!ReadRawValue(reader, g.simplified_center) ||
            !reader.ReadPod(g.simplified_radius) ||
            !reader.ReadPod(g.simplified_error) ||
            !reader.ReadPod(g.depth) ||
            !reader.ReadPod(g.parent_group_id)) {
            return false;
        }
    }
    return true;
}

// 写入 renderable 的 mesh 引用
bool WriteRenderable(
    SceneCacheBinaryWriter&   writer,
    const EntityToLocalIdMap& entity_to_local_id,
    const ecs::CRenderable&   renderable
) {
    return WriteEntityRef(writer, entity_to_local_id, renderable.mesh_entt);
}

// 读取 renderable 的 mesh 引用
bool ReadRenderable(
    SceneCacheBinaryReader&    reader,
    const Array<entt::entity>& local_id_to_entity,
    ecs::CRenderable&          out_renderable
) {
    return ReadEntityRef(reader, local_id_to_entity, out_renderable.mesh_entt);
}

// 写入 mega buffer 数据块
bool WriteMegaBuffers(SceneCacheBinaryWriter& writer, const ecs::CtxMegaBuffers& mega_buffers) {
    return WriteRawArray(writer, mega_buffers.position) && writer.WriteArray(mega_buffers.packed_normal) &&
           writer.WriteArray(mega_buffers.packed_tangent) && WriteRawArray(writer, mega_buffers.texcoord0) &&
           writer.WriteArray(mega_buffers.index);
}

// 读取 mega buffer 数据块
bool ReadMegaBuffers(SceneCacheBinaryReader& reader, ecs::CtxMegaBuffers& out_mega_buffers) {
    return ReadRawArray(reader, out_mega_buffers.position) &&
           reader.ReadArray(out_mega_buffers.packed_normal) &&
           reader.ReadArray(out_mega_buffers.packed_tangent) &&
           ReadRawArray(reader, out_mega_buffers.texcoord0) && reader.ReadArray(out_mega_buffers.index);
}

// 把单个实体的所有可序列化组件写入 payload
bool WriteEntityPayload(
    SceneCacheBinaryWriter&   writer,
    const entt::registry&     registry,
    const EntityToLocalIdMap& entity_to_local_id,
    entt::entity              entity,
    uint64                    local_id
) {
    const uint64                 component_flags = BuildComponentFlags(registry, entity);
    const SerializedEntityHeader entity_header{
        .local_id        = local_id,
        .component_flags = component_flags,
    };

    if (!writer.WritePod(entity_header)) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentName) &&
        !writer.WriteString(registry.get<ecs::CNode>(entity).name)) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentResourceName) &&
        !WriteResourceName(writer, registry.get<ecs::CResourceName>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentNode) &&
        !WriteNode(writer, entity_to_local_id, registry.get<ecs::CNode>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentSceneMeta) &&
        !WriteSceneMetaData(writer, entity_to_local_id, registry.get<ecs::CSceneMetaData>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentCamera) &&
        !WriteRawValue(writer, registry.get<ecs::CCamera>(entity).camera)) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentLight) &&
        !writer.WritePod(registry.get<ecs::CLight>(entity).type)) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentDirectional) &&
        !WriteLightDirectional(writer, registry.get<ecs::CLightDirectional>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentPoint) &&
        !WriteLightPoint(writer, registry.get<ecs::CLightPoint>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentAmbient) &&
        !WriteLightAmbient(writer, registry.get<ecs::CLightAmbient>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentEnvironment) &&
        !WriteLightEnvironment(writer, entity_to_local_id, registry.get<ecs::CLightEnvironment>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentMaterial) &&
        !WriteMaterial(writer, entity_to_local_id, registry.get<ecs::CMaterial>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentTexture) &&
        !WriteTexture(writer, registry.get<ecs::CTexture>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentPrimitive) &&
        !WritePrimitive(writer, entity_to_local_id, registry.get<ecs::CPrimitive>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentMesh) &&
        !WriteMesh(writer, entity_to_local_id, registry.get<ecs::CMesh>(entity))) {
        return false;
    }
    if (HasFlag(component_flags, SceneCacheComponentRenderable) &&
        !WriteRenderable(writer, entity_to_local_id, registry.get<ecs::CRenderable>(entity))) {
        return false;
    }

    return true;
}

// 从 payload 读取单个实体的所有可序列化组件
bool ReadEntityPayload(
    SceneCacheBinaryReader&    reader,
    entt::registry&            registry,
    const Array<entt::entity>& local_id_to_entity,
    Array<bool>&               seen_entities
) {
    SerializedEntityHeader entity_header{};
    if (!reader.ReadPod(entity_header)) {
        return false;
    }
    if (entity_header.local_id >= local_id_to_entity.size()) {
        return LogSceneCacheLoadFailure("Scene cache entity local id is outside the local entity table.");
    }

    const size_t local_index = static_cast<size_t>(entity_header.local_id);
    if (seen_entities[local_index]) {
        return LogSceneCacheLoadFailure("Scene cache contains a duplicated entity local id.");
    }
    seen_entities[local_index] = true;

    const entt::entity entity          = local_id_to_entity[local_index];
    const uint64       component_flags = entity_header.component_flags;
    std::string        serialized_node_name;
    ecs::CResourceName serialized_resource_name{};

    if (HasFlag(component_flags, SceneCacheComponentName)) {
        if (!reader.ReadString(serialized_node_name)) {
            return false;
        }
    }
    if (HasFlag(component_flags, SceneCacheComponentResourceName)) {
        if (!ReadResourceName(reader, serialized_resource_name)) {
            return false;
        }
    }
    if (HasFlag(component_flags, SceneCacheComponentNode)) {
        ecs::CNode node{};
        if (!ReadNode(reader, local_id_to_entity, node)) {
            return false;
        }
        node.is_dirty = true;
        node.name     = std::move(serialized_node_name);
        registry.emplace<ecs::CNode>(entity, node);
    }
    if (HasFlag(component_flags, SceneCacheComponentResourceName)) {
        registry.emplace<ecs::CResourceName>(entity, std::move(serialized_resource_name));
    }
    if (HasFlag(component_flags, SceneCacheComponentSceneMeta)) {
        ecs::CSceneMetaData meta_data{};
        if (!ReadSceneMetaData(reader, local_id_to_entity, meta_data)) {
            return false;
        }
        registry.emplace<ecs::CSceneMetaData>(entity, std::move(meta_data));
    }
    if (HasFlag(component_flags, SceneCacheComponentCamera)) {
        auto& camera = registry.emplace<ecs::CCamera>(entity);
        if (!ReadRawValue(reader, camera.camera)) {
            return false;
        }
    }
    if (HasFlag(component_flags, SceneCacheComponentLight)) {
        auto& light = registry.emplace<ecs::CLight>(entity);
        if (!reader.ReadPod(light.type)) {
            return false;
        }
    }
    if (HasFlag(component_flags, SceneCacheComponentDirectional)) {
        ecs::CLightDirectional light{};
        if (!ReadLightDirectional(reader, light)) {
            return false;
        }
        registry.emplace<ecs::CLightDirectional>(entity, light);
    }
    if (HasFlag(component_flags, SceneCacheComponentPoint)) {
        ecs::CLightPoint light{};
        if (!ReadLightPoint(reader, light)) {
            return false;
        }
        registry.emplace<ecs::CLightPoint>(entity, light);
    }
    if (HasFlag(component_flags, SceneCacheComponentAmbient)) {
        auto& light = registry.emplace<ecs::CLightAmbient>(entity);
        if (!ReadLightAmbient(reader, light)) {
            return false;
        }
    }
    if (HasFlag(component_flags, SceneCacheComponentEnvironment)) {
        ecs::CLightEnvironment environment{};
        if (!ReadLightEnvironment(reader, local_id_to_entity, environment)) {
            return false;
        }
        registry.emplace<ecs::CLightEnvironment>(entity, environment);
    }
    if (HasFlag(component_flags, SceneCacheComponentMaterial)) {
        ecs::CMaterial material{};
        if (!ReadMaterial(reader, local_id_to_entity, material)) {
            return false;
        }
        registry.emplace<ecs::CMaterial>(entity, material);
    }
    if (HasFlag(component_flags, SceneCacheComponentTexture)) {
        ecs::CTexture texture{};
        if (!ReadTexture(reader, texture)) {
            return false;
        }
        registry.emplace<ecs::CTexture>(entity, std::move(texture));
    }
    if (HasFlag(component_flags, SceneCacheComponentPrimitive)) {
        ecs::CPrimitive primitive{};
        if (!ReadPrimitive(reader, local_id_to_entity, primitive)) {
            return false;
        }
        registry.emplace<ecs::CPrimitive>(entity, primitive);
    }
    if (HasFlag(component_flags, SceneCacheComponentMesh)) {
        ecs::CMesh mesh{};
        if (!ReadMesh(reader, local_id_to_entity, mesh)) {
            return false;
        }
        registry.emplace<ecs::CMesh>(entity, std::move(mesh));
    }
    if (HasFlag(component_flags, SceneCacheComponentRenderable)) {
        ecs::CRenderable renderable{};
        if (!ReadRenderable(reader, local_id_to_entity, renderable)) {
            return false;
        }
        registry.emplace<ecs::CRenderable>(entity, renderable);
    }
    if (HasFlag(component_flags, SceneCacheComponentRootNodeTag)) {
        registry.emplace<ecs::CTagRootNode>(entity);
    }
    if (HasFlag(component_flags, SceneCacheComponentMainCameraTag)) {
        registry.emplace<ecs::CTagMainCamera>(entity);
    }
    if (HasFlag(component_flags, SceneCacheComponentMainLightTag)) {
        registry.emplace<ecs::CTagMainLight>(entity);
    }

    ecs::EnsureNodeName(registry, entity);

    return true;
}

} // namespace

// 打开一个用于写 cache 的二进制流
SceneCacheBinaryWriter::SceneCacheBinaryWriter(const std::filesystem::path& file_path) :
    m_stream(file_path, std::ios::binary | std::ios::trunc) {
    if (!m_stream.is_open()) {
        RecordError("Failed to open scene cache file for writing: " + file_path.string());
    }
}

// 关闭 writer 持有的文件流
SceneCacheBinaryWriter::~SceneCacheBinaryWriter() = default;

// 返回 writer 当前是否可继续写入
bool SceneCacheBinaryWriter::IsValid() const {
    return m_error.empty() && m_stream.is_open() && m_stream.good();
}

// 返回 writer 记录的首个错误
const std::string& SceneCacheBinaryWriter::GetError() const {
    return m_error;
}

// 写入一段原始字节
bool SceneCacheBinaryWriter::WriteBytes(const void* data, uint64 size) {
    if (!IsValid()) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (!data) {
        RecordError("Attempted to write null data to scene cache.");
        return false;
    }

    m_stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!m_stream.good()) {
        RecordError("Failed to write bytes to scene cache.");
        return false;
    }
    return true;
}

// 以长度前缀写入字符串
bool SceneCacheBinaryWriter::WriteString(std::string_view value) {
    const uint64 size = static_cast<uint64>(value.size());
    if (!WritePod(size)) {
        return false;
    }
    if (value.empty()) {
        return true;
    }
    return WriteBytes(value.data(), size);
}

// 移动写指针到指定偏移
bool SceneCacheBinaryWriter::Seek(uint64 position) {
    if (!IsValid()) {
        return false;
    }
    m_stream.seekp(static_cast<std::streamoff>(position), std::ios::beg);
    if (!m_stream.good()) {
        RecordError("Failed to seek scene cache writer.");
        return false;
    }
    return true;
}

// 刷新底层输出流
bool SceneCacheBinaryWriter::Flush() {
    if (!IsValid()) {
        return false;
    }
    m_stream.flush();
    if (!m_stream.good()) {
        RecordError("Failed to flush scene cache writer.");
        return false;
    }
    return true;
}

// 返回当前写指针位置
uint64 SceneCacheBinaryWriter::Tell() {
    if (!IsValid()) {
        return 0;
    }
    const std::streampos position = m_stream.tellp();
    if (position == std::streampos(-1)) {
        RecordError("Failed to query scene cache writer position.");
        return 0;
    }
    return static_cast<uint64>(position);
}

// 记录首个 writer 错误
void SceneCacheBinaryWriter::RecordError(std::string message) {
    if (m_error.empty()) {
        m_error = std::move(message);
    }
}

// 打开一个用于读 cache 的二进制流
SceneCacheBinaryReader::SceneCacheBinaryReader(const std::filesystem::path& file_path) :
    m_stream(file_path, std::ios::binary) {
    if (!m_stream.is_open()) {
        RecordError("Failed to open scene cache file for reading: " + file_path.string());
    }
}

// 关闭 reader 持有的文件流
SceneCacheBinaryReader::~SceneCacheBinaryReader() = default;

// 返回 reader 当前是否可继续读取
bool SceneCacheBinaryReader::IsValid() const {
    return m_error.empty() && m_stream.is_open() && m_stream.good();
}

// 返回 reader 记录的首个错误
const std::string& SceneCacheBinaryReader::GetError() const {
    return m_error;
}

// 读取一段原始字节
bool SceneCacheBinaryReader::ReadBytes(void* out_data, uint64 size) {
    if (!IsValid()) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    if (!out_data) {
        RecordError("Attempted to read scene cache bytes into null output.");
        return false;
    }

    m_stream.read(reinterpret_cast<char*>(out_data), static_cast<std::streamsize>(size));
    if (m_stream.gcount() != static_cast<std::streamsize>(size)) {
        RecordError("Scene cache ended before all requested bytes were read.");
        return false;
    }
    return true;
}

// 读取一个长度前缀字符串
bool SceneCacheBinaryReader::ReadString(std::string& out_value) {
    uint64 size = 0;
    if (!ReadPod(size)) {
        return false;
    }
    if (size > static_cast<uint64>(std::numeric_limits<size_t>::max())) {
        RecordError("Scene cache string is too large for this platform.");
        return false;
    }

    out_value.resize(static_cast<size_t>(size));
    if (out_value.empty()) {
        return true;
    }
    return ReadBytes(out_value.data(), size);
}

// 记录首个 reader 错误
void SceneCacheBinaryReader::RecordError(std::string message) {
    if (m_error.empty()) {
        m_error = std::move(message);
    }
}

// 把 LogicalScene 写成 cache 文件
bool SceneCacheSerializer::SaveLogicalScene(
    const std::filesystem::path& cache_path,
    SceneCacheHeader             header,
    const ecs::LogicalScene&     logical_scene
) {
    std::error_code             ec;
    const std::filesystem::path parent_path = cache_path.parent_path();
    if (!parent_path.empty()) {
        std::filesystem::create_directories(parent_path, ec);
        if (ec) {
            LOG_WARNING(
                "Scene Cache save failed: path={}, step=create_directories, directory={}, "
                "filesystem_error={}",
                cache_path.string(),
                parent_path.string(),
                ec.message()
            );
            return false;
        }
    }

    SceneCacheBinaryWriter writer(cache_path);
    if (!writer.IsValid()) {
        LOG_WARNING("Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError());
        return false;
    }

    header.header_size  = sizeof(SceneCacheHeader);
    header.payload_size = 0;
    if (!writer.WritePod(header)) {
        LOG_WARNING("Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError());
        return false;
    }

    const entt::registry& registry = logical_scene.r();
    Array<entt::entity>   entities = CollectSerializableEntities(registry);

    EntityToLocalIdMap entity_to_local_id;
    entity_to_local_id.reserve(entities.size());
    for (size_t index = 0; index < entities.size(); ++index) {
        entity_to_local_id.emplace(entities[index], static_cast<uint64>(index));
    }

    const uint64 payload_start = writer.Tell();
    if (!writer.IsValid()) {
        LOG_WARNING("Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError());
        return false;
    }

    const LogicalScenePayloadHeader payload_header{
        .version          = k_logical_scene_payload_version,
        .has_mega_buffers = registry.ctx().contains<ecs::CtxMegaBuffers>() ? 1u : 0u,
        .entity_count     = static_cast<uint64>(entities.size()),
        .reserved         = {},
    };
    if (!writer.WritePod(payload_header)) {
        LOG_WARNING("Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError());
        return false;
    }

    for (size_t index = 0; index < entities.size(); ++index) {
        if (!WriteEntityPayload(
                writer, registry, entity_to_local_id, entities[index], static_cast<uint64>(index)
            )) {
            if (!writer.GetError().empty()) {
                LOG_WARNING(
                    "Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError()
                );
            } else {
                LOG_WARNING(
                    "Scene Cache save failed: path={}, reason=failed to write entity payload, local_id={}",
                    cache_path.string(),
                    index
                );
            }
            return false;
        }
    }

    if (payload_header.has_mega_buffers != 0) {
        if (!WriteMegaBuffers(writer, registry.ctx().get<const ecs::CtxMegaBuffers>())) {
            LOG_WARNING(
                "Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError()
            );
            return false;
        }
    }

    const uint64 payload_end = writer.Tell();
    if (!writer.IsValid()) {
        LOG_WARNING("Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError());
        return false;
    }

    header.payload_size = payload_end - payload_start;
    if (!writer.Seek(0) || !writer.WritePod(header) || !writer.Flush()) {
        LOG_WARNING("Scene Cache save failed: path={}, reason={}", cache_path.string(), writer.GetError());
        return false;
    }

    return true;
}

// 从 cache 文件还原一个 LogicalScene
bool SceneCacheSerializer::LoadLogicalScene(
    const std::filesystem::path& cache_path,
    ecs::LogicalScene&           logical_scene,
    SceneCacheHeader*            out_header
) {
    SceneCacheBinaryReader reader(cache_path);
    if (!reader.IsValid()) {
        LOG_WARNING("Scene Cache load failed: path={}, reason={}", cache_path.string(), reader.GetError());
        return false;
    }

    SceneCacheHeader header{};
    if (!reader.ReadPod(header)) {
        LOG_WARNING("Scene Cache load failed: path={}, reason={}", cache_path.string(), reader.GetError());
        return false;
    }
    if (out_header) {
        *out_header = header;
    }

    LogicalScenePayloadHeader payload_header{};
    if (!reader.ReadPod(payload_header)) {
        LOG_WARNING("Scene Cache load failed: path={}, reason={}", cache_path.string(), reader.GetError());
        return false;
    }
    if (payload_header.version != k_logical_scene_payload_version) {
        LOG_WARNING(
            "Scene Cache load failed: path={}, reason=Scene cache logical scene payload version mismatch.",
            cache_path.string()
        );
        return false;
    }
    if (payload_header.entity_count > static_cast<uint64>(std::numeric_limits<size_t>::max())) {
        LOG_WARNING(
            "Scene Cache load failed: path={}, reason=Scene cache entity table is too large for this "
            "platform.",
            cache_path.string()
        );
        return false;
    }

    logical_scene.r()        = entt::registry{};
    entt::registry& registry = logical_scene.r();

    Array<entt::entity> local_id_to_entity;
    local_id_to_entity.resize(static_cast<size_t>(payload_header.entity_count));
    for (entt::entity& entity : local_id_to_entity) {
        entity = registry.create();
    }

    Array<bool> seen_entities(local_id_to_entity.size(), false);
    for (uint64 index = 0; index < payload_header.entity_count; ++index) {
        if (!ReadEntityPayload(reader, registry, local_id_to_entity, seen_entities)) {
            if (!reader.GetError().empty()) {
                LOG_WARNING(
                    "Scene Cache load failed: path={}, reason={}", cache_path.string(), reader.GetError()
                );
            } else {
                LOG_WARNING(
                    "Scene Cache load failed: path={}, reason=failed to read entity payload, local_id={}",
                    cache_path.string(),
                    index
                );
            }
            return false;
        }
    }

    if (payload_header.has_mega_buffers != 0) {
        ecs::CtxMegaBuffers mega_buffers{};
        if (!ReadMegaBuffers(reader, mega_buffers)) {
            LOG_WARNING(
                "Scene Cache load failed: path={}, reason={}", cache_path.string(), reader.GetError()
            );
            return false;
        }
        registry.ctx().emplace<ecs::CtxMegaBuffers>(std::move(mega_buffers));
    }

    logical_scene.SBuildPrimitiveHash();
    logical_scene.SBuildMeshHash();
    logical_scene.SBuildMeshAABB();
    logical_scene.Update();
    return true;
}

} // namespace Moer
