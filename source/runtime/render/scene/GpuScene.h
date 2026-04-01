#pragma once

#include "CpuScene.h"
#include "RenderAPI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {

/**
 * GPU Scene
 * 
 * RAII，构造时初始化，析构时释放（不提供手动Initialize/Destroy/Reset接口）
 * 
 * TODO: 析构函数（资源释放）
 */
class RENDER_API GpuScene {

public:
    GpuScene(CpuScene& cpu_scene, BindlessArrayRef bindless_array);
    ~GpuScene() noexcept;

    GpuScene(const GpuScene&)            = delete;
    GpuScene& operator=(const GpuScene&) = delete;

    void Update(const ecs::LogicalScene& logical_scene, CpuScene& cpu_scene);

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