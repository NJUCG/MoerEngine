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
#include "CullingReadbackState.h"
#include "RasterResource.h"

#include <algorithm>
#include <cstring>
#include <memory>

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
    DEFINE_SHADER_BUFFER(cluster_groups);
    DEFINE_SHADER_CONSTANT_STRUCT(CullParams, cull_params);

    DEFINE_SHADER_ARGS(
        source_draw_commands,
        primitives,
        instances,
        visible_instance_ids,
        draw_commands,
        counters,
        cull_data,
        cluster_groups,
        cull_params
    );
};

class HiZOcclusionCullPipeline : public ComputePipeline {
public:
    DEFINE_COMPUTE_PIPELINE_CLASS(HiZOcclusionCullPipeline);

    // 注意：参数名必须与 shader 中的变量名一致
    DEFINE_SHADER_BUFFER(source_draw_commands);
    DEFINE_SHADER_BUFFER(primitives);
    DEFINE_SHADER_BUFFER(instances);
    DEFINE_SHADER_BUFFER(visible_instance_ids);
    DEFINE_SHADER_BUFFER(draw_commands);
    DEFINE_SHADER_BUFFER(counters);
    DEFINE_SHADER_BUFFER(cull_data);
    DEFINE_SHADER_BUFFER(cluster_groups);
    DEFINE_SHADER_BINDLESS_ARRAY(bdls);
    DEFINE_SHADER_CONSTANT_STRUCT(CullParams, cull_params);

    DEFINE_SHADER_ARGS(
        source_draw_commands,
        primitives,
        instances,
        visible_instance_ids,
        draw_commands,
        counters,
        cull_data,
        cluster_groups,
        bdls,
        cull_params
    );

    MUTATION_BOOL(ENABLE_HIZ_OCCLUSION);
    MUTATION_SET(MutationSet, ENABLE_HIZ_OCCLUSION);
};

/**
 * Frustum Culling Pass
 */
class CullingPass {
public:
    struct CullingOptions {
        bool  enable_frustum_culling;
        bool  enable_hiz_occlusion;
        float lod_error_threshold;
        int   force_lod_level;     // -1 = auto, 0 = leaf, 1+ = 简化层级
    };

    // Stores the CPU-visible counters copied back from the GPU culling pass.
    struct CullStatistics {
        uint32_t total_instances_before     = 0; // 裁剪前的总实例数
        uint32_t total_instances_after      = 0; // 裁剪后的总实例数
        uint32_t visible_draws              = 0; // 可见的 Draw 数量
        uint32_t total_draws                = 0; // 总 Draw 数量
        uint32_t frustum_culled_instances   = 0; // 被视锥剔除的实例数
        uint32_t occlusion_culled_instances = 0; // 被Hi-Z遮挡剔除的实例数
        uint32_t lod_culled_instances       = 0; // 被 Cluster LOD 剔除的实例数
    };

public:
    // Creates the compute pipeline used to build per-pass visibility buffers.
    CullingPass(RasterContext& context) {
        m_pso = ShaderManager::Get().Compute<CullPipeline>("pipelines/raster/culling/Cull.comp.hlsl");

        HiZOcclusionCullPipeline::MutationSet occlusion_mutation_set{};
        occlusion_mutation_set.SetMutation<HiZOcclusionCullPipeline::ENABLE_HIZ_OCCLUSION>(true);
        m_hiz_occlusion_pso = ShaderManager::Get().Compute<HiZOcclusionCullPipeline>(
            "pipelines/raster/culling/Cull.comp.hlsl", occlusion_mutation_set
        );

        m_cull_data_buffer = context.device.CreateBuffer<byte>(
            "Raster::GpuCulling::CullData", sizeof(CullData), EBufferUsageFlags::CONSTANT_BUFFER
        );

        m_cluster_group_dummy_buf = context.device.CreateBuffer<byte>(
            "Raster::GpuCulling::ClusterGroupDummy",
            sizeof(GClusterGroup),
            EBufferUsageFlags::UNORDERED_ACCESS
        );
    }

