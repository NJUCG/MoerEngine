#include "GaussianShadowPass.h"
#include "RasterConfig.h"
#include "math/Function.h"
#include "rhi/RHI.h"
namespace Moer::Render::Raster {
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
    for (uint i = 0; i < 4; ++i)
        if (m_maps[i])
            m_bindless->UnbindTexture(m_handles[i]);
}
GaussianShadowPass::RecordParameters GaussianShadowPass::Prepare(
    RasterContext&          context,
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
    const uint mask    = editor_view ? 2u : 1u;
    for (uint i = 0; i < scene.GetInstances().size(); ++i) {
        const auto& instance = scene.GetInstances()[i];
        if ((instance.range.w & mask) && (instance.range.w & 4u) && instance.range.y) {
            result.draws.push_back(instance);
            result.instance_indices.push_back(i);
        }
    }
    const bool cube = config.shadow_map_mode == EShadowMapMode::POINT_CUBE;
    const bool directional =
        config.shadow_map_mode == EShadowMapMode::CSM || config.shadow_map_mode == EShadowMapMode::CSM_AUTO;
    const auto& updates = context.GetSceneUpdates();
    bool        valid   = (cube && updates.main_point_light.has_value()) ||
                          (directional && updates.main_directional_light.has_value());
    data.config         = uint4(
        valid ? (cube ? 2u : 1u) : 0u,
        cube ? 1u : uint(config.shadow_csm_num_of_cascades),
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
        for (uint face = 0; face < 6; ++face)
            data.world_to_clip[face] =
                projection *
                MakeLookatViewMatrixRH(source.light_pos, source.light_pos + directions[face], ups[face]);
    } else if (valid) {
        data.light = float4(-context.lighting_data.main_light_direction, 0);
        for (uint i = 0; i < data.config.y; ++i) {
            data.world_to_clip[i] = context.lighting_data.world2shadow_clip[i];
            data.mesh_maps[i]     = context.csm_data.shadow_map_textures[i].hdl;
        }
    }
    if (data.config.w)
        for (uint i = 0; i < data.config.y; ++i) {
            if (!m_maps[i] || m_maps[i]->GetWidth() != data.config.z || m_cube != cube) {
                if (m_maps[i])
                    m_bindless->UnbindTexture(m_handles[i]);
                auto usage = ETextureUsageFlags::SAMPLED | ETextureUsageFlags::DEPTH_STENCIL_ATTACHMENT |
                             ETextureUsageFlags::TRANSFER_SRC;
                m_maps[i]  = cube ? TextureRef(context.device.CreateCubeMap(
                                        "Gaussian point shadow",
                                        Extent2D(data.config.z, data.config.z),
                                        context.textures.depth_linear_sampler.tex->GetFormat(),
                                        usage
                                    )) :
                                    context.device.CreateTexture(
                                        "Gaussian cascade shadow",
                                        Extent3D(data.config.z, data.config.z, 1),
                                        context.textures.depth_linear_sampler.tex->GetFormat(),
                                        usage
                                    );
                m_handles[i] =
                    m_bindless->AllocateTexture(m_maps[i]->GetView(), Sampler(SF_NEAREST, SAM_CLAMP_TO_EDGE));
            }
            data.gaussian_maps[i] = m_handles[i];
            result.maps[i]        = m_maps[i];
        }
    m_cube                                = cube;
    context.lighting_data.gaussian_shadow = uint4(m_data_handle, data.config.w, 0, 0);
    return result;
}
void GaussianShadowPass::Record(CommandList& commands, const RecordParameters& parameters) {
    commands.CopyFrom(
        std::span<const byte>(reinterpret_cast<const byte*>(&parameters.data), sizeof(parameters.data)),
        parameters.data_buffer->GetView(),
        "Gaussian shadow views"
    );
    commands.UpdateBindlessArray(parameters.bindless);
    if (parameters.data.config.w) {
        commands.PushScopeWithTimeScope("Gaussian ShadowDepth");
        uint count = parameters.data.config.x == 2 ? 6u : parameters.data.config.y;
        for (uint view = 0; view < count; ++view)
            for (uint draw = 0; draw < parameters.draws.size(); ++draw) {
                const auto&        instance = parameters.draws[draw];
                GaussianShadowDraw param{};
                param.world_to_clip = parameters.data.world_to_clip[view];
                param.info =
                    uint4(instance.range.x, parameters.instance_indices[draw], parameters.data.config.z, 0);
                param.options = parameters.data.options;
                auto texture  = parameters.maps[parameters.data.config.x == 2 ? 0 : view];
                auto depth    = DepthAttachment(
                    parameters.data.config.x == 2 ? texture->GetView().Slice(view, 1) : texture->GetView()
                );
                depth.action =
                    draw == 0 ? EAttachmentAction::AC_DS_CLEAR_STORE : EAttachmentAction::AC_DS_LOAD_STORE;
                Array<SingleDrawParam> draws;
                draws.emplace_back(SingleDrawParam{6, instance.range.y, 0, 0, 0});
                commands
                    .Gfx(m_pipeline, param, parameters.gaussians->GetView(), parameters.instances->GetView())
                    .Draw(
                        "Gaussian shadow",
                        Rect2D(0, 0, parameters.data.config.z, parameters.data.config.z),
                        std::move(draws),
                        depth
                    );
            }
        commands.PopScopeWithTimeScope();
    }
    commands.AddCallback([parameters]() {});
}
TextureRef GaussianShadowPass::RecordDepthDebug(CommandList& commands, const RecordParameters& parameters) {
    const uint size  = parameters.data.config.z;
    const uint width = size * (parameters.data.config.x == 2 ? 3 : 2), height = size * 2;
    if (!m_debug_depth || m_debug_depth->GetWidth() != width || m_debug_depth->GetHeight() != height)
        m_debug_depth = RenderDevice::Get().CreateTexture(
            "Gaussian shadow depth atlas",
            Extent3D(width, height, 1),
            PF_R32G32B32A32_SFLOAT,
            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS |
                ETextureUsageFlags::TRANSFER_SRC
        );
    auto uniform = RenderDevice::Get().CreateBuffer<byte>(
        "Gaussian shadow debug views",
        sizeof(GaussianShadowData),
        EBufferUsageFlags::CONSTANT_BUFFER | EBufferUsageFlags::TRANSFER_DST
    );
    commands.CopyFrom(
        std::span<const byte>(reinterpret_cast<const byte*>(&parameters.data), sizeof(parameters.data)),
        uniform->GetView()
    );
    commands.UpdateBindlessArray(parameters.bindless);
    commands.Compute(m_inspect, uniform->GetView(), m_debug_depth->GetView(), parameters.bindless)
        .Dispatch(uint3((width + 7) / 8, (height + 7) / 8, 1), "Gaussian shadow depth atlas");
    auto output = m_debug_depth;
    commands.AddCallback([parameters, uniform, output]() {});
    return output;
}
} // namespace Moer::Render::Raster
