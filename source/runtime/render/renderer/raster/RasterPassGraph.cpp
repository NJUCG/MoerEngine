// Explicit RDG registration owned by each raster pass.

#include "AaPass.h"
#include "AoPass.h"
#include "BilateralFilterDenoiserPass.h"
#include "BloomPass.h"
#include "CameraGizmoPass.h"
#include "CsmGizmoPass.h"
#include "DirectionalShadowMaskPass.h"
#include "GeometryPass.h"
#include "HiZBuildPass.h"
#include "LightingPass.h"
#include "ProbeGizmoPass.h"
#include "ProbeUpdatePass.h"
#include "RtaoDenoiserPass.h"
#include "ShadowDepthPass.h"
#include "SkyboxPass.h"
#include "SsrPass.h"
#include "TessellatedSurfacePass.h"
#include "TonemappingPass.h"

#include "profile/ProfileScope.h"
#include "renderer/common/UiCombinePass.h"

#include <format>
#include <functional>
#include <utility>

namespace Moer::Render::Raster {

namespace {

template<typename Record>
void RecordWithPassMarker(CommandList& cmd_list, std::string_view name, Record&& record) {
    const std::string marker_name = std::format("Pass: {}", name);
    ScopedGpuMarker marker(cmd_list, marker_name, GpuMarkerPalette::Pass());
    std::invoke(std::forward<Record>(record));
}

std::vector<RenderGraph::TextureHandle> ImportShadowMaps(RenderGraph& graph, const RasterContext& context) {
    std::vector<RenderGraph::TextureHandle> result;
    for (const auto& map : context.csm_data.shadow_map_textures) {
        if (map.tex) {
            result.push_back(ImportRasterTexture(graph, "Shadow.cascade", map.tex->GetView().GetTexture()));
        }
    }
    for (const auto& map : context.point_shadow_data.shadow_cubes) {
        if (map.tex) {
            result.push_back(ImportRasterTexture(graph, "Shadow.point", map.tex.Get()));
        }
    }
    return result;
}

} // namespace

RenderGraph::PassHandle ShadowDepthPass::AddToGraph(
    RenderGraph&        graph,
    RasterContext&      context,
    const RasterConfig& ui_config,
    const Camera&       camera,
    GraphResources      resources
) {
    // Allocation and cache decisions determine the graph's physical resources
    // and node count, so they must finish before Compile. GPU recording remains parallel.
    auto       parameters = std::make_shared<const RecordParameters>(Prepare(context, ui_config, camera));
    const auto table      = graph.ImportToken("Raster.bindless", parameters->bindless.Get());
    RenderGraph::PassHandle publication{};
    if (parameters->update_bindless_array) {
        publication = graph.AddRecordPass(
            "ShadowDepth.PublishBindings",
            [=](RenderGraph::PassBuilder& builder) {
                builder.Write(table).SideEffect();
            },
            [parameters](CommandList& commands) {
                commands.UpdateBindlessArray(parameters->bindless);
            }
        );
    }
    const RasterSceneGraphReads scene_reads(graph, context.GetGpuSceneRes());
    const auto indices = ImportRasterBuffer(graph, "Scene.indices", parameters->index_buffer.GetBuffer());
    std::vector<RenderGraph::PassHandle> draws;
    for (size_t index = 0; index < parameters->draws.size(); ++index) {
        const auto& batch = parameters->draws[index];
        m_culling_pass.AddToGraph(graph, batch.pass_name, batch.culling);
        RenderGraph::BufferHandle matrices{};
        if (batch.multiview) {
            matrices = ImportRasterBuffer(
                graph, "ShadowDepth.matrices", parameters->point_shadow_view_matrices.Get()
            );
            graph.AddRecordPass(
                batch.pass_name + ".UploadMatrices",
                [=](RenderGraph::PassBuilder& builder) {
                    builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                        .Write(matrices, RenderGraph::BufferState::TransferDestination);
                },
                [parameters, index](CommandList& commands) {
                    const auto& data = parameters->draws[index].multiview_matrices;
                    commands.CopyFrom(
                        std::span<const byte>(reinterpret_cast<const byte*>(&data), sizeof(data)),
                        parameters->point_shadow_view_matrices->GetView()
                    );
                }
            );
        }
        const auto depth = ImportRasterTexture(graph, batch.pass_name, batch.depth_view.GetTexture());
        const auto visible =
            ImportRasterBuffer(graph, "ShadowDepth.visible", batch.culling.visible_instance_ids.GetBuffer());
        const auto indirect =
            ImportRasterBuffer(graph, "ShadowDepth.indirect", batch.culling.draw_commands.GetBuffer());
        const auto count =
            ImportRasterBuffer(graph, "ShadowDepth.count", batch.culling.draw_count.GetBuffer());
        RenderGraph::TextureRange range{};
        range.mip_first   = batch.depth_view.mip_level;
        range.mip_count   = batch.depth_view.num_mips;
        range.layer_first = batch.depth_view.array_layer;
        range.layer_count = batch.depth_view.num_array;
        draws.push_back(graph.AddRecordPass(
            batch.pass_name,
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                    .Read(table)
                    .Read(indices, RenderGraph::BufferState::IndexBuffer)
                    .Read(visible, RenderGraph::BufferState::ShaderResource)
                    .Read(indirect, RenderGraph::BufferState::IndirectArgument)
                    .Read(count, RenderGraph::BufferState::IndirectArgument)
                    .Write(depth, RenderGraph::TextureState::DepthStencilWrite, range);
                scene_reads.Declare(builder);
                if (matrices.IsValid()) {
                    builder.Read(matrices, RenderGraph::BufferState::ShaderResource);
                }
            },
            [this, parameters, index](CommandList& commands) {
                const auto& draw = parameters->draws[index];
                if (draw.multiview) {
                    RecordPointShadowMultiviewDraw(commands, *parameters, draw);
                } else {
                    RecordShadowDraw(commands, *parameters, draw);
                }
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        ));
    }
    return graph.AddRecordPass(
        "ShadowDepth.Ready",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Write(resources.shadow_maps).SideEffect();
            for (auto draw : draws) {
                builder.DependsOn(draw);
            }
            if (publication.IsValid()) {
                builder.DependsOn(publication);
            }
        },
        [](CommandList&) {}
    );
}

