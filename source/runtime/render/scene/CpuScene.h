#pragma once

/**
 * CpuScene数据
 * 
 * 从CpuScene开始，所有数据都是准备上传到Gpu中的
 * 换句话说，每个struct都要在shader中被使用
 * 因此，我们应该在SharedHeaders中定义这些struct
 */
#include "shaderheaders/shared/scene/SharedSceneStruct.h"

#include "LogicalScene.h"
#include "RenderAPI.h"
#include "rhi/RHICommandDrawData.h" // 为了准备IndirectDrawCommand
#include <entt/entt.hpp>

namespace Moer {
namespace Render {
// 前向声明 GpuScene
class GpuScene;
} // namespace Render

/**
 * CPU Scene
 * 
 * RAII，构造时初始化，析构时释放（不提供手动Initialize/Destroy/Reset接口）
 * 
 * 这个类主要负责以下2个功能：
 * - 存储所有准备上传到GPU的场景数据；
 * - 实现所有 LogicalScene -> CpuScene 的逻辑
 * 
 * 增量更新通过CTagNeedUpdate实现：
 * - 当CLight/CMaterial/CTransform被修改时，场景修改接口会添加CTagNeedUpdate标签
 * - UpdateLights/UpdateMaterials/UpdateMeshes通过view过滤出有CTagNeedUpdate的实体进行处理
 * - 处理完毕后清除CTagNeedUpdate标签
 */
class RENDER_API CpuScene {

    friend class Render::GpuScene; // 友元类GpuScene

public:
    CpuScene(ecs::LogicalScene& logical_scene);
    ~CpuScene() = default;

    CpuScene(const CpuScene&)            = delete;
    CpuScene& operator=(const CpuScene&) = delete;

    void Update();

    /**
     * 获取 Primitive ID（在 m_primitive_buf 中的索引）
     * @param primitive_entt CPrimitive 的 entity
     * @return primitive_id，如果不存在则返回无效值（需要调用者检查）
     */
    uint GetPrimitiveId(entt::entity primitive_entt) const;

    /**
     * 获取指定 Primitive 的第一个 Instance 在 m_instance_buf 中的索引
     * @param primitive_id Primitive ID
     * @return 第一个 Instance 的索引，如果不存在则返回 UINT_MAX
     */
    uint GetFirstInstanceIndex(uint primitive_id) const;

    /**
     * 获取 Primitive 数量（用于确定数组大小）
     * @return Primitive 数量
     */
    uint GetPrimitiveCount() const;

    /**
     * 获取指定 Primitive 的 Instance 数量（与 m_primitive_id_to_transform_entt_arrays[primitive_id].size() 一致）
     * @param primitive_id Primitive ID
     * @return 该 Primitive 的 Instance 数量，若 primitive_id 无效则返回 0
     */
    uint GetInstanceCountForPrimitive(uint primitive_id) const;

    /**
     * 获取指定 Primitive 下第 instance_idx 个 GInstance（只读）
     * 顺序与 m_instance_buf 中该 Primitive 的 Instance 段一致。
     * @param primitive_id Primitive ID
     * @param instance_idx 该 Primitive 内的 Instance 索引 [0, GetInstanceCountForPrimitive(primitive_id))
     * @return 对应的 GInstance 引用
     */
    const GInstance& GetInstanceForPrimitive(uint primitive_id, uint instance_idx) const;

    /**
     * 获取 Light 数量
     * @return Light 数量
     */
    uint GetLightCount() const;

private:
    ecs::LogicalScene& m_logical_scene;

private:
    /**
     * 下面的内容分为3类：数据、map缓存、逻辑
     * - 数据：存储最终上传到Gpu的数据
     * - map缓存：存储从LogicalScene的Entity ID到数据Buffer ID的映射，便于快速查找
     * - 逻辑：实现LogicalScene -> CpuScene的转换逻辑
     */

    // camera
    // TODO: camera目前是在pass中手动上传到gpu

    // light
    Array<GLight> m_light_buf;

    UnorderedMap<entt::entity, uint> m_map_light_entity_to_id;

    void InitializeLights();
    void UpdateLights();

    // material & texture
    Array<GMaterial> m_material_buf;

    UnorderedMap<entt::entity, uint> m_map_material_entity_to_id;

    // Material必须在Mesh之前初始化，因为Mesh需要Material ID
    void InitializeMaterials();
    void UpdateMaterials();

    // mesh
    Array<Render::DrawIndexedCmdData> m_draw_cmd_buf;  // 1:1 GPrimitive
    Array<GPrimitive>                 m_primitive_buf; // 1:1 GPrimitive & DrawIndexedCmdData
    // primitive_buf 与 draw_cmd_buf 是对应的，index相同则对应相同primitive
    Array<GInstance> m_instance_buf; // N:1 GPrimitive
    /**
     * 从LogicalScene中获取MegaBuffers引用
     * 
     * 这个写法实际上是非常正确的，注重生命周期管理
     */
    ecs::CtxMegaBuffers& mega_buf() {
        return m_logical_scene.r().ctx().get<ecs::CtxMegaBuffers>();
    }
    const ecs::CtxMegaBuffers& mega_buf() const {
        return m_logical_scene.r().ctx().get<const ecs::CtxMegaBuffers>();
    }

    UnorderedMap<entt::entity, uint> m_map_primitive_entity_to_id;
    Array<Array<GInstance>>          m_primitive_id_to_transform_entt_arrays;
    Array<uint>
        m_primitive_id_to_first_instance_idx; // m_primitive_id_to_transform_entt_arrays的前缀和，表示每个Primitive对应的第一个Instance在m_instance_buf中的索引

    // Mesh必须在Materials之后初始化，因为Primitive需要Material ID
    void InitializeMeshes();
    void UpdateMeshes();
};

} // namespace Moer