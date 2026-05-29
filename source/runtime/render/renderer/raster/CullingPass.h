#pragma once

/**
 * GPU Culling Pass
 * 
 * 使用 Compute Shader 生成 pass 专属的可见实例列表和间接绘制命令。
 */

#include "math/Function.h"
#include "misc/STL.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "scene/camera/Camera.h"
#include "shader/ShaderCommon.h"
#include "shader/ShaderPipeline.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/raster/culling/ShaderParameters.h"
#include "shaderheaders/shared/scene/SharedSceneStruct.h"

#include "RasterConfig.h"
#include "RasterResource.h"

#include <cstring>

namespace Moer::Render::Raster {

/**
 * Culling Pipeline
 */
class CullPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(CullPipeline);

    // 注意：参数名必须与 shader 中的变量名一致
    DEFINE_SHADER_BUFFER(source_draw_commands);
    DEFINE_SHADER_BUFFER(primitives);
    DEFINE_SHADER_BUFFER(instances);
    DEFINE_SHADER_BUFFER(visible_instance_ids);
    DEFINE_SHADER_BUFFER(draw_commands);
    DEFINE_SHADER_BUFFER(counters);
    DEFINE_SHADER_BUFFER(cull_data);
    DEFINE_SHADER_CONSTANT_STRUCT(CullParams, cull_params);

    DEFINE_SHADER_ARGS(
        source_draw_commands,
        primitives,
        instances,
        visible_instance_ids,
        draw_commands,
        counters,
        cull_data,
        cull_params
    );
};

/**
 * Frustum Culling Pass
 */
class CullingPass {
public:
    // Stores the CPU-visible counters copied back from the GPU culling pass.
    struct CullStatistics {
        uint32_t total_instances_before = 0; // 裁剪前的总实例数
        uint32_t total_instances_after  = 0; // 裁剪后的总实例数
        uint32_t visible_draws          = 0; // 可见的 Draw 数量
        uint32_t total_draws            = 0; // 总 Draw 数量
    };

public:
    // Creates the compute pipeline used to build per-pass visibility buffers.
    CullingPass(RasterContext& context) {
        m_pso              = ShaderManager::Get().Compute<CullPipeline>("pipelines/raster/culling/Cull.comp.hlsl");
        m_cull_data_buffer = context.device.CreateBuffer<byte>(
            "Raster::GpuCulling::CullData", sizeof(CullData), EBufferUsageFlags::CONSTANT_BUFFER
        );
    }

    // Builds a visibility set from the main camera frustum.
    void Process(
        RasterContext&                    context,
        const Camera&                     camera,
        const GpuScene::Res&              gpu_scene_res,
        GpuCullingBuffers::VisibilitySet& visibility_set,
        CullStatistics*                   out_stats          = nullptr,
        std::string_view                  profile_scope_name = {}
    ) {
        const uint draw_count = gpu_scene_res.draw_cmd_buf.buf->GetNumElement();

        CullParams params{};
        CullData   data{};
        params.draw_count = draw_count;
        camera.GetPlanes(data.frustum_planes);

        Process(context, gpu_scene_res, params, data, visibility_set, out_stats, profile_scope_name);
    }

    // Builds a visibility set from an explicit view-projection matrix.
    void Process(
        RasterContext&                    context,
        const float4x4&                   view_proj,
        const GpuScene::Res&              gpu_scene_res,
        GpuCullingBuffers::VisibilitySet& visibility_set,
        CullStatistics*                   out_stats          = nullptr,
        std::string_view                  profile_scope_name = {}
    ) {
        const uint draw_count = gpu_scene_res.draw_cmd_buf.buf->GetNumElement();

        CullParams params{};
        CullData   data{};
        params.draw_count = draw_count;
        ExtractFrustumPlanes(view_proj, data.frustum_planes);

        Process(context, gpu_scene_res, params, data, visibility_set, out_stats, profile_scope_name);
    }

private:
    // Converts the raw GPU counters into UI-facing culling statistics.
    static CullStatistics ToStatistics(const GpuCullingCounterData& counters) {
        CullStatistics stats{};
        stats.total_instances_before = counters.total_instances_before;
        stats.total_instances_after  = counters.total_instances_after;
        stats.visible_draws          = counters.visible_draws;
        stats.total_draws            = counters.total_draws;
        return stats;
    }

