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

} // namespace

RenderGraph::PreparedPassHandle ShadowDepthPass::AddToGraph(
    RenderGraph& graph,
    RasterContext& context,
    const RasterConfig& ui_config,
    const Camera& camera,
    GraphResources resources,
    std::span<const RenderGraph::SetupPassHandle> setup_dependencies
) {
    auto prepared = graph.AddSetupPass(
        "ShadowDepth.Prepare",
        uint8_t{0},
        [this, &context, &ui_config, &camera](const uint8_t&) {
            return Prepare(context, ui_config, camera);
        },
        setup_dependencies,
        RenderGraph::PrepareSafety::Unsafe
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "ShadowDepth",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Write(resources.shadow_maps).SideEffect();
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "ShadowDepth", [&] {
                cmd_list.PushScopeWithTimeScope(
                    RasterTool::GetShadowDepthPassProfileScopeName()
                );
                Record(cmd_list, prepared.Get());
                cmd_list.PopScopeWithTimeScope();
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle ProbeUpdatePass::AddToGraph(
    RenderGraph& graph,
    RasterContext& context,
    const RasterConfig& config,
    const Camera& camera,
    uint64 frame_index,
    GraphResources resources,
    std::span<const RenderGraph::SetupPassHandle> setup_dependencies
) {
    auto prepared = graph.AddSetupPass(
        "ProbeUpdate.Prepare",
        uint8_t{0},
        [this, &context, &config, &camera, frame_index](const uint8_t&) {
            return Prepare(context, config, camera, frame_index);
        },
        setup_dependencies,
        RenderGraph::PrepareSafety::Unsafe
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "ProbeUpdate",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ReadWrite(resources.probe_volume)
                .SideEffect();
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "ProbeUpdate", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle GeometryPass::AddToGraph(
    RenderGraph& graph,
    RasterContext& context,
    RasterConfig& raster_config,
    const Camera& camera,
    GraphResources resources,
    std::span<const RenderGraph::SetupPassHandle> setup_dependencies
) {
    auto prepared = graph.AddSetupPass(
        "Geometry.Prepare",
        uint8_t{0},
        [this, &context, &raster_config, &camera](const uint8_t&) {
            return Prepare(context, raster_config, camera);
        },
        setup_dependencies,
        RenderGraph::PrepareSafety::Unsafe
    );
    if (!prepared.IsValid()) {
        return {};
    }

    auto pass = graph.AddRecordPass(
        "Geometry",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.hiz_previous)
                .Write(resources.base_color)
                .Write(resources.normal)
                .Write(resources.metal_rough_ao)
                .Write(resources.depth);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Geometry", [&] {
                cmd_list.PushScopeWithTimeScope(
                    RasterTool::GetGeometryPassProfileScopeName()
                );
                Record(cmd_list, prepared.Get());
                cmd_list.PopScopeWithTimeScope();
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle TessellatedSurfacePass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& config,
    const Camera& camera,
    GraphResources resources
) {
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

    auto pass = graph.AddRecordPass(
        "TessellatedSurface",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ReadWrite(resources.base_color)
                .ReadWrite(resources.normal)
                .ReadWrite(resources.metal_rough_ao)
                .ReadWrite(resources.depth);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "TessellatedSurface", [&] {
                Record(cmd_list, prepared.Get());
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

    auto pass = graph.AddRecordPass(
        "DirectionalShadowMask",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.normal)
                .Read(resources.depth)
                .Read(resources.lighting_data)
                .Read(resources.shadow_maps)
                .Write(resources.shadow_mask);
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

    auto pass = graph.AddRecordPass(
        "Lighting",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.base_color)
                .Read(resources.normal)
                .Read(resources.metal_rough_ao)
                .Read(resources.depth)
                .Read(resources.shadow_mask)
                .Read(resources.lighting_data)
                .Read(resources.cubemap)
                .Read(resources.probe_volume)
                .Write(resources.lighting_output);
            if (resources.scene_lights.IsValid()) {
                builder.Read(resources.scene_lights);
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
            builder.Read(resources.depth)
                .Read(resources.cubemap)
                .ReadWrite(resources.lighting_output);
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

    auto pass = graph.AddRecordPass(
        "ProbeGizmo",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.probe_volume)
                .ReadWrite(resources.lighting_output)
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

    auto pass = graph.AddRecordPass(
        "AmbientOcclusion",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.normal)
                .Read(resources.depth)
                .Read(resources.lighting_output)
                .Write(resources.ao_only)
                .Write(resources.camera_motion_vector)
                .Write(resources.motion_vector_data);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "AmbientOcclusion", [&] {
                Record(cmd_list, prepared.Get());
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
            builder.Read(resources.ao_only)
                .Read(resources.lighting_output)
                .Read(resources.depth)
                .Read(resources.normal)
                .Write(resources.ao_output);
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
            builder.Read(resources.normal)
                .Read(resources.depth)
                .Read(resources.camera_motion_vector)
                .Read(resources.history_read)
                .ReadWrite(resources.ao_only)
                .Write(resources.history_write);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "RtaoDenoise", [&] {
                Record(cmd_list, prepared.Get());
            });
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
            builder.Read(resources.input)
                .Write(resources.denoiser_output);
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
            builder.Read(resources.input)
                .Read(resources.normal)
                .Read(resources.depth)
                .Read(resources.metal_rough_ao)
                .Write(resources.ssr_output);
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
        return graph.ImportTexture(name, texture.tex,
            RenderGraph::TextureDesc{.mip_count = texture.tex->GetNumMips(),
                                     .layer_count = texture.tex->GetNumArray()});
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
            builder.ReadWrite(resources.tonemapping_output)
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

    auto pass = graph.AddRecordPass(
        "CsmGizmo",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.shadow_maps)
                .ReadWrite(resources.tonemapping_output)
                .SideEffect();
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "CsmGizmo", [&] {
                Record(cmd_list, prepared.Get());
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
                builder.Read(resources.processing_input).Write(resources.output);
            } else if (writes_external_window) {
                builder.Read(resources.selected_framebuffer)
                    .Write(resources.window_framebuffer);
            } else {
                builder.Read(resources.selected_framebuffer).Write(resources.output);
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
