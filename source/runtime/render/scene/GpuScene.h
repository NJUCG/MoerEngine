#pragma once

#include "GpuSceneUpdate.h"
#include "RenderAPI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {

/**
 * GpuScene 持有真正的 GPU 资源，是 renderer 读取场景数据的入口。
 *
 * 结构:
 * - Res: 纹理、buffer、ray tracing scene
 * - m_map_texture_key_to_bindless_handle: 稳定纹理 key -> bindless handle
 * - PendingCommandList: copy/gfx queue 待执行命令
 *
 * 改这里:
 * - 加新的 GPU 资源或 bindless 绑定: GpuScene.h / GpuScene.cpp
 * - 改 CPU->GPU 数据布局: CpuScene.h + SharedSceneStruct.h + 各 pass
 * - 改 import / runtime create 后的资源重建: SceneLifecycle.cpp + GpuScene.cpp
 *
 * 用法:
 * - RenderScene 在渲染线程应用 GpuSceneUpdate 后取 PopPendingCommandList() 提交给 RHI
 * - renderer/pass 只读 res() / GetRaytracingScene()
 */
class RENDER_API GpuScene {

public:
    explicit GpuScene(BindlessArrayRef bindless_array);
    ~GpuScene() noexcept;

    GpuScene(const GpuScene&)            = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    void ApplyUpdate(GpuSceneUpdate&& update);

private:
    /**
     * MARK: Private Update Functions
     */

    // 同步 CPU light cache 到 GPU light buffer，必要时重建 bindless buffer。
    void UpdateLightBuffer(CommandList& cmd_list, const Array<GLight>& lights);

    // 同步 CPU material cache 到 GPU material buffer，必要时重建 bindless buffer。
    void UpdateMaterialBuffer(
        CommandList&                              cmd_list,
        Array<GMaterial>&                         materials,
        const Array<GpuSceneMaterialTextureRefs>& texture_refs
    );

    // 同步 CPU instance cache 到 GPU instance buffer，必要时重建 bindless buffer。
    void UpdateInstanceBuffer(CommandList& cmd_list, const Array<GInstance>& instances);

    // 同步 CPU draw command cache 到 GPU draw command buffer，必要时重建 bindless buffer。
    void UpdateDrawCommandBuffer(CommandList& cmd_list, const Array<DrawIndexedCmdData>& draw_commands);

    // 同步 CPU primitive cache 和 mega buffers，必要时重建 bindless buffer。
    void UpdatePrimitiveBuffer(CommandList& cmd_list, const Array<GPrimitive>& primitives);
    void UpdateClusterGroupBuffer(CommandList& cmd_list, const Array<GClusterGroup>& cluster_groups);
    void UpdatePositionMegaBuffer(CommandList& cmd_list, const Array<float3>& positions);
    void UpdatePackedNormalMegaBuffer(CommandList& cmd_list, const Array<uint32>& packed_normals);
    void UpdatePackedTangentMegaBuffer(CommandList& cmd_list, const Array<uint32>& packed_tangents);
    void UpdateTexcoord0MegaBuffer(CommandList& cmd_list, const Array<float2>& texcoords0);
    void UpdateIndexMegaBuffer(CommandList& cmd_list, const Array<uint32>& indices);

    void InitializeResources(GpuSceneUpdate& update);
    void ResolveMaterialTextureHandles(
        Array<GMaterial>&                         materials,
        const Array<GpuSceneMaterialTextureRefs>& texture_refs
    ) const;

public:
    /**
     * 便于抛出GpuScene Res接口
     */
    struct Res {
        // texture
        Array<TextureWithHandle> texture_array;

        // light
        BufferWithHandle light_buf;

        // material
        BufferWithHandle material_buf;

        // mesh
        BufferWithHandle draw_cmd_buf;
        BufferWithHandle primitive_buf;
        BufferWithHandle instance_buf;

        // mega buffers
        BufferWithHandle position_buf;
        BufferWithHandle packed_normal_buf;
        BufferWithHandle packed_tangent_buf;
        BufferWithHandle texcoord0_buf;

        BufferWithHandle index_buf;

        // raytracing scene
        RaytracingSceneRef rt_scene;

        // Cluster LOD Group buffer
        BufferWithHandle cluster_group_buf;          // GClusterGroup[]，LOD 运行时选择数据

        // RT 专用（mesh-level BLAS 方案）
        BufferWithHandle rt_instance_buf;           // GRtInstance[]，per-renderable
        BufferWithHandle rt_primitive_table_buf;     // uint[]，GeometryIndex → primitive_id 映射表
    };

    struct PendingCommandList {
        CommandList copy_queue_cmd_list;
        CommandList gfx_queue_cmd_list;
    };

    /**
     * GpuScene::Res
     * 
     * 以只读形式抛出所有gpu资源
     */
    const Res& res() const {
        return m_res;
    }

    /**
     * Bindless Array 引用
     */
    BindlessArrayRef bindless_array() {
        return m_bindless_array;
    }

    PendingCommandList&& PopPendingCommandList() {
        return std::move(m_pending_cmd_lists);
    }

    /**
     * MARK: Raytracing Scene
     * 
     * 初始化 Raytracing Scene，创建所有 BLAS 和 instance
     */
    void InitRaytracingScene(
        CommandList&                         cmd_list,
        const Array<GpuSceneRtMeshData>&     meshes,
        const Array<GpuSceneRtInstanceData>& instances
    );

    /**
     * 更新 Raytracing Scene，更新所有 instance 的 transform
     */
    void UpdateRaytracingScene(
        CommandList&                             cmd_list,
        const Array<GpuSceneRtInstanceData>& instances
    );

    void RebuildRaytracingSceneTlas(
        CommandList&                             cmd_list,
        const Array<GpuSceneRtInstanceData>& instances
    );

    /**
     * 获取 Raytracing Scene 引用
     */
    RaytracingSceneRef GetRaytracingScene() const {
        return m_res.rt_scene;
    }

    /**
     * 恢复 Draw Commands 到原始状态（从 CPU 数据重新上传）
     * 用于 GPU Culling 关闭时恢复原始 instance_cnt
     */
    void RestoreDrawCommands(CommandList& cmd_list);

private:
    Res              m_res;
    BindlessArrayRef m_bindless_array; // TODO: 移动到RenderDevice里？

    UnorderedMap<GpuSceneResourceKey, uint> m_map_texture_key_to_bindless_handle;

    // RT Scene Cache: BLAS 按 CMesh 存储（1 CMesh = 1 BLAS，每个 CPrimitive 对应一个 geometry）
    UnorderedMap<GpuSceneResourceKey, RaytracingGeometryRef> m_mesh_key_to_blas;

    // CPU 侧映射缓存，用于 TLAS-only rebuild / update 时复用
    Array<GRtInstance> m_rt_instance_cache;
    Array<uint32>      m_rt_primitive_table_cache;

    // 每个 mesh_entt 在 m_rt_primitive_table_cache 中的起始偏移
    UnorderedMap<GpuSceneResourceKey, uint32> m_mesh_key_to_primitive_table_offset;

    Array<DrawIndexedCmdData> m_draw_commands;

    // 记录场景上传命令，交给 renderer 统一提交。
    PendingCommandList m_pending_cmd_lists;
};

} // namespace Moer::Render