    // Extracts normalized frustum planes from a view-projection matrix.
    static void ExtractFrustumPlanes(const float4x4& view_proj, float4 out_planes[6]) {
        out_planes[0] = view_proj.r3 + view_proj.r0; // left
        out_planes[1] = view_proj.r3 - view_proj.r0; // right
        out_planes[2] = view_proj.r3 + view_proj.r1; // bottom
        out_planes[3] = view_proj.r3 - view_proj.r1; // top
        out_planes[4] = view_proj.r2;                // near
        out_planes[5] = view_proj.r3 - view_proj.r2; // far

        for (uint i = 0; i < 6; ++i) {
            const float length = Lengthf(float3(out_planes[i]));
            out_planes[i]      = float4(Normalizef(float3(out_planes[i])), out_planes[i].w / length);
        }
    }

    // Dispatches the culling compute pass and prepares its outputs for indirect drawing.
    void Process(
        RasterContext&                    context,
        const GpuScene::Res&              gpu_scene_res,
        const CullParams&                 params,
        const CullData&                   data,
        GpuCullingBuffers::VisibilitySet& visibility_set,
        CullStatistics*                   out_stats,
        std::string_view                  profile_scope_name
    ) {
        const uint draw_count = gpu_scene_res.draw_cmd_buf.buf->GetNumElement();
        const uint instance_count =
            static_cast<uint>(gpu_scene_res.instance_buf.buf->GetByteSize() / sizeof(GInstance));

        visibility_set.EnsureCapacity(
            context.device, context.bdls, context.cmd_list, "Raster::GpuCulling", draw_count, instance_count
        );

        if (out_stats) {
            *out_stats = ToStatistics(m_readback_counters);
        }

        context.cmd_list.ClearResource(visibility_set.counter_buf->GetView(), 0u);

        if (draw_count == 0) {
            context.cmd_list.CopyFrom(
                visibility_set.counter_buf->GetView(),
                std::span<byte>(reinterpret_cast<byte*>(&m_readback_counters), sizeof(GpuCullingCounterData))
            );
            return;
        }

        const uint dispatch_count = (params.draw_count + 63) / 64;

        // CopyFrom(span) 只保存指针，不能传入栈上的 CullData
        Array<byte> cull_data_upload(sizeof(CullData));
        std::memcpy(cull_data_upload.data(), &data, sizeof(CullData));
        context.cmd_list.CopyFrom(std::move(cull_data_upload), m_cull_data_buffer->GetView());

        if (!profile_scope_name.empty()) {
            context.cmd_list.PushScopeWithTimeScope(profile_scope_name);
        }

        context.cmd_list
            .Compute(
                m_pso,
                gpu_scene_res.draw_cmd_buf.buf->GetView(),             // SRV: source_draw_commands
                gpu_scene_res.primitive_buf.buf->GetView(),            // SRV: primitives
                gpu_scene_res.instance_buf.buf->GetView(),             // SRV: instances
                visibility_set.visible_instance_id_buf.buf->GetView(), // UAV: visible_instance_ids
                visibility_set.draw_cmd_buf->GetView(),                // UAV: draw_commands
                visibility_set.counter_buf->GetView(),                 // UAV: counters
                m_cull_data_buffer->GetView(),                         // CBV: cull_data
                params                                                 // Push constant: cull_params
            )
            .Dispatch(uint3(dispatch_count, 1, 1), "Culling");

        if (!profile_scope_name.empty()) {
            context.cmd_list.PopScopeWithTimeScope();
        }

        context.cmd_list.CopyFrom(
            visibility_set.counter_buf->GetView(),
            std::span<byte>(reinterpret_cast<byte*>(&m_readback_counters), sizeof(GpuCullingCounterData))
        );
    }

private:
    CullPipeline          m_pso;
    BufferRef             m_cull_data_buffer;
    GpuCullingCounterData m_readback_counters{};
};

} // namespace Moer::Render::Raster
