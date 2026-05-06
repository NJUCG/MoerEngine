#include "RaytracingRenderer.h"

#include "../../../../editor/raytracing_ui/RaytracingUI.h"

// Runtime
#include "PixelFormat.h"
#include "RaytracingConfig.h"
#include "config/ConfigManager.h"
#include "misc/BoundingBox.h"
#include "misc/Timer.h"
#include "renderer/common/RuntimeAssets.h"
#include "rendergraph/RenderGraph.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHIResource.h"
#include "rhi/plugin/NrdPlugin.h"
#include "rhi/vulkan/VulkanRHITrace.h"
#include "scene/GpuScene.h"
#include "string/Format.h"
#include "string/String.h"
#include "string/StringConvert.h"
#include "taskgraph/TaskGraph.h"
#include "trace/Trace.h"
#include "window/WindowContext.h"

// Editor
#include "AntiAliasPass.h"
#include "CompositionPass.h"
#include "Configs.h" // TODO: merge it with raster
#include "GBufferPass.h"
#include "LightingPass.h"
#include "PreprocessLightPass.h"
#include "RTResource.h"
#include "ShaderUtils.h"
#include "ToneMappingPass.h"
#include "VisualizePass.h"

// 3rd party
#include <atomic>
#include <stb/stb_image_write.h>