RenderGraph::PassHandle ProbeUpdatePass::AddToGraph(
    RenderGraph&        graph,
    RasterContext&      context,
    const RasterConfig& config,
    const Camera&       camera,
    uint64              frame_index,
    GraphResources      resources
) {
    auto parameters = std::make_shared<const RecordParameters>(Prepare(context, config, camera, frame_index));
    if (!parameters->enabled) {
        return {};
    }
    const auto import = [&](BufferView view) {
        return ImportRasterBuffer(graph, "ProbeUpdate.data", view.GetBuffer());
    };
    std::vector<RenderGraph::BufferHandle> uploads;
    const auto&                            upload = parameters->scene_upload;
    const auto add_upload                         = [&](const BufferRef& buffer, const Array<byte>& data) {
        if (buffer && !data.empty()) {
            uploads.push_back(ImportRasterBuffer(graph, "ProbeUpdate.upload", buffer.Get()));
        }
    };
    add_upload(upload.cell_buffer, upload.cell_data);
    add_upload(upload.volume_buffer, upload.volume_data);
    add_upload(upload.brick_buffer, upload.brick_data);
    add_upload(upload.page_table_buffer, upload.page_table_data);
    add_upload(upload.scene_data_buffer, upload.scene_data);
    auto last = graph.AddRecordPass(
        "ProbeUpdate.Upload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(resources.probe_volume)
                .SideEffect();
            for (auto buffer : uploads) {
                builder.Write(buffer, RenderGraph::BufferState::TransferDestination);
            }
        },
        [parameters](CommandList& commands) {
            ProbeVolumeResource::RecordSceneDataUpload(commands, parameters->scene_upload);
        }
    );
    const auto probes           = import(parameters->probe_data);
    const auto visibility       = import(parameters->visibility_atlas);
    const auto irradiance       = import(parameters->irradiance_atlas);
    const auto scene            = import(parameters->scene_data);
    const auto volumes          = import(parameters->volume_data);
    const auto bricks           = import(parameters->brick_data);
    const auto visibility_image = ImportRasterTexture(
        graph, "ProbeUpdate.visibility", parameters->visibility_atlas_texture.GetTexture()
    );
    const auto irradiance_image = ImportRasterTexture(
        graph, "ProbeUpdate.irradiance", parameters->irradiance_atlas_texture.GetTexture()
    );
    const auto environment =
        ImportRasterTexture(graph, "ProbeUpdate.environment", context.textures.cubemap_tex.tex.Get());
    const RasterSceneGraphReads            scene_reads(graph, context.GetGpuSceneRes());
    std::vector<RenderGraph::BufferHandle> ray_data;
    const auto&                            scene_resources = context.GetGpuSceneRes();
    for (const auto* resource :
         {&scene_resources.rt_instance_buf,
          &scene_resources.rt_primitive_table_buf,
          &scene_resources.index_buf,
          &scene_resources.light_buf}) {
        if (resource->buf) {
            ray_data.push_back(ImportRasterBuffer(graph, "ProbeUpdate.ray_data", resource->buf.Get()));
        }
    }
    for (uint index = 0; index < parameters->update.job_count && parameters->update.scheduled_probe_count;
         ++index) {
        if (!parameters->update.jobs[index].probe_count) {
            continue;
        }
        last = graph.AddRecordPass(
            std::format("ProbeUpdate.Job{}", index),
            [=](RenderGraph::PassBuilder& builder) {
                using BS = RenderGraph::BufferState;
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                    .ReadWrite(resources.probe_volume)
                    .ReadWrite(probes, BS::UnorderedAccess)
                    .ReadWrite(visibility, BS::UnorderedAccess)
                    .ReadWrite(irradiance, BS::UnorderedAccess)
                    .Read(scene, BS::ShaderResource)
                    .Read(volumes, BS::ShaderResource)
                    .Read(bricks, BS::ShaderResource)
                    .Write(visibility_image, RenderGraph::TextureState::UnorderedAccess)
                    .Write(irradiance_image, RenderGraph::TextureState::UnorderedAccess)
                    .Read(environment, RenderGraph::TextureState::Sampled)
                    .SideEffect();
                scene_reads.Declare(builder);
                for (auto buffer : ray_data) {
                    builder.Read(buffer, BS::ShaderResource);
                }
            },
            [this, parameters, index](CommandList& commands) {
                RecordJob(commands, *parameters, index);
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    }
    return last;
}

RenderGraph::PassHandle GeometryPass::AddToGraph(
    RenderGraph&   graph,
    RasterContext& context,
    RasterConfig&  raster_config,
    const Camera&  camera,
    GraphResources resources
) {
    auto parameters = std::make_shared<const RecordParameters>(Prepare(context, raster_config, camera));
    culling_pass.AddToGraph(graph, "Geometry", parameters->culling, resources.hiz_previous);
    const RasterSceneGraphReads scene_reads(graph, context.GetGpuSceneRes());
    const auto indices = ImportRasterBuffer(graph, "Scene.indices", parameters->index_buffer.GetBuffer());
    const auto visible =
        ImportRasterBuffer(graph, "Geometry.visible", parameters->culling.visible_instance_ids.GetBuffer());
    const auto indirect =
        ImportRasterBuffer(graph, "Geometry.indirect", parameters->draw_commands.GetBuffer());
    const auto count = ImportRasterBuffer(graph, "Geometry.count", parameters->draw_count.GetBuffer());
    const auto table = graph.ImportToken("Raster.bindless", parameters->bindless.Get());
    return graph.AddRecordPass(
        "Geometry.Draw",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .Read(table)
                .Read(indices, RenderGraph::BufferState::IndexBuffer)
                .Read(visible, RenderGraph::BufferState::ShaderResource)
                .Read(indirect, RenderGraph::BufferState::IndirectArgument)
                .Read(count, RenderGraph::BufferState::IndirectArgument)
                .Write(resources.base_color, RenderGraph::TextureState::RenderTarget)
                .Write(resources.normal, RenderGraph::TextureState::RenderTarget)
                .Write(resources.metal_rough_ao, RenderGraph::TextureState::RenderTarget)
                .Write(resources.depth, RenderGraph::TextureState::DepthStencilWrite);
            scene_reads.Declare(builder);
        },
        [this, parameters](CommandList& commands) {
            RecordDraw(commands, *parameters);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
}

RenderGraph::PreparedPassHandle TessellatedSurfacePass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& config,
    const Camera& camera,
    GraphResources resources
) {
    if (!supported || !config.tessellated_surface_enabled) {
        return {};
    }
    auto prepared = graph.AddSetupPass(
        "TessellatedSurface.Prepare",
        uint8_t{0},
        [this, &context, &config, &camera](const uint8_t&) {
            return Prepare(context, config, camera);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    const auto data = ImportRasterBuffer(graph, "Tessellation.data", surface_data_buffer.Get());
    graph.AddRecordPass(
        "Tessellation.Upload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(data, RenderGraph::BufferState::TransferDestination);
        },
        [this, prepared](CommandList& commands) {
            RecordUpload(commands, prepared.Get());
        }
    );
    auto pass = graph.AddRecordPass(
        "TessellatedSurface",
        [resources, data](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferState::ShaderResource);
            builder.ReadWrite(resources.base_color, RenderGraph::TextureState::RenderTarget)
                .ReadWrite(resources.normal, RenderGraph::TextureState::RenderTarget)
                .ReadWrite(resources.metal_rough_ao, RenderGraph::TextureState::RenderTarget)
                .ReadWrite(resources.depth, RenderGraph::TextureState::DepthStencilWrite);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "TessellatedSurface", [&] {
                RecordDraw(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle HiZBuildPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "HiZBuild.Prepare",
        uint8_t{0},
        [this, &context](const uint8_t&) { return Prepare(context); }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    RenderGraph::PassHandle pass{};
    for (uint32 mip = 0; mip < context.textures.hiz_current.tex->GetNumMips(); ++mip) {
        pass = graph.AddRecordPass(
            std::format("HiZBuild.Mip{}", mip),
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                    .Read(mip == 0 ? resources.depth : resources.hiz_current,
                          RenderGraph::TextureState::Sampled,
                          RenderGraph::TextureRange::Mips(mip == 0 ? 0 : mip - 1, 1))
                    .Write(resources.hiz_current, RenderGraph::TextureState::UnorderedAccess,
                           RenderGraph::TextureRange::Mips(mip, 1));
            },
            [this, prepared, mip](CommandList& cmd_list) { RecordMip(cmd_list, prepared.Get(), mip); },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    }
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle DirectionalShadowMaskPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    GraphResources resources,
    std::span<const RenderGraph::SetupPassHandle> setup_dependencies
) {
    auto prepared = graph.AddSetupPass(
        "DirectionalShadowMask.Prepare",
        uint8_t{0},
        [this, &context](const uint8_t&) { return Prepare(context); },
        setup_dependencies
    );
    if (!prepared.IsValid()) {
        return {};
    }

    const auto shadows = ImportShadowMaps(graph, context);
    auto       pass    = graph.AddRecordPass(
        "DirectionalShadowMask",
        [resources, shadows](RenderGraph::PassBuilder& builder) {
            for (auto texture : shadows) {
                builder.Read(texture, RenderGraph::TextureState::Sampled);
            }

            builder.Read(resources.normal, RenderGraph::TextureState::Sampled)
                .Read(resources.depth, RenderGraph::TextureState::Sampled)
                .Read(resources.lighting_data, RenderGraph::BufferState::ShaderResource)
                .Read(resources.shadow_maps)
                .Write(resources.shadow_mask, RenderGraph::TextureState::RenderTarget);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "DirectionalShadowMask", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle LightingPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& ui_config,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "Lighting.Prepare",
        uint8_t{0},
        [this, &context, &ui_config](const uint8_t&) {
            return Prepare(context, ui_config);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    const auto                  shadows = ImportShadowMaps(graph, context);
    const RasterProbeGraphReads probes(graph, context.probe_volume);
    const auto emu  = ImportRasterTexture(graph, "Lighting.emu", context.textures.lut_ggx_emu.tex.Get());
    const auto eavg = ImportRasterTexture(graph, "Lighting.eavg", context.textures.lut_ggx_eavg.tex.Get());
    auto       pass = graph.AddRecordPass(
        "Lighting",
        [resources, shadows, probes, emu, eavg](RenderGraph::PassBuilder& builder) {
            for (auto texture : shadows) {
                builder.Read(texture, RenderGraph::TextureState::Sampled);
            }
            probes.Declare(builder);
            builder.Read(emu, RenderGraph::TextureState::Sampled)
                .Read(eavg, RenderGraph::TextureState::Sampled);

            builder.Read(resources.base_color, RenderGraph::TextureState::Sampled)
                .Read(resources.normal, RenderGraph::TextureState::Sampled)
                .Read(resources.metal_rough_ao, RenderGraph::TextureState::Sampled)
                .Read(resources.depth, RenderGraph::TextureState::Sampled)
                .Read(resources.shadow_mask, RenderGraph::TextureState::Sampled)
                .Read(resources.lighting_data, RenderGraph::BufferState::ShaderResource)
                .Read(resources.cubemap, RenderGraph::TextureState::Sampled)
                .Read(resources.probe_volume)
                .Write(resources.lighting_output, RenderGraph::TextureState::RenderTarget);
            if (resources.scene_lights.IsValid()) {
                builder.Read(resources.scene_lights, RenderGraph::BufferState::ShaderResource);
            }
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Lighting", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle SkyboxPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& config,
    const Camera& camera,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "Skybox.Prepare",
        uint8_t{0},
        [this, &context, &config, &camera](const uint8_t&) {
            return Prepare(context, config, camera);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "Skybox",
        [resources](RenderGraph::PassBuilder& builder) {
            // Skybox disables depth writes, but the attachment STORE operation
            // still writes memory at the end of rendering.
            builder.ReadWrite(resources.depth, RenderGraph::TextureState::DepthStencilWrite)
                .Read(resources.cubemap, RenderGraph::TextureState::Sampled)
                .ReadWrite(resources.lighting_output, RenderGraph::TextureState::RenderTarget);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Skybox", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle ProbeGizmoPass::AddToGraph(
    RenderGraph& graph,
    RasterContext& context,
    RasterConfig config,
    const Camera& camera,
    GraphResources resources,
    std::span<const RenderGraph::SetupPassHandle> setup_dependencies
) {
    auto prepared = graph.AddSetupPass(
        "ProbeGizmo.Prepare",
        uint8_t{0},
        [this, &context, config = std::move(config), &camera](const uint8_t&) {
            return Prepare(context, config, camera);
        },
        setup_dependencies
    );
    if (!prepared.IsValid()) {
        return {};
    }

    const RasterProbeGraphReads probes(graph, context.probe_volume);
    auto                        pass = graph.AddRecordPass(
        "ProbeGizmo",
        [resources, probes](RenderGraph::PassBuilder& builder) {
            probes.Declare(builder);

            builder.Read(resources.probe_volume)
                .ReadWrite(resources.lighting_output, RenderGraph::TextureState::RenderTarget)
                .SideEffect();
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "ProbeGizmo", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle AoPass::AddToGraph(
    RenderGraph& graph,
    RasterContext& context,
    const RasterConfig& ui_config,
    const Camera& camera,
    uint64 frame_index,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "AmbientOcclusion.Prepare",
        uint8_t{0},
        [this, &context, &ui_config, &camera, frame_index](const uint8_t&) {
            return Prepare(context, ui_config, camera, frame_index);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    graph.AddRecordPass(
        "AmbientOcclusion.UploadMotion",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(resources.motion_vector_data, RenderGraph::BufferState::TransferDestination);
        },
        [this, prepared](CommandList& commands) {
            RecordUpload(commands, prepared.Get());
        }
    );
    const bool ray_query = ui_config.ao_mode == EAoMode::RTAO || ui_config.ao_mode == EAoMode::RTAO_AO_ONLY;
    auto       pass      = graph.AddRecordPass(
        "AmbientOcclusion",
        [resources, ray_query](RenderGraph::PassBuilder& builder) {
            builder
                .ExecuteOn(
                    RenderGraph::QueueRole::Graphics,
                    ray_query ? RenderGraph::PipelineType::Compute : RenderGraph::PipelineType::Graphics
                )
                .Read(resources.normal, RenderGraph::TextureState::Sampled)
                .Read(resources.depth, RenderGraph::TextureState::Sampled)
                .Read(resources.lighting_output, RenderGraph::TextureState::Sampled)
                .Write(
                    resources.ao_only,
                    ray_query ? RenderGraph::TextureState::UnorderedAccess :
                                           RenderGraph::TextureState::RenderTarget
                )
                .Write(
                    resources.camera_motion_vector,
                    ray_query ? RenderGraph::TextureState::UnorderedAccess :
                                           RenderGraph::TextureState::RenderTarget
                )
                .Read(resources.motion_vector_data, RenderGraph::BufferState::ShaderResource);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "AmbientOcclusion", [&] {
                RecordEvaluate(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle AoPass::AddCompositeToGraph(
    RenderGraph& graph,
    RasterContext& context,
    const RasterConfig& ui_config,
    uint ao_only_handle,
    CompositeGraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "AoComposite.Prepare",
        uint8_t{0},
        [this, &context, &ui_config, ao_only_handle](const uint8_t&) {
            return PrepareComposite(context, ui_config, ao_only_handle);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "AoComposite",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .Read(resources.ao_only, RenderGraph::TextureState::Sampled)
                .Read(resources.lighting_output, RenderGraph::TextureState::Sampled)
                .Read(resources.depth, RenderGraph::TextureState::Sampled)
                .Read(resources.normal, RenderGraph::TextureState::Sampled)
                .Write(resources.ao_output, RenderGraph::TextureState::UnorderedAccess);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "AoComposite", [&] {
                RecordComposite(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle RtaoDenoiserPass::AddToGraph(
    RenderGraph& graph,
    RasterContext& context,
    const RasterConfig& ui_config,
    uint ao_only_index,
    GraphResources resources
) {
    if (!ui_config.rtao_denoiser_enable ||
        (ui_config.ao_mode != EAoMode::RTAO && ui_config.ao_mode != EAoMode::RTAO_AO_ONLY)) {
        return {};
    }
    auto prepared = graph.AddSetupPass(
        "RtaoDenoise.Prepare",
        uint8_t{0},
        [this, &context, &ui_config, ao_only_index](const uint8_t&) {
            return Prepare(context, ui_config, ao_only_index);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "RtaoDenoise",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.normal, RenderGraph::TextureState::Sampled)
                .Read(resources.depth, RenderGraph::TextureState::Sampled)
                .Read(resources.camera_motion_vector, RenderGraph::TextureState::Sampled)
                .Read(resources.history_read, RenderGraph::TextureState::Sampled)
                .Read(resources.ao_only, RenderGraph::TextureState::Sampled)
                .Write(resources.history_write, RenderGraph::TextureState::RenderTarget);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "RtaoDenoise", [&] {
                RecordDenoise(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    pass = graph.AddRecordPass(
        "RtaoDenoise.Copy",
        [=](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.history_write, RenderGraph::TextureState::Sampled)
                .Write(resources.ao_only, RenderGraph::TextureState::RenderTarget);
        },
        [this, prepared](CommandList& commands) {
            RecordCopy(commands, prepared.Get());
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle BilateralFilterDenoiserPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& ui_config,
    TextureWithHandle input_image,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "BilateralDenoise.Prepare",
        std::move(input_image),
        [this, &context, &ui_config](const TextureWithHandle& input) {
            return Prepare(context, ui_config, input);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "BilateralDenoise",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.input, RenderGraph::TextureState::Sampled)
                .Write(resources.denoiser_output, RenderGraph::TextureState::RenderTarget);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "BilateralDenoise", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle SsrPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& ui_config,
    const Camera& camera,
    TextureWithHandle input_image,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "ScreenSpaceReflection.Prepare",
        std::move(input_image),
        [this, &context, &ui_config, &camera](const TextureWithHandle& input) {
            return Prepare(context, ui_config, camera, input);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "ScreenSpaceReflection",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.input, RenderGraph::TextureState::Sampled)
                .Read(resources.normal, RenderGraph::TextureState::Sampled)
                .Read(resources.depth, RenderGraph::TextureState::Sampled)
                .Read(resources.metal_rough_ao, RenderGraph::TextureState::Sampled)
                .Write(resources.ssr_output, RenderGraph::TextureState::RenderTarget);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "ScreenSpaceReflection", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle AaPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& ui_config,
    const Camera& camera,
    TextureWithHandle input_image,
    uint8 smaa_phase,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "AntiAliasing.Prepare",
        std::move(input_image),
        [this, &context, &ui_config, &camera, smaa_phase](const TextureWithHandle& input) {
            return Prepare(context, ui_config, camera, input, smaa_phase);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    using State = RenderGraph::TextureState;
    const auto import = [&](std::string_view name, const TextureWithHandle& texture) {
        return ImportRasterTexture(graph, name, texture.tex.Get());
    };
    const auto texture_1 = import("AA.edges_or_luma", context.textures.aa_texture_1);
    const bool smaa = ui_config.aa_mode >= EAaMode::SMAA_1X;
    const bool temporal = ui_config.aa_mode == EAaMode::SMAA_T2X;
    graph.AddRecordPass(
        smaa ? "AA.Edges" : "AA.Luma",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .Read(resources.input, State::Sampled).Write(texture_1, State::RenderTarget);
            if (temporal) { builder.Read(resources.depth, State::Sampled); }
        },
        [this, prepared](CommandList& commands) { RecordEdgesOrLuma(commands, prepared.Get()); },
        RenderGraph::PassExecutionClass::ParallelRecordEligible);
    RenderGraph::TextureHandle texture_2{};
    if (smaa) {
        texture_2 = import("AA.weights", context.textures.aa_texture_2);
        const auto area = import("AA.area", smaa_area_tex);
        const auto search = import("AA.search", smaa_search_tex);
        graph.AddRecordPass(
            "AA.Weights",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                    .Read(texture_1, State::Sampled).Read(area, State::Sampled).Read(search, State::Sampled)
                    .Write(texture_2, State::RenderTarget);
            },
            [this, prepared](CommandList& commands) { RecordWeights(commands, prepared.Get()); },
            RenderGraph::PassExecutionClass::ParallelRecordEligible);
    }
    const auto blend_output = temporal ? import("AA.current", *aa_texture_34[smaa_phase]) : resources.aa_output;
    auto pass = graph.AddRecordPass(
        "AA.Blend",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .Read(texture_1, State::Sampled).Write(blend_output, State::RenderTarget);
            if (smaa) { builder.Read(resources.input, State::Sampled).Read(texture_2, State::Sampled); }
        },
        [this, prepared](CommandList& commands) { RecordBlend(commands, prepared.Get()); },
        RenderGraph::PassExecutionClass::ParallelRecordEligible);
    if (temporal) {
        // An invalid history samples the current output, exactly as Prepare's shader handle does.
        const auto previous = history_valid ? import("AA.previous", *aa_texture_34[smaa_phase ^ 1u]) : blend_output;
        pass = graph.AddRecordPass(
            "AA.Resolve",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                    .Read(blend_output, State::Sampled).Read(previous, State::Sampled)
                    .Read(texture_1, State::Sampled).Write(resources.aa_output, State::RenderTarget);
            },
            [this, prepared](CommandList& commands) { RecordResolve(commands, prepared.Get()); },
            RenderGraph::PassExecutionClass::ParallelRecordEligible);
    }
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle BloomPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& raster_config,
    TextureWithHandle input_texture,
    GraphResources resources
) {
    if (!raster_config.bloom_enabled) {
        return {};
    }
    auto prepared = graph.AddSetupPass(
        "Bloom.Prepare",
        std::move(input_texture),
        [this, &context, &raster_config](const TextureWithHandle& input) {
            return Prepare(context, raster_config, input);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = AddRecordPasses(graph, resources, prepared,
                               context.textures.bloom_downsample_chain.tex->GetNumMips());
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PassHandle BloomPass::AddRecordPasses(
    RenderGraph& graph, GraphResources resources, RGPreparedValue<RecordParameters> prepared, uint32 mip_count
) {
    using Range = RenderGraph::TextureRange;
    using State = RenderGraph::TextureState;
    if (mip_count == 0) {
        throw std::invalid_argument("Bloom graph requires at least one mip");
    }
    // Every draw is one synchronization boundary. Different mips of the same
    // image can be sampled and written concurrently; never declare whole-chain writes.
    graph.AddRecordPass(
        "Bloom.Prefilter",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .Read(resources.input, State::Sampled, Range::Mips(0, 1))
                .Write(resources.downsample_chain, State::RenderTarget, Range::Mips(0, 1));
        },
        [this, prepared](CommandList& commands) { RecordPrefilter(commands, prepared.Get()); },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    for (uint32 mip = 1; mip < mip_count; ++mip) {
        graph.AddRecordPass(
            std::format("Bloom.Downsample{}", mip),
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                    .Read(resources.downsample_chain, State::Sampled, Range::Mips(mip - 1, 1))
                    .Write(resources.downsample_chain, State::RenderTarget, Range::Mips(mip, 1));
            },
            [this, prepared, mip](CommandList& commands) { RecordDownsample(commands, prepared.Get(), mip); },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    }
    for (uint32 source_mip = mip_count - 1; source_mip > 0; --source_mip) {
        const uint32 mip = source_mip - 1;
        const auto source = source_mip == mip_count - 1 ? resources.downsample_chain : resources.upsample_chain;
        graph.AddRecordPass(
            std::format("Bloom.Upsample{}", mip),
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                    .Read(source, State::Sampled, Range::Mips(source_mip, 1))
                    .Read(resources.downsample_chain, State::Sampled, Range::Mips(mip, 1))
                    .Write(resources.upsample_chain, State::RenderTarget, Range::Mips(mip, 1));
            },
            [this, prepared, mip](CommandList& commands) { RecordUpsample(commands, prepared.Get(), mip); },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    }
    return graph.AddRecordPass(
        "Bloom.Apply",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .Read(mip_count == 1 ? resources.downsample_chain : resources.upsample_chain,
                      State::Sampled, Range::Mips(0, 1))
                // Additive blending loads the existing scene color.
                .ReadWrite(resources.input, State::RenderTarget, Range::Mips(0, 1));
        },
        [this, prepared](CommandList& commands) { RecordApply(commands, prepared.Get()); },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
}

RenderGraph::PreparedPassHandle TonemappingPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& ui_config,
    TextureWithHandle input_image,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "Tonemapping.Prepare",
        std::move(input_image),
        [this, &context, &ui_config](const TextureWithHandle& input) {
            return Prepare(context, ui_config, input);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    return {prepared.GetSetupPass(), AddRecordPasses(graph, resources, prepared)};
}

RenderGraph::PassHandle TonemappingPass::AddRecordPasses(
    RenderGraph& graph, GraphResources resources, RGPreparedValue<RecordParameters> prepared
) {
    graph.AddRecordPass(
        "Tonemapping.ClearHistogram",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(resources.histogram, RenderGraph::BufferState::TransferDestination);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Tonemapping.ClearHistogram", [&] {
                RecordClear(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "Tonemapping.BuildHistogram",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .Read(resources.input, RenderGraph::TextureState::Sampled)
                .ReadWrite(resources.histogram, RenderGraph::BufferState::UnorderedAccess);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Tonemapping.BuildHistogram", [&] {
                RecordHistogram(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    graph.AddRecordPass(
        "Tonemapping.CalculateExposure",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                .Read(resources.histogram, RenderGraph::BufferState::ShaderResource)
                .ReadWrite(resources.exposure, RenderGraph::BufferState::UnorderedAccess);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Tonemapping.CalculateExposure", [&] {
                RecordExposure(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return graph.AddRecordPass(
        "Tonemapping.Apply",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Graphics)
                .Read(resources.input, RenderGraph::TextureState::Sampled)
                .Read(resources.histogram, RenderGraph::BufferState::ShaderResource)
                .Read(resources.exposure, RenderGraph::BufferState::ShaderResource)
                .Write(resources.tonemapping_output, RenderGraph::TextureState::RenderTarget);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Tonemapping.Apply", [&] {
                RecordTonemapping(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
}

RenderGraph::PreparedPassHandle CameraGizmoPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const Camera& scene_camera,
    const Camera& main_camera,
    GraphResources resources
) {
    auto prepared = graph.AddSetupPass(
        "CameraGizmo.Prepare",
        uint8_t{0},
        [this, &context, &scene_camera, &main_camera](const uint8_t&) {
            return Prepare(context, scene_camera, main_camera);
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "CameraGizmo",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ReadWrite(resources.tonemapping_output, RenderGraph::TextureState::RenderTarget)
                .SideEffect();
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "CameraGizmo", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle CsmGizmoPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& config,
    const SceneViewGizmoConfig& gizmos,
    const Camera& camera,
    const Camera& main_camera,
    GraphResources resources,
    std::span<const RenderGraph::SetupPassHandle> setup_dependencies
) {
    auto prepared = graph.AddSetupPass(
        "CsmGizmo.Prepare",
        uint8_t{0},
        [this, &context, &config, &gizmos, &camera, &main_camera](const uint8_t&) {
            return Prepare(context, config, gizmos, camera, main_camera);
        },
        setup_dependencies
    );
    if (!prepared.IsValid()) {
        return {};
    }

    const auto data = ImportRasterBuffer(graph, "CsmGizmo.data", m_cascade_data_buffer.buf.Get());
    graph.AddRecordPass(
        "CsmGizmo.Upload",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(data, RenderGraph::BufferState::TransferDestination);
        },
        [this, prepared](CommandList& commands) {
            RecordUpload(commands, prepared.Get());
        }
    );
    auto pass = graph.AddRecordPass(
        "CsmGizmo",
        [resources, data](RenderGraph::PassBuilder& builder) {
            builder.Read(data, RenderGraph::BufferState::ShaderResource);
            builder.Read(resources.shadow_maps)
                .ReadWrite(resources.tonemapping_output, RenderGraph::TextureState::RenderTarget)
                .SideEffect();
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "CsmGizmo", [&] {
                RecordDraw(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

} // namespace Moer::Render::Raster

namespace Moer::Render {

RenderGraph::PreparedPassHandle UiCombinePass::AddToGraph(
    RenderGraph& graph,
    bool ui_enabled,
    bool writes_external_window,
    bool is_separate_window,
    uint2 resolution,
    float2 scene_color_position,
    float2 scene_color_resolution,
    TextureView window_framebuffer,
    TextureView selected_framebuffer,
    TextureView default_output,
    TextureView processing_input,
    GraphResources resources
) {
    struct PrepareInput {
        bool        is_separate_window{};
        uint2       resolution{};
        float2      scene_color_position{};
        float2      scene_color_resolution{};
        TextureView window_framebuffer{};
        TextureView input_color{};
        TextureView default_output{};
    };

    PrepareInput input{
        .is_separate_window = ui_enabled ? is_separate_window : true,
        .resolution = resolution,
        .scene_color_position = ui_enabled ? scene_color_position : float2(0.f, 0.f),
        .scene_color_resolution = ui_enabled ?
            scene_color_resolution :
            float2(static_cast<float>(resolution.x), static_cast<float>(resolution.y)),
        .window_framebuffer = ui_enabled ? window_framebuffer : default_output,
        .input_color = ui_enabled ? selected_framebuffer : processing_input,
        .default_output = default_output
    };

    auto prepared = graph.AddSetupPass(
        "UiCombine.Prepare",
        std::move(input),
        [this](const PrepareInput& setup) {
            return Prepare(
                setup.is_separate_window,
                setup.resolution,
                setup.scene_color_position,
                setup.scene_color_resolution,
                setup.window_framebuffer,
                setup.input_color,
                setup.default_output
            );
        }
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "UiCombine",
        [resources, ui_enabled, writes_external_window](RenderGraph::PassBuilder& builder) {
            if (!ui_enabled) {
                builder.Read(resources.processing_input, RenderGraph::TextureState::Sampled)
                    .Write(resources.output, RenderGraph::TextureState::RenderTarget);
            } else if (writes_external_window) {
                builder.Read(resources.selected_framebuffer, RenderGraph::TextureState::Sampled)
                    .Write(resources.window_framebuffer, RenderGraph::TextureState::RenderTarget);
            } else {
                builder.Read(resources.selected_framebuffer, RenderGraph::TextureState::Sampled)
                    .Write(resources.output, RenderGraph::TextureState::RenderTarget);
            }
            builder.SideEffect();
        },
        [this, prepared](CommandList& cmd_list) {
            Raster::RecordWithPassMarker(cmd_list, "UiCombine", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

} // namespace Moer::Render
