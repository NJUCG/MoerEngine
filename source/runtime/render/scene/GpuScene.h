#pragma once

#include "CpuScene.h"

#include "RHIResource.h"

namespace Moer::Render {

// TODO: 与Raster中对应类型合并

struct BufferWithHandle {
    BufferRef buf;
    uint      hdl;
};

struct TextureWithHandle {
    TextureRef tex;
    uint       hdl;
};

/**
 * GPU Scene
 * 
 * RAII，构造时初始化，析构时释放（不提供手动Initialize/Destroy/Reset接口）
 * 
 * TODO: 析构函数（资源释放）
 */
class GpuScene {

public:
    GpuScene(CpuScene& cpu_scene, BindlessArrayRef bindless_array);
    ~GpuScene() = default;

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

private:
    ecs::LogicalScene& m_logical_scene;
    CpuScene&          m_cpu_scene;

private:
    Res              m_res;
    BindlessArrayRef m_bindless_array; // TODO: 移动到RenderDevice里？

    UnorderedMap<entt::entity, uint> m_map_texture_entity_to_bindless_handle;
};

} // namespace Moer::Render