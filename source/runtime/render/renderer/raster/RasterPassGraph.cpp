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
            builder.Read(resources.scene).Write(resources.shadow_maps).SideEffect();
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
            builder.Read(resources.scene)
                .ReadWrite(resources.probe_volume)
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
            builder.Read(resources.scene)
                .Read(resources.hiz_previous)
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

    auto pass = graph.AddRecordPass(
        "HiZBuild",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.depth).Write(resources.hiz_current);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "HiZBuild", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
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

    auto pass = graph.AddRecordPass(
        "AntiAliasing",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.input)
                .Read(resources.depth)
                .Write(resources.aa_output);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "AntiAliasing", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
}

RenderGraph::PreparedPassHandle BloomPass::AddToGraph(
    RenderGraph& graph,
    const RasterContext& context,
    const RasterConfig& raster_config,
    TextureWithHandle input_texture,
    GraphResources resources
) {
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

    auto pass = graph.AddRecordPass(
        "Bloom",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.ReadWrite(resources.input)
                .Write(resources.downsample_chain)
                .Write(resources.upsample_chain);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Bloom", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
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

    auto pass = graph.AddRecordPass(
        "Tonemapping",
        [resources](RenderGraph::PassBuilder& builder) {
            builder.Read(resources.input)
                .ReadWrite(resources.histogram)
                .ReadWrite(resources.exposure)
                .Write(resources.tonemapping_output);
        },
        [this, prepared](CommandList& cmd_list) {
            RecordWithPassMarker(cmd_list, "Tonemapping", [&] {
                Record(cmd_list, prepared.Get());
            });
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );
    return {prepared.GetSetupPass(), pass};
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
            builder.Read(resources.scene)
                .ReadWrite(resources.tonemapping_output)
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