namespace Moer::Render::Raytracing {

union FloatBits {
    float        f;
    unsigned int ui;
};

static Box3D scene_bounding{};

struct RGRaytracingPrepareLightsParams {
    DEFINE_RG_TEXTURE_ACCESS(local_light_pdf_tex, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(primitive_to_light_buf, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(task_buf, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(prim_light_buf, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(light_mapping_buf, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(light_data_buf, EBufferState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(
        local_light_pdf_tex,
        primitive_to_light_buf,
        task_buf,
        prim_light_buf,
        light_mapping_buf,
        light_data_buf
    );
};

struct RGRaytracingGBufferParams {
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_albedo, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(clip_depth, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(normal_roughness, ETextureState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(
        view_depth,
        diffuse_albedo,
        specular_roughness,
        normal,
        emission,
        motion,
        clip_depth,
        normal_roughness
    );
};

struct RGRaytracingLightingParams {
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(temporal_sample_pos, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(gradients, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(restir_luminance, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(prev_diffuse_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(light_reservoir_buf, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(ris_buf, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(ris_light_data_buf, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(neighbor_offset_buf, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(light_data_buf, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(light_mapping_buf, EBufferState::SHADER_RESOURCE);

    DEFINE_RG_PARAMETER_ACCESS(
        view_depth,
        normal_roughness,
        diffuse_lighting,
        specular_lighting,
        temporal_sample_pos,
        gradients,
        restir_luminance,
        prev_diffuse_lighting,
        light_reservoir_buf,
        ris_buf,
        ris_light_data_buf,
        neighbor_offset_buf,
        light_data_buf,
        light_mapping_buf
    );
};

struct RGRaytracingNrdParams {
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(denoised_diffuse_lighting, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(denoised_specular_lighting, ETextureState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(
        motion,
        normal_roughness,
        view_depth,
        diffuse_lighting,
        specular_lighting,
        denoised_diffuse_lighting,
        denoised_specular_lighting
    );
};

struct RGRaytracingCompositionParams {
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_albedo, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_roughness, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(normal, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(denoised_diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(denoised_specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(hdr_color, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(
        view_depth,
        diffuse_albedo,
        specular_roughness,
        normal,
        emission,
        diffuse_lighting,
        specular_lighting,
        denoised_diffuse_lighting,
        denoised_specular_lighting,
        hdr_color,
        motion
    );
};

struct RGRaytracingAntiAliasParams {
    DEFINE_RG_TEXTURE_ACCESS(hdr_color, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(motion, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(feedback_color_ping, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(resolved_color, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(feedback_color_pong, ETextureState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(hdr_color, motion, feedback_color_ping, resolved_color, feedback_color_pong);
};

struct RGRaytracingToneMappingParams {
    DEFINE_RG_TEXTURE_ACCESS(resolved_color, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(ldr_color, ETextureState::RENDER_TARGET);

    DEFINE_RG_PARAMETER_ACCESS(resolved_color, ldr_color);
};

struct RGRaytracingVisualizeParams {
    DEFINE_RG_TEXTURE_ACCESS(ldr_color, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(diffuse_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(specular_lighting, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(view_depth, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(emission, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(debug_color, ETextureState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(
        ldr_color,
        diffuse_lighting,
        specular_lighting,
        view_depth,
        emission,
        debug_color
    );
};

struct RGRaytracingShowTextureParams {
    DEFINE_RG_TEXTURE_ACCESS(selected_texture, ETextureState::SHADER_RESOURCE);
    DEFINE_RG_TEXTURE_ACCESS(ldr_color, ETextureState::UNORDERED_ACCESS);

    DEFINE_RG_PARAMETER_ACCESS(selected_texture, ldr_color);
};

struct RGRaytracingFrameResources {
    RenderGraphHandle view_depth{};
    RenderGraphHandle diffuse_albedo{};
    RenderGraphHandle specular_roughness{};
    RenderGraphHandle normal{};
    RenderGraphHandle emission{};
    RenderGraphHandle motion{};
    RenderGraphHandle clip_depth{};
    RenderGraphHandle prev_view_depth{};
    RenderGraphHandle prev_diffuse_albedo{};
    RenderGraphHandle prev_specular_roughness{};
    RenderGraphHandle prev_normal{};
    RenderGraphHandle normal_roughness{};
    RenderGraphHandle diffuse_lighting{};
    RenderGraphHandle prev_diffuse_lighting{};
    RenderGraphHandle specular_lighting{};
    RenderGraphHandle prev_specular_lighting{};
    RenderGraphHandle temporal_sample_pos{};
    RenderGraphHandle gradients{};
    RenderGraphHandle restir_luminance{};
    RenderGraphHandle prev_luminance{};
    RenderGraphHandle denoised_diffuse_lighting{};
    RenderGraphHandle denoised_specular_lighting{};
    RenderGraphHandle debug_color{};
    RenderGraphHandle ldr_color{};
    RenderGraphHandle hdr_color{};
    RenderGraphHandle feedback_color_ping{};
    RenderGraphHandle feedback_color_pong{};
    RenderGraphHandle resolved_color{};
    RenderGraphHandle local_light_pdf_tex{};
    RenderGraphHandle light_mapping_buf{};
    RenderGraphHandle prim_light_buf{};
    RenderGraphHandle task_buf{};
    RenderGraphHandle primitive_to_light_buf{};
    RenderGraphHandle light_data_buf{};
    RenderGraphHandle ris_buf{};
    RenderGraphHandle ris_light_data_buf{};
    RenderGraphHandle neighbor_offset_buf{};
    RenderGraphHandle light_reservoir_buf{};
};

static RenderGraphHandle
ImportTextureIfValid(RenderGraph& graph, StringView name, const TextureRef& texture) {
    return texture ? graph.ImportTexture(name, texture, EQueueType::Graphics) : RenderGraphHandle{};
}

static RenderGraphHandle ImportBufferIfValid(RenderGraph& graph, StringView name, const BufferRef& buffer) {
    return buffer ? graph.ImportBuffer(name, buffer, EQueueType::Graphics) : RenderGraphHandle{};
}

static RGRaytracingFrameResources
ImportRaytracingFrameResources(RenderGraph& graph, const RTContext& rt_ctx) {
    const FrameResources& frame_rt = rt_ctx.frame_rt;
    return RGRaytracingFrameResources{
        .view_depth = ImportTextureIfValid(graph, MOER_TEXT("RT.view_depth"), frame_rt.view_depth),
        .diffuse_albedo =
            ImportTextureIfValid(graph, MOER_TEXT("RT.diffuse_albedo"), frame_rt.diffuse_albedo),
        .specular_roughness =
            ImportTextureIfValid(graph, MOER_TEXT("RT.specular_roughness"), frame_rt.specular_roughness),
        .normal     = ImportTextureIfValid(graph, MOER_TEXT("RT.normal"), frame_rt.normal),
        .emission   = ImportTextureIfValid(graph, MOER_TEXT("RT.emission"), frame_rt.emission),
        .motion     = ImportTextureIfValid(graph, MOER_TEXT("RT.motion"), frame_rt.motion),
        .clip_depth = ImportTextureIfValid(graph, MOER_TEXT("RT.clip_depth"), frame_rt.clip_depth),
        .prev_view_depth =
            ImportTextureIfValid(graph, MOER_TEXT("RT.prev_view_depth"), frame_rt.prev_view_depth),
        .prev_diffuse_albedo =
            ImportTextureIfValid(graph, MOER_TEXT("RT.prev_diffuse_albedo"), frame_rt.prev_diffuse_albedo),
        .prev_specular_roughness = ImportTextureIfValid(
            graph, MOER_TEXT("RT.prev_specular_roughness"), frame_rt.prev_specular_roughness
        ),
        .prev_normal = ImportTextureIfValid(graph, MOER_TEXT("RT.prev_normal"), frame_rt.prev_normal),
        .normal_roughness =
            ImportTextureIfValid(graph, MOER_TEXT("RT.normal_roughness"), frame_rt.normal_roughness),
        .diffuse_lighting =
            ImportTextureIfValid(graph, MOER_TEXT("RT.diffuse_lighting"), frame_rt.diffuse_lighting),
        .prev_diffuse_lighting = ImportTextureIfValid(
            graph, MOER_TEXT("RT.prev_diffuse_lighting"), frame_rt.prev_diffuse_lighting
        ),
        .specular_lighting =
            ImportTextureIfValid(graph, MOER_TEXT("RT.specular_lighting"), frame_rt.specular_lighting),
        .prev_specular_lighting = ImportTextureIfValid(
            graph, MOER_TEXT("RT.prev_specular_lighting"), frame_rt.prev_specular_lighting
        ),
        .temporal_sample_pos =
            ImportTextureIfValid(graph, MOER_TEXT("RT.temporal_sample_pos"), frame_rt.temporal_sample_pos),
        .gradients = ImportTextureIfValid(graph, MOER_TEXT("RT.gradients"), frame_rt.gradients),
        .restir_luminance =
            ImportTextureIfValid(graph, MOER_TEXT("RT.restir_luminance"), frame_rt.restir_luminance),
        .prev_luminance =
            ImportTextureIfValid(graph, MOER_TEXT("RT.prev_luminance"), frame_rt.prev_luminance),
        .denoised_diffuse_lighting = ImportTextureIfValid(
            graph, MOER_TEXT("RT.denoised_diffuse_lighting"), frame_rt.denoised_diffuse_lighting
        ),
        .denoised_specular_lighting = ImportTextureIfValid(
            graph, MOER_TEXT("RT.denoised_specular_lighting"), frame_rt.denoised_specular_lighting
        ),
        .debug_color = ImportTextureIfValid(graph, MOER_TEXT("RT.debug_color"), frame_rt.debug_color),
        .ldr_color   = ImportTextureIfValid(graph, MOER_TEXT("RT.ldr_color"), frame_rt.ldr_color),
        .hdr_color   = ImportTextureIfValid(graph, MOER_TEXT("RT.hdr_color"), frame_rt.hdr_color),
        .feedback_color_ping =
            ImportTextureIfValid(graph, MOER_TEXT("RT.feedback_color_ping"), frame_rt.feedback_color_ping),
        .feedback_color_pong =
            ImportTextureIfValid(graph, MOER_TEXT("RT.feedback_color_pong"), frame_rt.feedback_color_pong),
        .resolved_color =
            ImportTextureIfValid(graph, MOER_TEXT("RT.resolved_color"), frame_rt.resolved_color),
        .local_light_pdf_tex =
            ImportTextureIfValid(graph, MOER_TEXT("RT.local_light_pdf_tex"), rt_ctx.local_light_pdf_tex),
        .light_mapping_buf =
            ImportBufferIfValid(graph, MOER_TEXT("RT.light_mapping_buf"), rt_ctx.light_mapping_buf),
        .prim_light_buf = ImportBufferIfValid(graph, MOER_TEXT("RT.prim_light_buf"), rt_ctx.prim_light_buf),
        .task_buf       = ImportBufferIfValid(graph, MOER_TEXT("RT.task_buf"), rt_ctx.task_buf),
        .primitive_to_light_buf =
            ImportBufferIfValid(graph, MOER_TEXT("RT.primitive_to_light_buf"), rt_ctx.primitive_to_light_buf),
        .light_data_buf = ImportBufferIfValid(graph, MOER_TEXT("RT.light_data_buf"), rt_ctx.light_data_buf),
        .ris_buf        = ImportBufferIfValid(graph, MOER_TEXT("RT.ris_buf"), rt_ctx.ris_buf),
        .ris_light_data_buf =
            ImportBufferIfValid(graph, MOER_TEXT("RT.ris_light_data_buf"), rt_ctx.ris_light_data_buf),
        .neighbor_offset_buf =
            ImportBufferIfValid(graph, MOER_TEXT("RT.neighbor_offset_buf"), rt_ctx.neighbor_offset_buf),
        .light_reservoir_buf =
            ImportBufferIfValid(graph, MOER_TEXT("RT.light_reservoir_buf"), rt_ctx.light_reservoir_buf)
    };
}

static RenderGraphHandle
CurrentFrameTexture(bool current_frame, RenderGraphHandle current, RenderGraphHandle previous) {
    return current_frame ? current : previous;
}

RaytracingRenderer::RaytracingRenderer(
    uint2&                        _resolution,
    const SharedPtr<EditorConfig> _config,
    const EngineHooks&            _hooks,
    RuntimeAssets&                _runtime_assets
) :
    Renderer(_resolution, _config, _hooks, _runtime_assets) {}

void RaytracingRenderer::Run(const SharedPtr<EditorConfig> editor_config, const EngineHooks& hooks) {
    ::Moer::RaytracingUI config_ui(editor_config->raytracing_config);
    if (hooks.on_register_renderer_config_section) {
        hooks.on_register_renderer_config_section(
            "Raytracing", "Settings", [&config_ui](Synapse::Context& ui) {
                config_ui.ShowConfig(ui);
            }
        );
    }

    bool b_new_env_map = false;

    TextureRef         env_map{};
    Array<TextureView> env_mips;
    TextureRef         env_pdf{};
    Array<TextureView> env_pdf_mips;
    TextureWithHandle  env_tex_with_hdl;

    RaytracingSceneRef rt_scene = {};
    ShaderUtils        sd_utils(device, manager);

    ImportantSamplingParams is_params{};
    is_params.render_size = resolution;
    ImportanceSamplingContext is_ctx(is_params);

    bool first_load = true;

    UnorderedMap<String, TextureWithHandle> material_textures;

    float elapsed_time = 0.0f;

    TextureRef output = device.CreateTexture(
        MOER_TEXT("output"),
        Extent2D(resolution.x, resolution.y),
        presentation_surface->GetFormat(),
        ETextureUsageFlags::COLOR_ATTACHMENT
    );

    TextureRef combine_output = device.CreateTexture(
        MOER_TEXT("combine_output"),
        Extent2D(resolution.x, resolution.y),
        presentation_surface->GetFormat(),
        ETextureUsageFlags::COLOR_ATTACHMENT
    );

    auto create_frame_buffers = [&](uint2 _new_extent) {
        output = device.CreateTexture(
            MOER_TEXT("output"),
            Extent2D(_new_extent.x, _new_extent.y),
            presentation_surface->GetFormat(),
            ETextureUsageFlags::COLOR_ATTACHMENT
        );

        combine_output = device.CreateTexture(
            MOER_TEXT("combine_output"),
            Extent2D(_new_extent.x, _new_extent.y),
            presentation_surface->GetFormat(),
            ETextureUsageFlags::COLOR_ATTACHMENT
        );
    };

    Timer timer;
    timer.Start();
    uint64                last_time               = 0ull;
    bool                  b_feedback_valid        = false;
    bool                  b_trace_capture_started = false;
    bool                  b_trace_capture_armed   = false;
    bool                  b_export                = false;
    String                selected_material_texture_name{};
    bool                  b_final_show_texture = false;
    bool                  b_use_bindless       = true;
    uint                  mip_level            = 0;
    std::filesystem::path exported_file_path   = ConfigManager::GetInstance().GetWorkspacePath() / "saved";
    if (!std::filesystem::exists(exported_file_path)) {
        std::filesystem::create_directory(exported_file_path);
    }
    //////////////////////////////////////////////////////////////////////////
    // passes
    //////////////////////////////////////////////////////////////////////////
    SetRHITraceRuntimeEnabled(false);
    UniquePtr<PrepareLightPass> prepare_light_pass = MakeUnique<PrepareLightPass>(device, manager, scene);
    UniquePtr<GBufferPass>      g_buffer_pass      = MakeUnique<GBufferPass>(device, manager);
    UniquePtr<LightingPass>     lighting_pass      = MakeUnique<LightingPass>(manager);
    UniquePtr<CompositionPass>  composition_pass   = MakeUnique<CompositionPass>(device, manager);
    UniquePtr<VisualizePass>    visualize_pass     = MakeUnique<VisualizePass>(device, manager);
    UniquePtr<RTContext>        rt_ctx             = MakeUnique<RTContext>(sd_utils, is_ctx, bindless_array);
    UniquePtr<ToneMappingPass>  tone_mapping_pass;

    rt_ctx->SetResolution(resolution);
    AntialiasPass::CreateInfo antialias_pass_info{
        .motion              = rt_ctx->frame_rt.motion,
        .feedback_color_ping = rt_ctx->frame_rt.feedback_color_ping,
        .feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong,
        .resolved_color      = rt_ctx->frame_rt.resolved_color,
        .hdr_color           = rt_ctx->frame_rt.hdr_color
    };
    UniquePtr<AntialiasPass> antialias_pass =
        MakeUnique<AntialiasPass>(device, manager, scene, antialias_pass_info);

//////////////////////////////////////////////////////////////////////////
// NRD
//////////////////////////////////////////////////////////////////////////
#if WITH_NRD
    uint64 nrd_time      = 0ull;
    auto*  nrd_plugin    = device.LoadPlugin<Ext::NRDPlugin>();
    auto   nrd_interface = nrd_plugin->CreateInterface(max_frame_in_flight, resolution.x, resolution.y);
#endif

    VisualizeConfig visualize_config{};
    visualize_config.b_split        = false;
    visualize_config.split_ratio    = 0.5f;
    visualize_config.visualize_mode = EFC_DI;

    Array<std::function<void(uint)>> on_free_buffer_callbacks;

    auto add_on_free_buffer = [&](uint _buffer_handle) {
        on_free_buffer_callbacks.emplace_back([&, _buffer_handle](uint _timeline) {
            bindless_array->UnbindBuffer(_buffer_handle);
        });
    };

    auto add_on_free_texture = [&](uint _texture_handle) {
        on_free_buffer_callbacks.emplace_back([&, _texture_handle](uint _timeline) {
            bindless_array->UnbindTexture(_texture_handle);
        });
    };

    while (WindowContext::ShouldClose(WindowContext::GetMainWindow()) == false) {
        TRACE_SCOPE_CAT("Raytracing.Frame", "Frame");

        PumpAsyncLoads();

        RaytracingConfig& ui_config = editor_config->raytracing_config;

        LogSceneLoadStatus(*editor_config);

        auto window_state = TickWindowContext(hooks);
        bool skip_present = false;

        if (window_state == EWindowState::Hiding) {
            std::this_thread::yield();
            skip_present = true;

        } else if (window_state == EWindowState::SizeChanged) {
            create_frame_buffers(resolution);

            rt_ctx->SetResolution(resolution);

            is_ctx.~ImportanceSamplingContext();
            is_params.render_size = resolution;
            new (&is_ctx) ImportanceSamplingContext(is_params);

#if WITH_NRD
            nrd_interface =
                nrd_plugin->RecreateInterface(std::move(nrd_interface), resolution.x, resolution.y);
#endif

            antialias_pass_info.motion              = rt_ctx->frame_rt.motion;
            antialias_pass_info.feedback_color_ping = rt_ctx->frame_rt.feedback_color_ping;
            antialias_pass_info.feedback_color_pong = rt_ctx->frame_rt.feedback_color_pong;
            antialias_pass_info.resolved_color      = rt_ctx->frame_rt.resolved_color;
            antialias_pass_info.hdr_color           = rt_ctx->frame_rt.hdr_color;
            antialias_pass   = MakeUnique<AntialiasPass>(device, manager, scene, antialias_pass_info);
            b_feedback_valid = false;

        } else if (window_state == EWindowState::Default) {
            // do nothing

        } else {
            assert(false);
        }

        if (hooks.on_tick_ui) {
            hooks.on_tick_ui();
        }

        timer.Stop();
        auto frame_time = timer.ElapsedMilliseconds();
        timer.Start();

        Array<CommandList> pre_frame_cmd_lists{};

        if (scene.IsReady() && runtime_assets.IsReady()) {

            // 处理场景加载过程中遗留的命令
            // The gfx CommandList contains a CopyScope for all CPU→GPU uploads.
            // The executor splits the stream at scope boundaries, routes the enclosed
            // commands to the copy queue, and auto-generates acquire/release barriers.
            auto&& scene_cmd_list = scene.PopPendingCommandList();
            if (!scene_cmd_list.gfx_queue_cmd_list.IsEmpty()) {
                pre_frame_cmd_lists.emplace_back(std::move(scene_cmd_list.gfx_queue_cmd_list));
            }

            // load scene
            if (first_load) {
                first_load = false;

                b_new_env_map = true;
                // calculate bounding box
                scene_bounding.min = float3(0.f);
                scene_bounding.max = float3(0.f);

                scene.r().view<ecs::CRenderable, ecs::CTransform>().each(
                    [&](auto entity_id, const auto& c_renderable, const auto& c_transform) {
                        if (c_transform.d_aabb.IsValid()) {
                            scene_bounding.Expand(c_transform.d_aabb);
                        }
                    }
                );

                // new_cell size
                float3 extent = scene_bounding.max - scene_bounding.min;

                float max_extent                = std::max(std::max(extent.x, extent.y), extent.z);
                float cell_size                 = max_extent * 2 / is_ctx.GetGridConfig().grid_size.x;
                ui_config.grid_config.cell_size = cell_size;

                rt_scene = scene.GetGpuSceneRes().rt_scene;

                rt_ctx->SetBindlessHandles(scene.GetGpuSceneRes());
                rt_ctx->SetRaytracingScene(rt_scene);
                rt_ctx->FillLowDiscrepancySequence(cmd_list);

                cmd_list.UpdateBindlessArray(bindless_array);

                if (hooks.on_register_renderer_config_section) {

                    auto& debug_queue = gfx_queue;

                    hooks.on_register_renderer_config_section(
                        "Raytracing",
                        "Material Texture",
                        [&material_textures,
                         &selected_material_texture_name,
                         &b_use_bindless,
                         &b_final_show_texture,
                         &mip_level,
                         &debug_queue](Synapse::Context& ui) {
                            ui.Checkbox("Show Final Texture", &b_final_show_texture);
                            ui.SliderInt("Mip Level", (int*)&mip_level, 0, 12);
                            ui.Checkbox("Use Bindless", &b_use_bindless);
                            if (ui.TreeNode("MaterialTexture")) {
                                for (auto& [name, tex] : material_textures) {
                                    // selectable
                                    const Utf8String ui_name = PlatformToUtf8(name);
                                    if (ui.Selectable(
                                            ui_name.c_str(), selected_material_texture_name == name
                                        )) {
                                        selected_material_texture_name = name;
                                    }
                                }
                                ui.TreePop();
                            }

                            //Pass profiling
                            auto entrys = debug_queue.GetProfilerEntry();
                            if (!entrys.cpu_entries.empty()) {
                                ui.Text("CPU Time:");
                                for (auto& [name, time] : entrys.cpu_entries) {
                                    if (name.ends_with(MOER_TEXT("Percentage"))) {
                                        ui.Text("%s: %.3f%%", name.c_str(), time * 100);

                                    } else
                                        ui.Text("%s: %.3f ms", name.c_str(), time);
                                }
                            }
                            if (!entrys.gpu_entries.empty()) {
                                ui.Text("GPU Time:");
                                for (auto& [name, time] : entrys.gpu_entries) {
                                    ui.Text("%s: %.3f ms", name.c_str(), time);
                                }
                            }
                        }
                    );
                }

                // cmd_list.UpdateRaytracingScene(rt_scene);
                if (!cmd_list.IsEmpty()) {
                    // Keep first-load bootstrap in async submit chain.
                    pre_frame_cmd_lists.emplace_back(std::move(cmd_list));
                    cmd_list = CommandList(EQueueType::Graphics);
                }
            }

            if (b_new_env_map) {
                auto src_env_map = runtime_assets.GetDefaultEnvMap();
                env_map          = device.CreateTexture(
                    src_env_map->GetName(),
                    Extent3D(src_env_map->GetExtent()),
                    PF_R16G16B16A16_SFLOAT,
                    ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                    src_env_map->GetNumMips()
                );
                sd_utils.SampleTextureCS(
                    cmd_list, src_env_map->GetView(0, 1), env_map->GetView(0, 1), src_env_map->GetFormat()
                );

                Sampler sampler{SF_CUBIC, SAM_REPEAT};
                env_tex_with_hdl.tex = env_map;
                env_tex_with_hdl.hdl =
                    bindless_array->AllocateTexture(env_map->GetView(0, env_map->GetNumMips()), sampler);

                for (int i = 0; i < env_map->GetNumMips(); ++i) {
                    env_mips.push_back(env_map->GetView(i));
                }
                sd_utils.GenerateMips(cmd_list, env_mips);
                b_new_env_map = false;

                rt_ctx->LoadDefaultResources(runtime_assets);
                rt_ctx->CreateEnvMapResources(env_tex_with_hdl, cmd_list);
            }

            if (scene.IsReady()) {
                TRACE_SCOPE_CAT("Raytracing.BuildTLAS", "Frame");
                for (size_t i = 0; i < rt_scene->GetInstanceCount(); i++) {
                    auto& instance = rt_scene->GetInstance(i);
                    rt_scene->MarkModified(instance.instance_id);
                }
                cmd_list.UpdateRaytracingScene(rt_scene);
            }

            if (!tone_mapping_pass) {
                ToneMappingPass::CreateInfo info{};
                info.color_lut    = rt_ctx->default_res.black_tex;
                tone_mapping_pass = MakeUnique<ToneMappingPass>(device, manager, scene, info);
            }

            auto& camera = scene.GetMainCamera().camera;

            // prepare frame
            {
                rt_ctx->FillLowDiscrepancySequence(cmd_list);
                camera.Tick(editor_config);
            }

            // update light direction from ui data
            {
                b_export = ui_config.export_cfg.b_export;

                auto light_entity = scene.GetMainDirectionalLightEntity();
                if (light_entity != entt::null && scene.r().valid(light_entity) &&
                    scene.r().all_of<ecs::CLightDirectional, ecs::CTransform>(light_entity)) {
                    auto& c_light_dir     = scene.r().get<ecs::CLightDirectional>(light_entity);
                    c_light_dir.color     = float3(0.9f, 0.65f, 0.4f);
                    c_light_dir.intensity = ui_config.exposure;

                    auto& c_transform    = scene.r().get<ecs::CTransform>(light_entity);
                    c_transform.rotation = Quaternion(float3(0.f, 0.f, -1.f), -ui_config.sun_direction);
                    c_transform.is_dirty = true;
                }
            }

            ToneMappingPass::Params tone_params{};
            AntialiasPass::Params   aa_params{};
            // fill ui data
            {

                TRACE_SCOPE_CAT("Raytracing.Configs", "Frame");

                auto        grid_cfg           = is_ctx.GetGridChangableConfig();
                const auto& ui_cfg             = ui_config;
                grid_cfg.cell_size             = ui_config.grid_config.cell_size;
                grid_cfg.center                = camera.GetPosition();
                auto grid_static_cfg           = is_ctx.GetGridConfig();
                grid_static_cfg.light_per_ceil = ui_config.grid_config.light_per_ceil;
                grid_static_cfg.grid_mode      = ui_config.grid_config.grid_mode;
                is_ctx.SetGridConfig(grid_static_cfg);
                is_ctx.SetChangeableGridConfig(grid_cfg);

                rt_ctx->config.reblur_diffuse_hit_dist_params =
                    *reinterpret_cast<const float4*>(&ui_cfg.denoiser_cfg.hit_dist_params);
                rt_ctx->config.reblur_specular_hit_dist_params =
                    *reinterpret_cast<const float4*>(&ui_cfg.denoiser_cfg.hit_dist_params);
                rt_ctx->config.denoiser_mode = ui_cfg.denoiser_cfg.denoiser_type;

                auto di_initial_sample_config      = is_ctx.GetDIInitialSampleParams();
                auto di_temporal_resampling_config = is_ctx.GetDITemporalResampleParams();
                auto di_spatial_resampling_config  = is_ctx.GetDISpatialResampleParams();
                di_initial_sample_config.local_light_sample_mode =
                    ui_cfg.restir_di_cfg.initial_sample_config.local_light_sample_mode;
                di_initial_sample_config.env_map_is = is_ctx.GetLightBufferParams().env_light.light_cnt;
                di_temporal_resampling_config.bias_correction_mode =
                    ui_cfg.restir_di_cfg.temporal_resample_config.bias_correction;
                di_temporal_resampling_config.depth_threshold =
                    ui_cfg.restir_di_cfg.temporal_resample_config.depth_threshold;
                di_temporal_resampling_config.normal_threshold =
                    ui_cfg.restir_di_cfg.temporal_resample_config.normal_threshold;

                di_spatial_resampling_config.bias_correction_mode =
                    ui_cfg.restir_di_cfg.spatial_resample_config.bias_correction;
                di_spatial_resampling_config.depth_threshold =
                    ui_cfg.restir_di_cfg.spatial_resample_config.depth_threshold;
                di_spatial_resampling_config.normal_threshold =
                    ui_cfg.restir_di_cfg.spatial_resample_config.normal_threshold;
                di_spatial_resampling_config.num_spatial_samples =
                    ui_cfg.restir_di_cfg.spatial_resample_config.num_spatial_samples;

                is_ctx.SetReSTIRDIIInitialSampleParams(di_initial_sample_config);
                is_ctx.SetReSTIRDITemporalResampleParams(di_temporal_resampling_config);
                is_ctx.SetReSTIRDISpatialResampleParams(di_spatial_resampling_config);

                // params.min_adapted_luminance   = 0.002f;
                // params.max_adapted_luminance   = 0.5f;
                // params.exposure_bias           = -1.f;
                // params.eye_adaptation_speed_up = 2.f;
                // params.eye_adaptation_speed_up = 1.f;
                const auto& tone_cfg                  = ui_config.tone_mapping_cfg;
                tone_params.eye_adaptation_speed_down = tone_cfg.eye_adaptation_speed_down;
                tone_params.eye_adaptation_speed_up   = tone_cfg.eye_adaptation_speed_up;
                tone_params.exposure_bias             = tone_cfg.exposure_bias;
                tone_params.max_adapted_luminance     = tone_cfg.max_adapted_luminance;
                tone_params.min_adapted_luminance     = tone_cfg.min_adapted_luminance;
                tone_params.histogram_low_percentile  = tone_cfg.histogram_low_percentile;
                tone_params.histogram_high_percentile = tone_cfg.histogram_high_percentile;
                tone_params.white_point               = tone_cfg.white_point;
                tone_params.enable_tone_mapping       = tone_cfg.enable_tone_mapping;

                const auto& aa_cfg             = ui_config.aa_cfg;
                aa_params.clamping_factor      = aa_cfg.clamping_factor;
                aa_params.new_frame_weight     = aa_cfg.new_frame_weight;
                aa_params.max_radiance         = aa_cfg.max_radiance;
                aa_params.enable_history_clamp = aa_cfg.enable_history_clamping;

                antialias_pass->SetJitter(aa_cfg.jitter_mode);

                rt_ctx->b_parallel_process_light = ui_config.process_light_cfg.parallel_mode;
                rt_ctx->num_threads              = ui_config.process_light_cfg.num_threads;
            }

            is_ctx.TickFrame(time);
            visualize_config.visualize_mode = ui_config.final_color;
            const bool uses_post_process_output =
                b_final_show_texture || ui_config.final_color == Render::EFinalColor::EFC_SceneColor;
            const float2 pixel_jitter =
                uses_post_process_output ? antialias_pass->GetPixelOffset() : float2(0.f);

            uint num_emissive_meshes, num_emissive_triangles;
            prepare_light_pass->CountEmissiveInstances(num_emissive_meshes, num_emissive_triangles);

            static constexpr uint s_mesh_alloc_chunk      = 128;
            static constexpr uint s_triangle_alloc_chunk  = 1024;
            static constexpr uint s_primitive_alloc_chunk = 128;
            uint                  max_primitives          = scene.GetCpuScene().GetPrimitiveCount();
            rt_ctx->CreateBuffersIfNeeded(
                (num_emissive_meshes + s_mesh_alloc_chunk - 1) & ~(s_mesh_alloc_chunk - 1),
                (num_emissive_triangles + s_triangle_alloc_chunk - 1) & ~(s_triangle_alloc_chunk - 1),
                (scene.GetCpuScene().GetLightCount() + s_primitive_alloc_chunk - 1) &
                    ~(s_primitive_alloc_chunk - 1),
                max_primitives
            );
            cmd_list.UpdateBindlessArray(bindless_array);

            rt_ctx->Tick(camera, pixel_jitter);

            RenderGraph                      raytracing_graph;
            const RGRaytracingFrameResources rg_rt =
                ImportRaytracingFrameResources(raytracing_graph, *rt_ctx);
            const bool              current_rt_frame = rt_ctx->b_current_frame;
            const RenderGraphHandle current_view_depth =
                CurrentFrameTexture(current_rt_frame, rg_rt.view_depth, rg_rt.prev_view_depth);
            const RenderGraphHandle current_diffuse_albedo =
                CurrentFrameTexture(current_rt_frame, rg_rt.diffuse_albedo, rg_rt.prev_diffuse_albedo);
            const RenderGraphHandle current_specular_roughness = CurrentFrameTexture(
                current_rt_frame, rg_rt.specular_roughness, rg_rt.prev_specular_roughness
            );
            const RenderGraphHandle current_normal =
                CurrentFrameTexture(current_rt_frame, rg_rt.normal, rg_rt.prev_normal);
            const RenderGraphHandle current_restir_luminance =
                CurrentFrameTexture(current_rt_frame, rg_rt.restir_luminance, rg_rt.prev_luminance);

            {
                auto* params                = raytracing_graph.Alloc<RGRaytracingPrepareLightsParams>();
                params->local_light_pdf_tex = RGTextureView{
                    .handle = rg_rt.local_light_pdf_tex,
                };
                params->primitive_to_light_buf = RGBufferView{
                    .handle = rg_rt.primitive_to_light_buf,
                };
                params->task_buf = RGBufferView{
                    .handle = rg_rt.task_buf,
                };
                params->prim_light_buf = RGBufferView{
                    .handle = rg_rt.prim_light_buf,
                };
                params->light_mapping_buf = RGBufferView{
                    .handle = rg_rt.light_mapping_buf,
                };
                params->light_data_buf = RGBufferView{
                    .handle = rg_rt.light_data_buf,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.PrepareLights"),
                    params,
                    ERGPassFlags::Graphics,
                    [pass    = prepare_light_pass.get(),
                     context = rt_ctx.get()](CommandList& graph_cmd_list, RGContext) {
                        pass->Process(graph_cmd_list, *context);
                    }
                );
            }

            {
                auto* params       = raytracing_graph.Alloc<RGRaytracingGBufferParams>();
                params->view_depth = RGTextureView{
                    .handle = current_view_depth,
                };
                params->diffuse_albedo = RGTextureView{
                    .handle = current_diffuse_albedo,
                };
                params->specular_roughness = RGTextureView{
                    .handle = current_specular_roughness,
                };
                params->normal = RGTextureView{
                    .handle = current_normal,
                };
                params->emission = RGTextureView{
                    .handle = rg_rt.emission,
                };
                params->motion = RGTextureView{
                    .handle = rg_rt.motion,
                };
                params->clip_depth = RGTextureView{
                    .handle = rg_rt.clip_depth,
                };
                params->normal_roughness = RGTextureView{
                    .handle = rg_rt.normal_roughness,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.GBuffer"),
                    params,
                    ERGPassFlags::Graphics,
                    [pass    = g_buffer_pass.get(),
                     context = rt_ctx.get()](CommandList& graph_cmd_list, RGContext) {
                        pass->Process(graph_cmd_list, *context);
                    }
                );
            }

            {
                auto* params       = raytracing_graph.Alloc<RGRaytracingLightingParams>();
                params->view_depth = RGTextureView{
                    .handle = current_view_depth,
                };
                params->normal_roughness = RGTextureView{
                    .handle = rg_rt.normal_roughness,
                };
                params->diffuse_lighting = RGTextureView{
                    .handle = rg_rt.diffuse_lighting,
                };
                params->specular_lighting = RGTextureView{
                    .handle = rg_rt.specular_lighting,
                };
                params->temporal_sample_pos = RGTextureView{
                    .handle = rg_rt.temporal_sample_pos,
                };
                params->gradients = RGTextureView{
                    .handle = rg_rt.gradients,
                };
                params->restir_luminance = RGTextureView{
                    .handle = current_restir_luminance,
                };
                params->prev_diffuse_lighting = RGTextureView{
                    .handle = rg_rt.prev_diffuse_lighting,
                };
                params->light_reservoir_buf = RGBufferView{
                    .handle = rg_rt.light_reservoir_buf,
                };
                params->ris_buf = RGBufferView{
                    .handle = rg_rt.ris_buf,
                };
                params->ris_light_data_buf = RGBufferView{
                    .handle = rg_rt.ris_light_data_buf,
                };
                params->neighbor_offset_buf = RGBufferView{
                    .handle = rg_rt.neighbor_offset_buf,
                };
                params->light_data_buf = RGBufferView{
                    .handle = rg_rt.light_data_buf,
                };
                params->light_mapping_buf = RGBufferView{
                    .handle = rg_rt.light_mapping_buf,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.Lighting"),
                    params,
                    ERGPassFlags::Graphics,
                    [pass    = lighting_pass.get(),
                     context = rt_ctx.get()](CommandList& graph_cmd_list, RGContext) {
                        pass->Process(graph_cmd_list, *context);
                    }
                );
            }

//////////////////////////////////////////////////////////////////////////
// NRD
//////////////////////////////////////////////////////////////////////////
#if WITH_NRD
            {
                auto* params   = raytracing_graph.Alloc<RGRaytracingNrdParams>();
                params->motion = RGTextureView{
                    .handle = rg_rt.motion,
                };
                params->normal_roughness = RGTextureView{
                    .handle = rg_rt.normal_roughness,
                };
                params->view_depth = RGTextureView{
                    .handle = current_view_depth,
                };
                params->diffuse_lighting = RGTextureView{
                    .handle = rg_rt.diffuse_lighting,
                };
                params->specular_lighting = RGTextureView{
                    .handle = rg_rt.specular_lighting,
                };
                params->denoised_diffuse_lighting = RGTextureView{
                    .handle = rg_rt.denoised_diffuse_lighting,
                };
                params->denoised_specular_lighting = RGTextureView{
                    .handle = rg_rt.denoised_specular_lighting,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.NRD"),
                    params,
                    ERGPassFlags::Graphics,
                    [&](CommandList& graph_cmd_list, RGContext) {
                        bool          b_current_frame = rt_ctx->b_current_frame;
                        const auto&   frame_rt        = rt_ctx->frame_rt;
                        nrd::Denoiser denoiser        = nrd::Denoiser::MAX_NUM;
                        switch (ui_config.denoiser_cfg.denoiser_type) {
                            case s_denoiser_mode_reblur:
                                denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
                                break;
                            case s_denoiser_mode_relax:
                                denoiser = nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;
                                break;
                        }

                        if (denoiser == nrd::Denoiser::MAX_NUM) {
                            return;
                        }

                        nrd_interface->Begin();
                        nrd_interface->UpdateCommonSettings(
                            nrd_time++,
                            Vector2ui(resolution.x, resolution.y),
                            pixel_jitter,
                            Transpose(camera.GetViewMatrix()),
                            Transpose(camera.GetProjectionMatrix())
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::MOTION_VECTOR, frame_rt.motion
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::NORMAL_ROUGHNESS, frame_rt.normal_roughness
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::VIEW_Z,
                            b_current_frame ? frame_rt.view_depth : frame_rt.prev_view_depth
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::IN_DIFFUSE, frame_rt.diffuse_lighting
                        );
                        nrd_interface->SetInput(
                            Ext::NRDInterface::EResourceSlot::IN_SPECULAR, frame_rt.specular_lighting
                        );
                        nrd_interface->SetOutput(
                            Ext::NRDInterface::EResourceSlot::OUT_DIFFUSE, frame_rt.denoised_diffuse_lighting
                        );
                        nrd_interface->SetOutput(
                            Ext::NRDInterface::EResourceSlot::OUT_SPECULAR,
                            frame_rt.denoised_specular_lighting
                        );
                        nrd_interface->Denoise(graph_cmd_list, denoiser, MOER_TEXT("Radiance Denoising"));
                    }
                );
            }
#endif

            {
                auto* params       = raytracing_graph.Alloc<RGRaytracingCompositionParams>();
                params->view_depth = RGTextureView{
                    .handle = current_view_depth,
                };
                params->diffuse_albedo = RGTextureView{
                    .handle = current_diffuse_albedo,
                };
                params->specular_roughness = RGTextureView{
                    .handle = current_specular_roughness,
                };
                params->normal = RGTextureView{
                    .handle = current_normal,
                };
                params->emission = RGTextureView{
                    .handle = rg_rt.emission,
                };
                params->diffuse_lighting = RGTextureView{
                    .handle = rg_rt.diffuse_lighting,
                };
                params->specular_lighting = RGTextureView{
                    .handle = rg_rt.specular_lighting,
                };
                params->denoised_diffuse_lighting = RGTextureView{
                    .handle = rg_rt.denoised_diffuse_lighting,
                };
                params->denoised_specular_lighting = RGTextureView{
                    .handle = rg_rt.denoised_specular_lighting,
                };
                params->hdr_color = RGTextureView{
                    .handle = rg_rt.hdr_color,
                };
                params->motion = RGTextureView{
                    .handle = rg_rt.motion,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.Composition"),
                    params,
                    ERGPassFlags::Graphics,
                    [pass    = composition_pass.get(),
                     context = rt_ctx.get()](CommandList& graph_cmd_list, RGContext) {
                        pass->Process(graph_cmd_list, *context);
                    }
                );
            }

            if (uses_post_process_output) {
                auto* aa_rg_params      = raytracing_graph.Alloc<RGRaytracingAntiAliasParams>();
                aa_rg_params->hdr_color = RGTextureView{
                    .handle = rg_rt.hdr_color,
                };
                aa_rg_params->motion = RGTextureView{
                    .handle = rg_rt.motion,
                };
                aa_rg_params->feedback_color_ping = RGTextureView{
                    .handle = rg_rt.feedback_color_ping,
                };
                aa_rg_params->resolved_color = RGTextureView{
                    .handle = rg_rt.resolved_color,
                };
                aa_rg_params->feedback_color_pong = RGTextureView{
                    .handle = rg_rt.feedback_color_pong,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.AntiAlias"),
                    aa_rg_params,
                    ERGPassFlags::Graphics,
                    [pass = antialias_pass.get(), context = rt_ctx.get(), aa_params, b_feedback_valid](
                        CommandList& graph_cmd_list, RGContext
                    ) {
                        pass->Process(
                            graph_cmd_list,
                            *context,
                            aa_params,
                            b_feedback_valid,
                            context->frame_rt.hdr_color,
                            context->frame_rt.resolved_color
                        );
                    }
                );

                auto* tone_rg_params           = raytracing_graph.Alloc<RGRaytracingToneMappingParams>();
                tone_rg_params->resolved_color = RGTextureView{
                    .handle = rg_rt.resolved_color,
                };
                tone_rg_params->ldr_color = RGTextureView{
                    .handle = rg_rt.ldr_color,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.ToneMapping"),
                    tone_rg_params,
                    ERGPassFlags::Graphics,
                    [pass    = tone_mapping_pass.get(),
                     context = rt_ctx.get(),
                     tone_params](CommandList& graph_cmd_list, RGContext) {
                        pass->Process(
                            graph_cmd_list,
                            *context,
                            tone_params,
                            context->frame_rt.resolved_color,
                            context->frame_rt.ldr_color
                        );
                    }
                );
            } else {
                b_feedback_valid = false;
            }

            {
                auto* params      = raytracing_graph.Alloc<RGRaytracingVisualizeParams>();
                params->ldr_color = RGTextureView{
                    .handle = rg_rt.ldr_color,
                };
                params->diffuse_lighting = RGTextureView{
                    .handle = rg_rt.diffuse_lighting,
                };
                params->specular_lighting = RGTextureView{
                    .handle = rg_rt.specular_lighting,
                };
                params->view_depth = RGTextureView{
                    .handle = current_view_depth,
                };
                params->emission = RGTextureView{
                    .handle = rg_rt.emission,
                };
                params->debug_color = RGTextureView{
                    .handle = rg_rt.debug_color,
                };
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.Visualize"),
                    params,
                    ERGPassFlags::Graphics,
                    [pass    = visualize_pass.get(),
                     context = rt_ctx.get(),
                     visualize_config,
                     bdls = bindless_array](CommandList& graph_cmd_list, RGContext) {
                        pass->Process(graph_cmd_list, *context, visualize_config, bdls);
                    }
                );
            }

            if (b_final_show_texture && material_textures.contains(selected_material_texture_name)) {
                TextureRef        texture          = material_textures[selected_material_texture_name].tex;
                RenderGraphHandle selected_texture = ImportTextureIfValid(
                    raytracing_graph, MOER_TEXT("RT.selected_material_texture"), texture
                );
                auto* params             = raytracing_graph.Alloc<RGRaytracingShowTextureParams>();
                params->selected_texture = RGTextureView{
                    .handle = selected_texture,
                };
                params->ldr_color = RGTextureView{
                    .handle = rg_rt.ldr_color,
                };
                const uint selected_texture_handle = material_textures[selected_material_texture_name].hdl;
                ShowTextureParams show_texture_params{};
                show_texture_params.dst_dim      = resolution;
                show_texture_params.bdls_handle  = selected_texture_handle;
                show_texture_params.mip_level    = mip_level;
                show_texture_params.use_bindless = b_use_bindless;
                raytracing_graph.AddPass(
                    MOER_TEXT("RT.ShowTexture"),
                    params,
                    ERGPassFlags::Graphics,
                    [&, show_texture_params, texture](CommandList& graph_cmd_list, RGContext) {
                        sd_utils.ShowTexture(
                            graph_cmd_list,
                            bindless_array,
                            show_texture_params,
                            texture,
                            rt_ctx->frame_rt.ldr_color
                        );
                    }
                );
            }
            raytracing_graph.Dispatch(&cmd_list);
            // copy normal to output
            //  cmd_list.CopyFrom(out_direct_lighting->GetView(),
            //  scene_color->GetView());
            rt_ctx->AdvanceFrame();
            if (uses_post_process_output) {
                tone_mapping_pass->AdvanceFrame(camera.GetDeltaTime());
                antialias_pass->AdvanceFrame();
                b_feedback_valid = true;
            }

            //////////////////////////////////////////////////////////////////////////
            // handle export
            //////////////////////////////////////////////////////////////////////////

            if (b_export) {
                FenceRef export_fence = device.CreateFence();
                cmd_list.TickProfiling().Signal(export_fence, 1);
                Array<CommandList> export_cmd_lists{};
                export_cmd_lists.emplace_back(std::move(cmd_list));
                RHIExecutor::Get().Submit(std::move(export_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
                cmd_list = CommandList(EQueueType::Graphics);
                export_fence->Wait(1);
                gfx_queue.Sync();
                DumpTextureToFile(
                    ui_config.export_cfg,
                    rt_ctx->frame_rt,
                    device,
                    gfx_queue,
                    exported_file_path,
                    Printf(MOER_TEXT("{}"), time)
                );
                b_export = false;
            }
        }

        const bool ready_for_history_trace =
            scene.IsReady() && runtime_assets.IsReady() && !first_load && !b_new_env_map && b_feedback_valid;
        if (ready_for_history_trace && !b_trace_capture_started) {
            if (!b_trace_capture_armed) {
                b_trace_capture_armed = true;
            } else {
                ResetRHITraceFrameIndex();
                b_trace_capture_started = true;
            }
        } else if (!ready_for_history_trace) {
            b_trace_capture_armed = false;
        }
        SetRHITraceRuntimeEnabled(b_trace_capture_started);

        // rt_scene->MarkModified(0);
        // cmd_list.UpdateRaytracingScene(rt_scene);
        TextureRef final_color =
            b_final_show_texture ? rt_ctx->frame_rt.ldr_color : rt_ctx->frame_rt.debug_color;
        TextureRef present_output = final_color;

        if (!editor_config->play_mode_enabled) {
            if (hooks.on_publish_scene_output) {
                hooks.on_publish_scene_output(final_color);
            }
            if (hooks.on_render_gui) {
                cmd_list.ClearResource(combine_output->GetView(), float4(0.f, 0.f, 0.f, 1.f));
                hooks.on_render_gui(cmd_list, combine_output);
                present_output = combine_output;
            }
        }

        if (rt_scene) {
            rt_scene->AdvanceFrame();
        }

        const bool should_close_now = WindowContext::ShouldClose(WindowContext::GetMainWindow());
        if (should_close_now) {
            // Avoid extra present work in the closing frame to reduce shutdown deadlock risk.
            skip_present = true;
        }

        time++;
        RHIPresentRequest present_request = presentation_surface->CreatePresentRequest(present_output);
        cmd_list.Signal(timeline, time).DeleteResources().TickProfiling().TickFrame();
        Array<CommandList> frame_cmd_lists = std::move(pre_frame_cmd_lists);
        frame_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(
            std::move(frame_cmd_lists),
            ERHIExecSubmitFlags::FlushGPU,
            skip_present ? nullptr : &present_request
        );
        cmd_list = CommandList(EQueueType::Graphics);

        if (!skip_present && !editor_config->play_mode_enabled && hooks.on_present_windows) {
            hooks.on_present_windows();
        }
        if (should_close_now) {
            break;
        }
        if (hooks.on_is_need_reload && hooks.on_is_need_reload()) {
            break;
        }
    }

    if (time > 0) {
        timeline->Wait(time);
        gfx_queue.Sync();
    }

    SetRHITraceRuntimeEnabled(true);

    const auto& allocated_buf = rt_ctx->GetAllocatedBdlsBuf();
    for (auto& buf : allocated_buf) {
        bindless_array->UnbindBuffer(buf);
    }

    const auto& allocated_tex = rt_ctx->GetAllocatedBdlsTex();
    for (auto& tex : allocated_tex) {
        bindless_array->UnbindTexture(tex);
    }

    for (auto& callback : on_free_buffer_callbacks) {
        callback(0);
    }

    ReleaseResources();

    if (hooks.on_unregister_renderer_config_section) {
        hooks.on_unregister_renderer_config_section("Raytracing", "Settings");
        hooks.on_unregister_renderer_config_section("Raytracing", "Material Texture");
    }
}

void RaytracingRenderer::DumpTextureToFile(
    ExportConfig&          _config,
    FrameResources&        _frame_rt,
    RenderDevice&          _device,
    CommandQueue&          _gfx_queue,
    std::filesystem::path& _exported_file_path,
    StringView             _suffix
) {

    CommandList cmd_list{};

    auto dequantentize_half = [](short _h, bool _gamma_correct = true) {
        unsigned int s  = unsigned(_h & 0x8000) << 16;
        int          em = _h & 0x7fff;

        // bias exponent and pad mantissa with 0; 112 is relative exponent
        // bias (127-15)
        int r = (em + (112 << 10)) << 13;

        // denormal: flush to zero
        r = (em < (1 << 10)) ? 0 : r;

        // infinity/NaN; note that we preserve NaN payload as a byproduct of
        // unifying inf/nan cases 112 is an exponent bias fixup; since we
        // already applied it once, applying it twice converts 31 to 255
        r += (em >= (31 << 10)) ? (112 << 23) : 0;

        FloatBits u;
        u.ui = s | r;
        if (_gamma_correct) {
            u.f = u.f <= 0.0031308f ? 12.92f * u.f : 1.055f * std::pow(u.f, 1.f / 2.4f) - 0.055f;
        }
        return u.f;
    };

    auto dequantentize_byte_to_srgb = [](unsigned char _b) {
        float c = _b / 255.f;
        c       = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;

        // to byte
        return (unsigned char)(c * 255.f);
    };

    size_t            size;
    Array<Moer::byte> copy_back_data;
    String            file_name = MOER_TEXT("screenshot_");
    bool              hdr       = false;

    uint3 resolution = _frame_rt.ldr_color->GetExtent();
    switch (_config.output_texture) {
        case EOT_LDR: {
            size = sizeof(uint) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            cmd_list.CopyFrom(_frame_rt.ldr_color->GetView(), copy_back_data);
            file_name += _suffix;
            file_name += MOER_TEXT(".png");
            break;
        }
        case EOT_HDR: {
            size = sizeof(float2) * resolution.x * resolution.y;
            copy_back_data.resize(size);
            cmd_list.CopyFrom(_frame_rt.resolved_color->GetView(), copy_back_data);
            file_name += _suffix;
            file_name += MOER_TEXT(".exr");
            hdr = true;
            break;
        }
        default:
            size = 0;
    }
    if (size != 0) {
        FenceRef export_fence = _device.CreateFence();
        cmd_list.TickProfiling().Signal(export_fence, 1);
        Array<CommandList> export_cmd_lists{};
        export_cmd_lists.emplace_back(std::move(cmd_list));
        RHIExecutor::Get().Submit(std::move(export_cmd_lists), ERHIExecSubmitFlags::FlushGPU);
        export_fence->Wait(1);
        _gfx_queue.Sync();
        if (hdr) {
            // export to exr
            // cast r16g16b16a16_sfloat to r32g32b32a32_sfloat
            assert(
                _frame_rt.resolved_color->GetFormat() == PF_R16G16B16A16_SFLOAT &&
                "resolved color format must be r16g16b16a16_sfloat"
            );
        } else {
            // ldr format must be r8g8b8a8_unorm
            assert(
                _frame_rt.ldr_color->GetFormat() == PF_R8G8B8A8_UNORM && "ldr format must be r8g8b8a8_unorm"
            );
        }
        _config.b_export = false;
        // cast r8g8b8a8_unorm to srgb
        LambdaTask::Create([=,
                            r_copy_back_data(std::move(copy_back_data)),
                            dequantentize_byte_to_srgb(std::move(dequantentize_byte_to_srgb)),
                            dequantentize_half(std::move(dequantentize_half)),
                            &_config]() {
            // free copy back data
            auto copy_back_data = std::move(r_copy_back_data);
            if (hdr) {
                uint          range_cnt = 8;
                uint          range     = copy_back_data.size() / range_cnt;
                Array<float4> copy_back_data_f4(copy_back_data.size() / 8);
                ParallelFor(range_cnt, [&](uint _idx) {
                    size_t start = range * _idx;
                    size_t end   = range * (_idx + 1);
                    for (size_t i = start; i < copy_back_data.size() && i < end; i += 8) {
                        float4* data   = &copy_back_data_f4[i / 8];
                        short*  data_s = (short*)&copy_back_data[i];
                        data->x        = dequantentize_half(data_s[0]);
                        data->y        = dequantentize_half(data_s[1]);
                        data->z        = dequantentize_half(data_s[2]);
                        data->w        = dequantentize_half(data_s[3]);
                    }
                });
                const std::filesystem::path output_path =
                    _exported_file_path / std::filesystem::path(file_name.c_str());
                const auto       native_path = output_path.native();
                const Utf8String output_path_utf8 =
                    PlatformToUtf8(StringView(native_path.data(), native_path.size()));
                stbi_write_hdr(
                    output_path_utf8.c_str(), resolution.x, resolution.y, 4, (float*)copy_back_data_f4.data()
                );
            } else {
                // parallel to 4 threads
                uint range_cnt = 8;
                uint range     = copy_back_data.size() / range_cnt;
                ParallelFor(range_cnt, [&](uint _idx) {
                    size_t start = range * _idx;
                    size_t end   = range * (_idx + 1);
                    for (size_t i = start; i < copy_back_data.size() && i < end; i += 4) {
                        copy_back_data[i] = (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i]));
                        copy_back_data[i + 1] =
                            (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 1]));
                        copy_back_data[i + 2] =
                            (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 2]));
                        copy_back_data[i + 3] =
                            (Moer::byte)dequantentize_byte_to_srgb(ubyte(copy_back_data[i + 3]));
                    }
                });
                const std::filesystem::path output_path =
                    _exported_file_path / std::filesystem::path(file_name.c_str());
                const auto       native_path = output_path.native();
                const Utf8String output_path_utf8 =
                    PlatformToUtf8(StringView(native_path.data(), native_path.size()));
                stbi_write_png(
                    output_path_utf8.c_str(),
                    resolution.x,
                    resolution.y,
                    4,
                    (void*)copy_back_data.data(),
                    4 * resolution.x
                );
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }).Dispatch();
    }
}

} // namespace Moer::Render::Raytracing
