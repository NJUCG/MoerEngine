#pragma once

/**
 * GPU Frustum Culling Pass
 * 
 * 使用 Compute Shader 对视锥外的物体进行剔除
 * 将不可见的 draw command 的 instance_cnt 设为 0
 */

#include "math/Function.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/geometry_pass/ShaderParameters.h"

#include "RasterConfig.h"
#include "RasterResource.h"

namespace Moer::Render::Raster {

/**
 * Culling 统计结构（与 Shader 对应）
 */
struct CullStatistics {
    uint total_instances_before; // 剔除前的总 instance 数
    uint total_instances_after;  // 剔除后的总 instance 数
    uint visible_draws;          // 可见的 draw call 数量
    uint total_draws;            // 总 draw call 数量
};

/**
 * Frustum Culling 参数
 */
struct FrustumCullParams {
    float4 frustum_planes[6]; // World space frustum planes
    uint   draw_count;
    uint   _pad[3];
};

/**
 * Frustum Culling Pipeline
 */
class FrustumCullPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(FrustumCullPipeline);

    // 注意：参数名必须与 shader 中的变量名一致
    // UAV: draw_commands (可写的间接绘制命令缓冲区)
    DEFINE_SHADER_BUFFER(draw_commands);
    // SRV: primitives (只读的图元数据)
    DEFINE_SHADER_BUFFER(primitives);
    // SRV: instances (只读的实例数据)
    DEFINE_SHADER_BUFFER(instances);
    // UAV: statistics (统计结果)
    DEFINE_SHADER_BUFFER(statistics);
    // Push constant: cull_params
    DEFINE_SHADER_CONSTANT_STRUCT(FrustumCullParams, cull_params);

    DEFINE_SHADER_ARGS(draw_commands, primitives, instances, statistics, cull_params);
};

/**
 * Frustum Culling Pass
 */
class CullingPass {
public:
    // 裁剪统计信息
    struct CullStatistics {
        uint32_t total_instances_before = 0; // 裁剪前的总实例数
        uint32_t total_instances_after  = 0; // 裁剪后的总实例数
        uint32_t visible_draws          = 0; // 可见的 Draw 数量
        uint32_t total_draws            = 0; // 总 Draw 数量
    };

public:
    CullingPass(RasterContext& context) {
        auto& device = RenderDevice::Get();

        // 创建 Compute PSO
        m_pso = ShaderManager::Get().Compute<FrustumCullPipeline>(
            "pipelines/raster/culling/FrustumCull.comp.hlsl"
        );

        // 创建统计 buffer (GPU 端)
        m_statistics_buf =
            device.CreateBuffer<CullStatistics>("CullingStatistics", 1, EBufferUsageFlags::UNORDERED_ACCESS);
    }

    /**
     * 执行视锥剔除
     * 
     * @param context RasterContext
     * @param camera 相机，用于获取视锥体
     * @param gpu_scene_res GPU Scene 资源
     * @param out_stats 输出统计结果（可选，为 nullptr 则不读取）
     */
    void Process(
        RasterContext&       context,
        const Camera&        camera,
        const GpuScene::Res& gpu_scene_res,
        CullStatistics*      out_stats = nullptr
    ) {
        uint draw_count = gpu_scene_res.draw_cmd_buf.buf->GetNumElement();

        // 0. 输出上一帧的统计信息（延迟一帧 readback）
        if (out_stats) {
            *out_stats = m_readback_stats;
        }

        // 1. 清零统计 buffer
        context.cmd_list.ClearResource(m_statistics_buf->GetView(), 0u);

        // 2. 获取视锥体平面（world space）
        FrustumCullParams params;
        params.draw_count = draw_count;
        camera.GetPlanes(params.frustum_planes);

        // 3. Barrier: 转换 draw_cmd_buf 从 INDIRECT_ARGUMENT -> UNORDERED_ACCESS
        context.cmd_list.Barriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Compute,
            WriteBuffer{gpu_scene_res.draw_cmd_buf.buf->GetView(), EBufferState::UNORDERED_ACCESS}
        );

        // 4. Dispatch Compute Shader
        uint dispatch_count = (params.draw_count + 63) / 64;

        context.cmd_list
            .Compute(
                m_pso,
                gpu_scene_res.draw_cmd_buf.buf->GetView(),  // UAV: draw_commands
                gpu_scene_res.primitive_buf.buf->GetView(), // SRV: primitives
                gpu_scene_res.instance_buf.buf->GetView(),  // SRV: instances
                m_statistics_buf->GetView(),                // UAV: statistics
                params                                      // Push constant: cull_params
            )
            .Dispatch(uint3(dispatch_count, 1, 1), "FrustumCulling");

        // 5. Barrier: 转换回 INDIRECT 用于 DrawIndirect
        context.cmd_list.Barriers(
            EQueueType::Graphics,
            EQueueType::Graphics,
            EPassType::Graphics,
            WriteBuffer{gpu_scene_res.draw_cmd_buf.buf->GetView(), EBufferState::INDIRECT}
        );

        // 6. Readback 统计数据（下一帧才可读）
        context.cmd_list.CopyFrom(
            m_statistics_buf->GetView(),
            std::span<byte>(reinterpret_cast<byte*>(&m_readback_stats), sizeof(CullStatistics))
        );
    }

private:
    FrustumCullPipeline m_pso;
    BufferRef           m_statistics_buf;   // GPU 端统计 buffer
    CullStatistics      m_readback_stats{}; // CPU 端 readback 结果（延迟一帧）
};

} // namespace Moer::Render::Raster