    // Builds a visibility set from the main camera frustum.
    void Process(
        RasterContext&                    context,
        const Camera&                     camera,
        const GpuScene::Res&              gpu_scene_res,
        GpuCullingBuffers::VisibilitySet& visibility_set,
        CullStatistics*                   out_stats          = nullptr,
        std::string_view                  profile_scope_name = {},
        CullingOptions                    options            = {true, false, 1.0f, -1}
    ) {
        const uint draw_count = gpu_scene_res.draw_cmd_buf.buf->GetNumElement();

        CullParams params{};
        CullData   data{};
        params.draw_count = draw_count;
        camera.GetPlanes(data.frustum_planes);

        if (options.enable_frustum_culling) {
            params.flags |= CULL_FLAG_ENABLE_FRUSTUM;
        }

        FillHiZOcclusionParams(context, options, params, data);

        const float viewport_height = static_cast<float>(context.textures.depth_linear_sampler.GetSize().y);
        FillClusterLodParams(camera, gpu_scene_res, options, viewport_height, params, data);

        Process(context, gpu_scene_res, params, data, visibility_set, out_stats, profile_scope_name);
    }

    // Builds a visibility set from an explicit view-projection matrix (shadow pass).
    // LOD 数据不由此路径填充，shader 将回退到叶子 cluster 渲染。
    void Process(
        RasterContext&                    context,
        const float4x4&                   view_proj,
        const GpuScene::Res&              gpu_scene_res,
        GpuCullingBuffers::VisibilitySet& visibility_set,
        CullStatistics*                   out_stats          = nullptr,
        std::string_view                  profile_scope_name = {},
        CullingOptions                    options            = {true, false, 1.0f, -1}
    ) {
        const uint draw_count = gpu_scene_res.draw_cmd_buf.buf->GetNumElement();

        CullParams params{};
        CullData   data{};
        params.draw_count = draw_count;
        ExtractFrustumPlanes(view_proj, data.frustum_planes);

        if (options.enable_frustum_culling) {
            params.flags |= CULL_FLAG_ENABLE_FRUSTUM;
        }

        FillHiZOcclusionParams(context, options, params, data);

        Process(context, gpu_scene_res, params, data, visibility_set, out_stats, profile_scope_name);
    }

    // Builds one conservative visibility set for an axis-aligned world-space volume.
    // Point-shadow multiview uses this cube as the union of its six 90-degree frusta.
    void ProcessAabb(
        RasterContext&                    context,
        const float3&                     bounds_min,
        const float3&                     bounds_max,
        const GpuScene::Res&              gpu_scene_res,
        GpuCullingBuffers::VisibilitySet& visibility_set,
        CullStatistics*                   out_stats          = nullptr,
        std::string_view                  profile_scope_name = {},
        CullingOptions                    options            = {true, false, 1.0f, -1}
    ) {
        const uint draw_count = gpu_scene_res.draw_cmd_buf.buf->GetNumElement();

        CullParams params{};
        CullData   data{};
        params.draw_count = draw_count;
        data.frustum_planes[0] = float4(1.0f, 0.0f, 0.0f, -bounds_min.x);
        data.frustum_planes[1] = float4(-1.0f, 0.0f, 0.0f, bounds_max.x);
        data.frustum_planes[2] = float4(0.0f, 1.0f, 0.0f, -bounds_min.y);
        data.frustum_planes[3] = float4(0.0f, -1.0f, 0.0f, bounds_max.y);
        data.frustum_planes[4] = float4(0.0f, 0.0f, 1.0f, -bounds_min.z);
        data.frustum_planes[5] = float4(0.0f, 0.0f, -1.0f, bounds_max.z);

        if (options.enable_frustum_culling) {
            params.flags |= CULL_FLAG_ENABLE_FRUSTUM;
        }

        FillHiZOcclusionParams(context, options, params, data);

        Process(context, gpu_scene_res, params, data, visibility_set, out_stats, profile_scope_name);
    }

private:
    // Converts the raw GPU counters into UI-facing culling statistics.
    static CullStatistics ToStatistics(const GpuCullingCounterData& counters) {
        CullStatistics stats{};
        stats.total_instances_before     = counters.total_instances_before;
        stats.total_instances_after      = counters.total_instances_after;
        stats.visible_draws              = counters.visible_draws;
        stats.total_draws                = counters.total_draws;
        stats.frustum_culled_instances   = counters.frustum_culled_instances;
        stats.occlusion_culled_instances = counters.occlusion_culled_instances;
        stats.lod_culled_instances       = counters.lod_culled_instances;
        return stats;
    }

