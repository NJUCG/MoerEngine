#pragma once

#include "CpuScene.h"
#include "RenderAPI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {

/**
 * GpuScene 持有真正的 GPU 资源，是 renderer 读取场景数据的入口。
 *
 * 结构:
 * - Res: 纹理、buffer、ray tracing scene
 * - m_map_texture_entity_to_bindless_handle: 逻辑纹理 -> bindless handle
 * - PendingCommandList: copy/gfx queue 待执行命令
 *
 * 改这里:
 * - 加新的 GPU 资源或 bindless 绑定: GpuScene.h / GpuScene.cpp
 * - 改 CPU->GPU 数据布局: CpuScene.h + SharedSceneStruct.h + 各 pass
 * - 改 import / runtime create 后的资源重建: SceneLifeCycle.cpp + GpuScene.cpp
 *
 * 用法:
 * - Scene::Tick 后取 PopPendingCommandList() 提交给 RHI
 * - renderer/pass 只读 res() / GetRaytracingScene()
 */
class RENDER_API GpuScene {

public:
    GpuScene(CpuScene& cpu_scene, BindlessArrayRef bindless_array);
    ~GpuScene() noexcept;

    GpuScene(const GpuScene&)            = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    void Update(const ecs::LogicalScene& logical_scene, CpuScene& cpu_scene, bool rebuilt_mesh);

private:
    /**
     * MARK: Private Update Functions
     */

    // 同步 CPU light cache 到 GPU light buffer，必要时重建 bindless buffer。
    void UpdateLightBuffer(CommandList& cmd_list);

    // 同步 CPU material cache 到 GPU material buffer，必要时重建 bindless buffer。
    void UpdateMaterialBuffer(CommandList& cmd_list);

    // 同步 CPU instance cache 到 GPU instance buffer，必要时重建 bindless buffer。
    void UpdateInstanceBuffer(CommandList& cmd_list);

    // 同步 CPU draw command cache 到 GPU draw command buffer，必要时重建 bindless buffer。
    void UpdateDrawCommandBuffer(CommandList& cmd_list);

    // 同步 CPU primitive cache 和 mega buffers，必要时重建 bindless buffer。
    void UpdatePrimitiveBuffer(CommandList& cmd_list);
    void UpdatePositionMegaBuffer(CommandList& cmd_list);
    void UpdatePackedNormalMegaBuffer(CommandList& cmd_list);
    void UpdatePackedTangentMegaBuffer(CommandList& cmd_list);
    void UpdateTexcoord0MegaBuffer(CommandList& cmd_list);
    void UpdateIndexMegaBuffer(CommandList& cmd_list);

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
    void InitRaytracingScene(CommandList& cmd_list);

    /**
     * 更新 Raytracing Scene，更新所有 instance 的 transform
     */
    void UpdateRaytracingScene(CommandList& cmd_list);

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
    ecs::LogicalScene& m_logical_scene;
    CpuScene&          m_cpu_scene;

private:
    Res              m_res;
    BindlessArrayRef m_bindless_array; // TODO: 移动到RenderDevice里？

    UnorderedMap<entt::entity, uint> m_map_texture_entity_to_bindless_handle;

    // Raytracing Scene Cache: BLAS 按 primitive_id 顺序存储，与 CpuScene 的 primitive 顺序一致
    Array<RaytracingGeometryRef> m_primitive_id_to_blas;

    // 将gfx queue的数据存下来，等待主线程执行
    PendingCommandList m_pending_cmd_lists;
};

} // namespace Moer::Render