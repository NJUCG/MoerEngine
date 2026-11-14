#include "GaussianShadowPass.h"
#include "RasterConfig.h"
#include "RasterGraphResourceAccess.h"
#include "math/Function.h"
#include "rhi/RHI.h"

#include <format>

namespace Moer::Render::Raster {
namespace {
// CPU frame parameters retain mathematical matrices; transpose only for GPU upload.
GaussianShadowData PrepareGpuShadowData(GaussianShadowData data) {
    for (auto& matrix : data.world_to_clip) {
        matrix = Transpose(matrix);
    }
    data.world_to_view = Transpose(data.world_to_view);
    return data;
}
} // namespace

GaussianShadowPass::GaussianShadowPass(RasterContext& context) :
    GaussianShadowPass(
        context.manager,
        context.bdls,
        context.textures.depth_linear_sampler.tex->GetFormat()
    ) {}
GaussianShadowPass::GaussianShadowPass(
    ShaderManager&   manager,
    BindlessArrayRef bindless,
    EPixelFormat     depth_format
) :
    m_bindless(std::move(bindless)) {
    GfxPsoCreateInfo info(
        RHIRasterizeInfo::Preset(),
        {},
        {},
        RHIDepthStencilStateInfo::Preset<DepthStencil::DEPTH_WRITE_GREATER>(),
        depth_format
    );
    m_pipeline = manager.Raster()
                     .Vertex("features/gaussian/Shadow.vert.hlsl")
                     .Pixel("features/gaussian/Shadow.frag.hlsl")
                     .Build<GaussianShadowPipeline>(std::move(info));
    m_inspect  = manager.Compute<GaussianShadowInspectPipeline>("features/gaussian/ShadowInspect.comp.hlsl");
    m_data     = RenderDevice::Get().CreateBuffer<byte>(
        "Gaussian shadow views",
        sizeof(GaussianShadowData),
        EBufferUsageFlags::UNORDERED_ACCESS | EBufferUsageFlags::TRANSFER_DST
    );
    m_data_handle = m_bindless->AllocateBuffer(m_data->GetView());
}
GaussianShadowPass::~GaussianShadowPass() {
    m_bindless->UnbindBuffer(m_data_handle);
    for (uint32 i = 0; i < 4; ++i)
        if (m_maps[i])
            m_bindless->UnbindTexture(m_handles[i]);
    for (uint32 i = 0; i < 4; ++i)
        if (m_mesh_maps[i])
            m_bindless->UnbindTexture(m_mesh_handles[i]);
}
GaussianShadowPass::RecordParameters GaussianShadowPass::Prepare(
    RasterContext&          context,
    const SceneUpdateBatch& updates,
    const RasterConfig&     config,
    const Camera&           camera,
    const GaussianGpuScene& scene,
    GaussianViewSettings    settings,
    bool                    editor_view
) {
    RecordParameters result{};
    result.data_buffer = m_data;
    result.bindless    = m_bindless;
    result.gaussians   = scene.GetGaussianBuffer();
    result.instances   = scene.GetInstanceBuffer();
    auto&      data    = result.data;
    const uint32 mask  = editor_view ? 2u : 1u;
    bool       has_receivers = false;
    for (uint32 i = 0; i < scene.GetInstances().size(); ++i) {
        const auto& instance = scene.GetInstances()[i];
        has_receivers |= (instance.range.w & mask) && (instance.range.w & 8u) && instance.range.y;
        if ((instance.range.w & mask) && (instance.range.w & 4u) && instance.range.y) {
            result.draws.push_back(instance);
            result.instance_indices.push_back(i);
        }
    }
    const bool cube = config.shadow_map_mode == EShadowMapMode::POINT_CUBE;
    const bool directional =
        config.shadow_map_mode == EShadowMapMode::CSM || config.shadow_map_mode == EShadowMapMode::CSM_AUTO;
    data.light_indices  = uint4(
        updates.main_directional_light_index,
        updates.main_point_light_index,
        config.shadow_map_mode == EShadowMapMode::POINT_CUBE ? 1u : 0u,
        0
    );
    bool valid          = (cube && updates.main_point_light.has_value()) ||
                          (directional && updates.main_directional_light.has_value());
    data.config         = uint4(
        valid ? (cube ? 2u : 1u) : 0u,
        cube ? 1u : uint32(config.shadow_csm_num_of_cascades),
        config.shadow_csm_sm_size,
        valid && !result.draws.empty() ? 1u : 0u
    );
    data.options = float4(
        settings.shadow_alpha_cutoff,
        settings.shadow_receive_strength,
        settings.shadow_world_bias,
        config.shadow_csm_blend_option ? 1.f : 0.f
    );
    data.world_to_view = camera.GetViewMatrix();
    data.splits        = context.lighting_data.cascade_split_ratios;
    data.blends        = context.lighting_data.cascade_blend_start_ratios;
    data.clips = float4(camera.GetNearClip(), camera.GetFarClip(), camera.GetNearClip(), camera.GetFarClip());
    if (valid && cube) {
        auto& source               = context.point_shadow_data.shadow_cubes[0];
        data.light                 = float4(source.light_pos, 1);
        data.mesh_maps.x           = source.handle;
        data.clips.x               = source.near_plane;
        data.clips.y               = source.far_plane;
        const float3 directions[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        const float3 ups[6]        = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        auto projection = MakePerspectiveMatrixRH(1.57079632679f, 1.f, source.far_plane, source.near_plane);
        // Negative-height RHI viewports require this flip for standard cube-map addressing.
        projection[1][1] *= -1.f;
        for (uint32 face = 0; face < 6; ++face)
            data.world_to_clip[face] =
                projection *
                MakeLookatViewMatrixRH(source.light_pos, source.light_pos + directions[face], ups[face]);
    } else if (valid) {
        data.light = float4(-context.lighting_data.main_light_direction, 0);
        for (uint32 i = 0; i < data.config.y; ++i) {
            data.world_to_clip[i] = context.lighting_data.world2shadow_clip[i];
            data.mesh_maps[i]     = context.csm_data.shadow_map_textures[i].hdl;
        }
    }
    if (valid && has_receivers)
        for (uint32 i = 0; i < data.config.y; ++i) {
            TextureRef source =
                cube ? TextureRef(context.point_shadow_data.shadow_cubes[0].tex) :
                       TextureRef(context.csm_data.shadow_map_textures[i].tex->GetView().GetTexture());
            if (m_mesh_maps[i].Get() != source.Get()) {
                if (m_mesh_maps[i])
                    m_bindless->UnbindTexture(m_mesh_handles[i]);
                m_mesh_maps[i] = source;
                m_mesh_handles[i] =
                    m_bindless->AllocateTexture(source->GetView(), Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
            }
            data.mesh_maps[i] = m_mesh_handles[i];
        }
    EnsureResources(context, config, data.config.w != 0);
    if (data.config.w)
        for (uint32 i = 0; i < data.config.y; ++i) {
            data.gaussian_maps[i] = m_handles[i];
            result.maps[i]        = m_maps[i];
        }
    context.lighting_data.gaussian_shadow = uint4(m_data_handle, data.config.w, 0, 0);
    return result;
}
void GaussianShadowPass::EnsureResources(RasterContext& context, const RasterConfig& config, bool cast) {
    if (!cast)
        return;
    const bool   cube  = config.shadow_map_mode == EShadowMapMode::POINT_CUBE;
    const uint32 count = cube ? 1u : uint32(config.shadow_csm_num_of_cascades);
    for (uint32 i = 0; i < count; ++i) {
        if (!m_maps[i] || m_maps[i]->GetWidth() != uint32(config.shadow_csm_sm_size) || m_cube != cube) {
            if (m_maps[i])
                m_bindless->UnbindTexture(m_handles[i]);
            auto usage = ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT |
                         ETextureUsageFlags::TRANSFER_SRC;
            m_maps[i] = cube ?
                            TextureRef(context.device.CreateCubeMap(
                                "Gaussian point shadow",
                                Extent2D(uint32(config.shadow_csm_sm_size), uint32(config.shadow_csm_sm_size)),
                                context.textures.depth_linear_sampler.tex->GetFormat(),
                                usage
                            )) :
                            context.device.CreateTexture(
                                "Gaussian cascade shadow",
                                Extent3D(uint32(config.shadow_csm_sm_size), uint32(config.shadow_csm_sm_size), 1),
                                context.textures.depth_linear_sampler.tex->GetFormat(),
                                usage
                            );
            InitializeGraphicsTexture(m_maps[i]);
            m_handles[i] =
                m_bindless->AllocateTexture(m_maps[i]->GetView(), Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
        }
    }
    m_cube = cube;
}
double GaussianShadowPass::GetGpuTime() {
    if (m_timings.empty()) {
        return m_gpu_ms;
    }
    for (const auto& query : m_timings) {
        if (query.Valid() && query.Status() == QueryStatus::Pending) {
            return m_gpu_ms;
        }
    }
    double duration_ns = 0.;
    for (const auto& query : m_timings) {
        if (query.Valid()) {
            auto result = query.Get();
            if (const auto* timing = std::get_if<TimestampQueryResult>(&result.payload)) {
                duration_ns += timing->duration_ns;
            }
        }
    }
    m_gpu_ms = duration_ns / 1000000.;
    m_timings.clear();
    return m_gpu_ms;
}
void GaussianShadowPass::RecordUpload(CommandList& commands, const RecordParameters& parameters) {
    const auto gpu_data = PrepareGpuShadowData(parameters.data);
    commands.CopyFrom(
        std::span<const byte>(reinterpret_cast<const byte*>(&gpu_data), sizeof(gpu_data)),
        parameters.data_buffer->GetView(),
        "Gaussian shadow views"
    );
    commands.UpdateBindlessArray(parameters.bindless);
    commands.AddCallback([parameters]() {});
}

QueryFuture GaussianShadowPass::RecordDraw(
    CommandList& commands, const RecordParameters& parameters,
    uint32 view, uint32 draw, bool collect_timing
) {
    QueryToken timing;
    if (collect_timing) {
        timing = commands.BeginTimestampQuery("Gaussian shadow casting");
    }
    const auto&        instance = parameters.draws[draw];
    GaussianShadowDraw param{};
    param.world_to_clip = Transpose(parameters.data.world_to_clip[view]);
    param.info = uint4(instance.range.x, parameters.instance_indices[draw], parameters.data.config.z, 0);
    param.options = parameters.data.options;
    const bool cube = parameters.data.config.x == 2;
    auto texture = parameters.maps[cube ? 0 : view];
    auto depth   = DepthAttachment(cube ? texture->GetView().Slice(view, 1) : texture->GetView());
    depth.action = draw == 0 ? EAttachmentAction::AC_DS_CLEAR_STORE : EAttachmentAction::AC_DS_LOAD_STORE;
    Array<SingleDrawParam> draws;
    draws.emplace_back(SingleDrawParam{6, instance.range.y, 0, 0, 0});
    commands.Gfx(m_pipeline, param, parameters.gaussians->GetView(), parameters.instances->GetView())
        .Draw(
            "Gaussian shadow",
            Rect2D(0, 0, parameters.data.config.z, parameters.data.config.z),
            std::move(draws),
            depth
        );
    if (timing.Valid()) {
        commands.EndTimestampQuery(timing);
        return timing.GetFuture();
    }
    return {};
}

RenderGraph::PassHandle GaussianShadowPass::AddToGraph(
    RenderGraph& graph, std::shared_ptr<const RecordParameters> parameters
) {
    const auto data = ImportRasterBuffer(graph, "GaussianShadow.views", parameters->data_buffer.Get());
    const auto table = graph.ImportToken("Raster.bindless", parameters->bindless.Get());
    const auto upload = graph.AddRecordPass(
        "GaussianShadow.Upload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Transfer)
                .Write(data, RenderGraph::BufferAccessType::TransferDestination)
                .Write(table);
        },
        [this, parameters](CommandList& commands) {
            RecordUpload(commands, *parameters);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    if (!parameters->data.config.w) {
        return upload;
    }
    const auto gaussians = ImportRasterBuffer(graph, "GaussianShadow.gaussians", parameters->gaussians.Get());
    const auto instances = ImportRasterBuffer(graph, "GaussianShadow.instances", parameters->instances.Get());
    const uint32 count = parameters->data.config.x == 2 ? 6u : parameters->data.config.y;
    const bool collect_timing = m_timings.empty();
    if (collect_timing) {
        // Slots are fixed before parallel recording; each callback writes only its own future.
        m_timings.resize(count * parameters->draws.size());
    }
    std::vector<RenderGraph::PassHandle> draws;
    for (uint32 view = 0; view < count; ++view) {
        const bool cube = parameters->data.config.x == 2;
        const auto texture = ImportRasterTexture(
            graph, "GaussianShadow.depth", parameters->maps[cube ? 0 : view].Get()
        );
        RenderGraph::TextureRange range{};
        range.layer_first = cube ? view : 0;
        range.layer_count = 1;
        for (uint32 draw = 0; draw < parameters->draws.size(); ++draw) {
            const size_t query_index = view * parameters->draws.size() + draw;
            draws.push_back(graph.AddRecordPass(
                std::format("GaussianShadow.View{}.Draw{}", view, draw),
                [=](RenderGraph::PassBuilder& builder) {
                    builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Graphics)
                        .Read(table)
                        .Read(gaussians, RenderGraph::BufferAccessType::ShaderRead)
                        .Read(instances, RenderGraph::BufferAccessType::ShaderRead);
                    if (draw == 0) {
                        builder.Write(texture, RenderGraph::TextureAccessType::DepthStencilAttachment, range);
                    } else {
                        builder.ReadWrite(texture, RenderGraph::TextureAccessType::DepthStencilAttachment, range);
                    }
                },
                [this, parameters, view, draw, collect_timing, query_index](CommandList& commands) {
                    auto timing = RecordDraw(commands, *parameters, view, draw, collect_timing);
                    if (collect_timing) {
                        m_timings[query_index] = std::move(timing);
                    }
                },
                RenderGraph::PassRecordPolicy::ParallelAllowed
            ));
        }
    }
    return graph.AddRecordPass(
        "GaussianShadow.Ready",
        [=](RenderGraph::PassBuilder& builder) {
            builder.DependsOn(upload).SideEffect();
            for (auto draw : draws) {
                builder.DependsOn(draw);
            }
        },
        [](CommandList&) {},
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
}

TextureRef GaussianShadowPass::AddDepthDebugToGraph(
    RenderGraph& graph, const RecordParameters& parameters
) {
    const uint32 size  = std::min(parameters.data.config.z, 512u);
    const uint32 width = size * (parameters.data.config.x == 2 ? 3 : 2), height = size * 2;
    if (!m_debug_depth || m_debug_depth->GetWidth() != width || m_debug_depth->GetHeight() != height) {
        m_debug_depth = RenderDevice::Get().CreateTexture(
            "Gaussian shadow depth atlas",
            Extent3D(width, height, 1),
            PF_R32G32B32A32_SFLOAT,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS |
                ETextureUsageFlags::TRANSFER_SRC
        );
        InitializeGraphicsTexture(m_debug_depth);
    }
    auto uniform = RenderDevice::Get().CreateBuffer<byte>(
        "Gaussian shadow debug views",
        sizeof(GaussianShadowData),
        EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::TRANSFER_DST
    );
    const auto data = ImportRasterBuffer(graph, "GaussianShadow.debug_views", uniform.Get());
    const auto output = m_debug_depth;
    const auto image = ImportRasterTexture(graph, "GaussianShadow.debug_atlas", output.Get());
    const auto table = graph.ImportToken("Raster.bindless", parameters.bindless.Get());
    std::vector<RenderGraph::TextureHandle> maps;
    for (const auto& texture : parameters.maps) {
        if (texture) {
            maps.push_back(ImportRasterTexture(graph, "GaussianShadow.debug_map", texture.Get()));
        }
    }
    graph.AddRecordPass(
        "GaussianShadow.DebugUpload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Transfer)
                .Write(data, RenderGraph::BufferAccessType::TransferDestination)
                .Write(table);
        },
        [parameters, uniform](CommandList& commands) {
            const auto gpu_data = PrepareGpuShadowData(parameters.data);
            commands.CopyFrom(
                std::span<const byte>(reinterpret_cast<const byte*>(&gpu_data), sizeof(gpu_data)),
                uniform->GetView()
            );
            commands.UpdateBindlessArray(parameters.bindless);
        },
        RenderGraph::PassRecordPolicy::Serial
    );
    graph.AddRecordPass(
        "GaussianShadow.DebugInspect",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::GpuWorkType::Compute)
                .Read(data, RenderGraph::BufferAccessType::ShaderRead)
                .Read(table)
                .Write(image, RenderGraph::TextureAccessType::Storage);
            for (auto map : maps) {
                builder.Read(map, RenderGraph::TextureAccessType::Sampled);
            }
        },
        [this, parameters, uniform, output, width, height](CommandList& commands) {
            commands.Compute(m_inspect, uniform->GetView(), output->GetView(), parameters.bindless)
                .Dispatch(uint3((width + 7) / 8, (height + 7) / 8, 1), "Gaussian shadow depth atlas");
            commands.AddCallback([parameters, uniform, output]() {});
        },
        RenderGraph::PassRecordPolicy::ParallelAllowed
    );
    return output;
}
} // namespace Moer::Render::Raster