    static bool CanUseHiZOcclusion(const RasterContext& context, const CullingOptions& options) {
        return options.enable_hiz_occlusion && context.hiz_data.previous_valid &&
               context.textures.hiz_previous.tex != nullptr && context.hiz_data.mip_count > 0;
    }

    // LOD 过滤始终执行（Nanite 模型：LOD 选择是正确性要求，不是可选优化）。
    // 当 cluster_group_buf 不存在时跳过（shader 通过 cluster_group_id < 0 回退到全部渲染）。
    static void FillClusterLodParams(
        const Camera&        camera,
        const GpuScene::Res& gpu_scene_res,
        const CullingOptions& options,
        float                viewport_height,
        CullParams&          params,
        CullData&            data
    ) {
        if (gpu_scene_res.cluster_group_buf.buf == nullptr) {
            return;
        }

        params.flags |= CULL_FLAG_ENABLE_CLUSTER_LOD;
        data.camera_position      = camera.GetPosition();
        data.camera_znear         = camera.GetNearClip();
        data.camera_proj_11       = camera.GetProjectionMatrix()[1][1];
        // clusterlod.h 的 ComputeScreenError 返回归一化屏幕空间 [0,1]，
        // UI 中阈值单位为像素，需除以视口高度转换为归一化单位
        data.lod_error_threshold  = options.lod_error_threshold / std::max(viewport_height, 1.0f);
        data.force_lod_level      = options.force_lod_level;
    }

    // 将上一帧Hi-Z资源写入 push constants，并把上一帧view-proj写入 cbuffer
    static void FillHiZOcclusionParams(
        RasterContext&        context,
        const CullingOptions& options,
        CullParams&           params,
        CullData&             data
    ) {
        if (!CanUseHiZOcclusion(context, options)) {
            return;
        }

        const uint  hiz_mip_count = std::min(context.hiz_data.mip_count, CULL_MAX_HIZ_MIPS);
        const uint2 hiz_size      = context.textures.hiz_previous.GetSize(0);

        params.flags |= CULL_FLAG_ENABLE_HIZ_OCCLUSION;
        params.hiz_mip_count = hiz_mip_count;

        for (uint mip = 0; mip < hiz_mip_count; ++mip) {
            params.hiz_mip_handles[mip] = context.textures.hiz_previous.GetMipHandle(mip);
        }

        data.previous_view_proj = Transpose(context.hiz_data.previous_view_proj);
        data.hiz_info           = uint4(hiz_size.x, hiz_size.y, hiz_mip_count, 0u);
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

    void ConsumeCounterReadback() {
        if (m_counter_readback.Valid()) {
            const std::optional<ReadbackResult> result =
                m_counter_readback.TryGet();
            if (!result.has_value()) {
                return;
            }
            if (result->status == ReadbackStatus::Ready) {
                const std::optional<GpuCullingCounterData> counters =
                    result->ReadValue<GpuCullingCounterData>();
                if (counters.has_value()) {
                    m_readback_counters = *counters;
                }
            }
            m_counter_readback = {};
        }

        if (!m_legacy_counter_readback) {
            return;
        }
        const Detail::LegacyCounterReadbackStatus legacy_status =
            m_legacy_counter_readback->GetStatus();
        if (legacy_status ==
            Detail::LegacyCounterReadbackStatus::Pending) {
            return;
        }
        if (legacy_status ==
            Detail::LegacyCounterReadbackStatus::Ready) {
            m_readback_counters =
                m_legacy_counter_readback->counters;
        }
        m_legacy_counter_readback.reset();
    }

    void QueueCounterReadback(
        CommandList& _command_list,
        BufferView   _counter_buffer
    ) {
        if (m_counter_readback.Valid() ||
            m_legacy_counter_readback) {
            return;
        }

        Buffer* source = _counter_buffer.GetBuffer();
        if (source == nullptr) {
            return;
        }
        if (source->SupportsOwningReadback()) {
            m_counter_readback = _command_list.Readback(
                _counter_buffer, "RasterGpuCullingCounters"
            );
            return;
        }

        m_legacy_counter_readback =
            Detail::QueueLegacyCounterReadback(
                _command_list,
                _counter_buffer,
                "RasterGpuCullingCountersLegacy"
            );
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

        ConsumeCounterReadback();
        if (out_stats) {
            *out_stats = ToStatistics(m_readback_counters);
        }

        context.cmd_list.ClearResource(visibility_set.counter_buf->GetView(), 0u);

        if (draw_count == 0) {
            QueueCounterReadback(
                context.cmd_list,
                visibility_set.counter_buf->GetView()
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

        const auto cluster_group_view =
            gpu_scene_res.cluster_group_buf.buf != nullptr
                ? gpu_scene_res.cluster_group_buf.buf->GetView()
                : m_cluster_group_dummy_buf->GetView();

        if ((params.flags & CULL_FLAG_ENABLE_HIZ_OCCLUSION) != 0u) {
            context.cmd_list
                .Compute(
                    m_hiz_occlusion_pso,
                    gpu_scene_res.draw_cmd_buf.buf->GetView(),             // SRV: source_draw_commands
                    gpu_scene_res.primitive_buf.buf->GetView(),            // SRV: primitives
                    gpu_scene_res.instance_buf.buf->GetView(),             // SRV: instances
                    visibility_set.visible_instance_id_buf.buf->GetView(), // UAV: visible_instance_ids
                    visibility_set.draw_cmd_buf->GetView(),                // UAV: draw_commands
                    visibility_set.counter_buf->GetView(),                 // UAV: counters
                    m_cull_data_buffer->GetView(),                         // CBV: cull_data
                    cluster_group_view,                                    // SRV: cluster_groups
                    context.bdls,                                          // Bindless heap for Hi-Z mip views
                    params                                                 // Push constant: cull_params
                )
                .Dispatch(uint3(dispatch_count, 1, 1), "Hi-Z Occlusion Culling");
        } else {
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
                    cluster_group_view,                                    // SRV: cluster_groups
                    params                                                 // Push constant: cull_params
                )
                .Dispatch(uint3(dispatch_count, 1, 1), "Culling");
        }

        if (!profile_scope_name.empty()) {
            context.cmd_list.PopScopeWithTimeScope();
        }

        QueueCounterReadback(
            context.cmd_list,
            visibility_set.counter_buf->GetView()
        );
    }

private:
    CullPipeline             m_pso;
    HiZOcclusionCullPipeline m_hiz_occlusion_pso;
    BufferRef                m_cull_data_buffer;
    BufferRef                m_cluster_group_dummy_buf;
    GpuCullingCounterData    m_readback_counters{};
    ReadbackFuture           m_counter_readback{};
    std::shared_ptr<Detail::LegacyCounterReadbackState>
        m_legacy_counter_readback{};
};

} // namespace Moer::Render::Raster
