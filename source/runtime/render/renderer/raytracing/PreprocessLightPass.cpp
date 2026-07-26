#include "PreprocessLightPass.h"

#include "RaytracingGraphResources.h"
#include "rhi/RHICommand.h"
#include "shader/ShaderResourceManager.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <stdexcept>
#include <string_view>

namespace Moer::Render::Raytracing {

namespace {

uint FloatToUInt(float value, float scale) {
    return static_cast<uint>(std::floor(value * scale + 0.5f));
}

float Saturate(float value) {
    return std::clamp(value, 0.f, 1.f);
}

uint Float3ToR8G8B8Unorm(const float3& value) {
    return (FloatToUInt(Saturate(value.x), 255.f) & 0xffu) |
           ((FloatToUInt(Saturate(value.y), 255.f) & 0xffu) << 8) |
           ((FloatToUInt(Saturate(value.z), 255.f) & 0xffu) << 16);
}

uint16_t Fp32ToFp16(float value) {
    static const union FloatBits {
        uint  ui;
        float f;
    } multiplier = {0x07800000};

    FloatBits biased_float{};
    biased_float.f  = value * multiplier.f;
    const uint bits = biased_float.ui;

    const uint sign = bits & 0x80000000;
    const uint body = bits & 0x0fffffff;
    return static_cast<uint16_t>((sign >> 16 | body >> 13) & 0xffffu);
}

void PackPolyLightColor(const float3& color, PolymorphicLightInfo& info) {
    const float max_radiance = Max(Max(color.x, color.y), color.z);
    if (max_radiance <= 0.f) {
        return;
    }

    const float log_radiance = std::clamp(
        (std::log2f(max_radiance) - g_poly_morphic_light_min_log2_radiance) /
            (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance),
        0.f,
        1.f
    );
    const uint  packed_radiance = std::min(static_cast<uint>(std::ceil(log_radiance * 65534.f)) + 1, 0xffffu);
    const float unpacked_radiance = std::exp2f(
        (static_cast<float>(packed_radiance - 1) / 65534.f) *
            (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance) +
        g_poly_morphic_light_min_log2_radiance
    );

    info.color_type_flags |= Float3ToR8G8B8Unorm(color / unpacked_radiance);
    info.log_radiance = packed_radiance;
}

constexpr uint RoundUpCapacity(uint count, uint chunk_size) {
    if (count == 0) {
        return 0;
    }
    return (count + chunk_size - 1) & ~(chunk_size - 1);
}

static_assert(RoundUpCapacity(0, 128) == 0);
static_assert(RoundUpCapacity(1, 128) == 128);
static_assert(RoundUpCapacity(127, 128) == 128);
static_assert(RoundUpCapacity(128, 128) == 128);
static_assert(RoundUpCapacity(129, 128) == 256);
static_assert(RoundUpCapacity(1023, 1024) == 1024);
static_assert(RoundUpCapacity(1025, 1024) == 2048);

RenderGraph::TextureState PreferredReadState(const TextureRef& texture) {
    const auto usage = texture->GetUsage();
    const bool supports_uav =
        (usage & ETextureUsageFlags::UNORDERED_ACCESS) == ETextureUsageFlags::UNORDERED_ACCESS;
    return supports_uav ? RenderGraph::TextureState::ShaderResource : RenderGraph::TextureState::Sampled;
}

void RequireBufferCapacity(const BufferRef& buffer, size_t required_elements, std::string_view name) {
    if (!buffer || buffer->GetNumElement() < required_elements) {
        throw std::runtime_error(std::format(
            "PrepareLights buffer '{}' capacity is {}, but {} elements are required",
            name,
            buffer ? buffer->GetNumElement() : 0,
            required_elements
        ));
    }
}

} // namespace

PrepareLightPass::PrepareLightPass(ShaderManager& manager, BindlessArrayRef bindless_array) :
    bindless_array(std::move(bindless_array)),
    prepare_light_pipeline(manager.Compute<PrepareLightShaderPipeline>(
        "pipelines/raytracing/lighting/precompute/PrepareLights.hlsl"
    )) {}

PrepareLightPass::RecordResources PrepareLightPass::CaptureResources(const RTContext& rt_ctx) const {
    const RaytracingBindlessResources& scene = rt_ctx.GetBindlessResources();
    return RecordResources{
        .primitive_to_light_buf = rt_ctx.primitive_to_light_buf,
        .task_buf               = rt_ctx.task_buf,
        .prim_light_buf         = rt_ctx.prim_light_buf,
        .light_mapping_buf      = rt_ctx.light_mapping_buf,
        .light_data_buf         = rt_ctx.light_data_buf,
        .primitive_buf          = scene.primitive_buf,
        .instance_buf           = scene.instance_buf,
        .material_buf           = scene.material_buf,
        .position_buf           = scene.position_buf,
        .index_buf              = scene.index_buf,
        .texcoord0_buf          = scene.texcoord0_buf,
        .local_light_pdf_tex    = rt_ctx.local_light_pdf_tex,
        .local_light_pdf_mips   = rt_ctx.local_light_pdf_mips,
        .material_textures      = scene.material_textures,
        .bindless_array         = bindless_array,
        .shader_utils           = &rt_ctx.sd_utils
    };
}

PrepareLightPass::PreparedCommand PrepareLightPass::Prepare(
    RTContext&                          rt_ctx,
    const RaytracingSceneFrameSnapshot& scene_snapshot,
    uint64                              scene_revision
) {
    static constexpr uint s_mesh_alloc_chunk      = 128;
    static constexpr uint s_triangle_alloc_chunk  = 1024;
    static constexpr uint s_primitive_alloc_chunk = 128;

    uint emissive_mesh_count     = 0;
    uint emissive_triangle_count = 0;
    uint primitive_count         = std::max(scene_snapshot.primitive_count, 1u);
    for (const auto& primitive : scene_snapshot.emissive_primitives) {
        if (primitive.num_triangles == 0) {
            continue;
        }
        ++emissive_mesh_count;
        emissive_triangle_count += primitive.num_triangles;
        primitive_count = std::max(primitive_count, primitive.primitive_id + 1);
    }

    const bool has_environment_light = rt_ctx.scene_params.enable_env_map && rt_ctx.env_pdf_tex;
    const uint primitive_light_count =
        static_cast<uint>(scene_snapshot.analytic_lights.size()) + static_cast<uint>(has_environment_light);
    rt_ctx.CreateBuffersIfNeeded(
        RoundUpCapacity(emissive_mesh_count, s_mesh_alloc_chunk),
        RoundUpCapacity(emissive_triangle_count, s_triangle_alloc_chunk),
        RoundUpCapacity(primitive_light_count, s_primitive_alloc_chunk),
        primitive_count
    );

    auto payload                  = MakeShared<RecordPayload>();
    payload->resources            = CaptureResources(rt_ctx);
    payload->reads_scene_geometry = emissive_mesh_count != 0;

    PreparedCommand command{};
    command.next_primitive_to_light_identity = payload->resources.primitive_to_light_buf.Get();
    command.next_tasks_identity              = payload->resources.task_buf.Get();
    command.next_primitive_lights_identity   = payload->resources.prim_light_buf.Get();
    command.next_light_mapping_identity      = payload->resources.light_mapping_buf.Get();
    command.next_light_data_identity         = payload->resources.light_data_buf.Get();
    command.next_local_light_pdf_identity    = payload->resources.local_light_pdf_tex.Get();
    command.next_scene_revision              = scene_revision;

    payload->primitive_to_light_initialized =
        accepted_primitive_to_light_identity == command.next_primitive_to_light_identity;
    payload->tasks_initialized = accepted_tasks_identity == command.next_tasks_identity;
    payload->primitive_lights_initialized =
        accepted_primitive_lights_identity == command.next_primitive_lights_identity;
    payload->light_mapping_initialized =
        accepted_light_mapping_identity == command.next_light_mapping_identity;
    payload->light_data_initialized = accepted_light_data_identity == command.next_light_data_identity;
    payload->local_light_pdf_initialized =
        accepted_local_light_pdf_identity == command.next_local_light_pdf_identity;
    payload->reset_light_history = !payload->light_mapping_initialized || !payload->light_data_initialized ||
                                   accepted_scene_revision != scene_revision;

    const UnorderedMap<uint64, EmissiveLightHistory> empty_instance_offsets{};
    const UnorderedMap<uint64, uint>                 empty_primitive_offsets{};
    const auto&                                      previous_instance_offsets =
        payload->reset_light_history ? empty_instance_offsets : instance_light_buffer_offsets;
    const auto& previous_primitive_offsets =
        payload->reset_light_history ? empty_primitive_offsets : primitive_light_buffer_offsets;
    const bool current_odd_frame = payload->reset_light_history ? false : b_odd_frame;

    payload->primitive_to_light = Array<uint>(primitive_count, s_invalid_light_idx);

    uint light_buffer_offset          = 0;
    uint num_emissive_triangle_lights = 0;
    for (const auto& primitive : scene_snapshot.emissive_primitives) {
        if (primitive.num_triangles == 0) {
            continue;
        }
        const auto previous = previous_instance_offsets.find(primitive.stable_key);
        const bool previous_is_compatible =
            previous != previous_instance_offsets.end() &&
            previous->second.num_triangles == primitive.num_triangles &&
            previous->second.index_start_idx == primitive.index_start_idx &&
            previous->second.first_instance_idx == primitive.first_instance_idx;

        PrepareLightsTask task{};
        task.primitive_id       = primitive.primitive_id;
        task.light_offset       = light_buffer_offset;
        task.num_triangles      = primitive.num_triangles;
        task.prev_light_offset  = previous_is_compatible ? previous->second.light_offset : -1;
        task.index_start_idx    = primitive.index_start_idx;
        task.first_instance_idx = primitive.first_instance_idx;

        if (primitive.primitive_id >= payload->primitive_to_light.size()) {
            payload->primitive_to_light.resize(primitive.primitive_id + 1, s_invalid_light_idx);
        }
        payload->primitive_to_light[primitive.primitive_id]              = light_buffer_offset;
        command.next_instance_light_buffer_offsets[primitive.stable_key] = EmissiveLightHistory{
            .light_offset       = light_buffer_offset,
            .num_triangles      = primitive.num_triangles,
            .index_start_idx    = primitive.index_start_idx,
            .first_instance_idx = primitive.first_instance_idx
        };
        light_buffer_offset += task.num_triangles;
        num_emissive_triangle_lights += task.num_triangles;
        payload->tasks.emplace_back(task);
    }

    uint num_finite_primitive_lights   = 0;
    uint num_infinite_primitive_lights = 0;
    uint num_environment_lights        = 0;

    for (const auto& light : scene_snapshot.analytic_lights) {
        const auto previous = previous_primitive_offsets.find(light.stable_key);

        PrepareLightsTask task{};
        task.primitive_id  = static_cast<uint>(payload->primitive_light_infos.size()) | g_task_prim_light_bit;
        task.light_offset  = light_buffer_offset;
        task.num_triangles = 1;
        task.prev_light_offset = previous == previous_primitive_offsets.end() ? -1 : previous->second;

        command.next_primitive_light_buffer_offsets[light.stable_key] = light_buffer_offset;
        ++light_buffer_offset;
        payload->tasks.emplace_back(task);
        payload->primitive_light_infos.emplace_back(light.light_info);

        switch (light.light_class) {
            case EAnalyticLightClass::Finite:
                ++num_finite_primitive_lights;
                break;
            case EAnalyticLightClass::Infinite:
                ++num_infinite_primitive_lights;
                break;
            case EAnalyticLightClass::Environment:
                ++num_environment_lights;
                break;
        }
    }

    if (has_environment_light) {
        PolymorphicLightInfo light_info{};
        light_info.color_type_flags = static_cast<uint>(EPolyLightType::ELEnv)
                                      << g_poly_morphic_light_type_shift;

        PackPolyLightColor(float3(rt_ctx.scene_params.env_map_scale), light_info);
        light_info.direction1  = rt_ctx.scene_params.env_map_handle;
        const uint3 env_extent = rt_ctx.env_pdf_tex->GetExtent();
        light_info.direction2  = env_extent.x | (env_extent.y << 16);
        light_info.scalars     = Fp32ToFp16(rt_ctx.scene_params.env_map_rotation);
        light_info.scalars |= g_poly_morphic_light_env_is_scalar_bit;

        constexpr uint64 environment_light_key = ~0ull;
        const auto       previous              = previous_primitive_offsets.find(environment_light_key);

        PrepareLightsTask task{};
        task.primitive_id  = static_cast<uint>(payload->primitive_light_infos.size()) | g_task_prim_light_bit;
        task.light_offset  = light_buffer_offset;
        task.num_triangles = 1;
        task.prev_light_offset = previous == previous_primitive_offsets.end() ? -1 : previous->second;

        command.next_primitive_light_buffer_offsets[environment_light_key] = light_buffer_offset;
        ++light_buffer_offset;
        payload->tasks.emplace_back(task);
        payload->primitive_light_infos.emplace_back(light_info);
        ++num_environment_lights;
    }

    payload->params.num_tasks          = static_cast<uint>(payload->tasks.size());
    payload->params.primitive_buf_hdl  = rt_ctx.GetBindlessHandles().primitive_buf_hdl;
    payload->params.instance_buf_hdl   = rt_ctx.GetBindlessHandles().instance_buf_hdl;
    payload->params.material_buf_hdl   = rt_ctx.GetBindlessHandles().material_buf_hdl;
    payload->params.position_buf_hdl   = rt_ctx.GetBindlessHandles().position_buf_hdl;
    payload->params.index_buf_hdl      = rt_ctx.GetBindlessHandles().index_buf_hdl;
    payload->params.texcoord0_buf_hdl  = rt_ctx.GetBindlessHandles().texcoord0_buf_hdl;
    payload->params.primitive_to_light = rt_ctx.GetBindlessHandles().primitive_to_light;

    const uint max_lights_in_buffer   = static_cast<uint>(rt_ctx.light_data_buf->GetNumElement() / 2);
    payload->params.cur_light_offset  = max_lights_in_buffer * static_cast<uint>(current_odd_frame);
    payload->params.prev_light_offset = max_lights_in_buffer * static_cast<uint>(!current_odd_frame);
    payload->light_buffer_params      = ImportanceSamplingContext::BuildLightBufferParams(
        payload->params.cur_light_offset,
        num_finite_primitive_lights + num_emissive_triangle_lights,
        num_infinite_primitive_lights,
        num_environment_lights
    );
    payload->dispatch_light_count = light_buffer_offset;

    RequireBufferCapacity(
        payload->resources.primitive_to_light_buf, payload->primitive_to_light.size(), "primitive_to_light"
    );
    RequireBufferCapacity(payload->resources.task_buf, payload->tasks.size(), "tasks");
    RequireBufferCapacity(
        payload->resources.prim_light_buf, payload->primitive_light_infos.size(), "primitive_lights"
    );
    RequireBufferCapacity(
        payload->resources.light_mapping_buf,
        static_cast<size_t>(payload->dispatch_light_count) * 2,
        "light_mapping"
    );
    RequireBufferCapacity(
        payload->resources.light_data_buf,
        static_cast<size_t>(payload->dispatch_light_count) * 2,
        "light_data"
    );

    constexpr uint max_destination_mips_per_dispatch = 5;
    const uint     mip_count = static_cast<uint>(payload->resources.local_light_pdf_mips.size());
    for (uint source_mip = 0; source_mip + 1 < mip_count; source_mip += max_destination_mips_per_dispatch) {
        payload->mip_dispatches.emplace_back(MipDispatch{
            .source_mip      = source_mip,
            .bound_mip_count = std::min(max_destination_mips_per_dispatch + 1, mip_count - source_mip)
        });
    }

    command.next_odd_frame = !current_odd_frame;
    command.record         = std::move(payload);
    return command;
}

void PrepareLightPass::RecordUploads(CommandList& cmd_list, SharedPtr<const RecordPayload> payload) {
    if (!payload->primitive_to_light.empty()) {
        cmd_list.CopyFrom(
            std::span<byte>(
                reinterpret_cast<byte*>(const_cast<uint*>(payload->primitive_to_light.data())),
                payload->primitive_to_light.size() * sizeof(uint)
            ),
            payload->resources.primitive_to_light_buf->GetView(),
            "Upload primitive_to_light"
        );
    }
    if (payload->tasks.empty() && !payload->tasks_initialized) {
        // A newly allocated empty task buffer still needs a concrete contents
        // and state boundary before it can be exported as an SRV. This keeps
        // empty-scene graph compilation complete without inventing an
        // external initial state for the new allocation.
        cmd_list.ClearResource(payload->resources.task_buf->GetView(), 0u);
    } else if (!payload->tasks.empty()) {
        cmd_list.CopyFrom(
            std::span<byte>(
                reinterpret_cast<byte*>(const_cast<PrepareLightsTask*>(payload->tasks.data())),
                payload->tasks.size() * sizeof(PrepareLightsTask)
            ),
            payload->resources.task_buf->GetView(0, payload->tasks.size() * sizeof(PrepareLightsTask)),
            "Upload tasks"
        );
    }
    if (payload->primitive_light_infos.empty() && !payload->primitive_lights_initialized) {
        cmd_list.ClearResource(payload->resources.prim_light_buf->GetView(), 0u);
    } else if (!payload->primitive_light_infos.empty()) {
        cmd_list.CopyFrom(
            std::span<byte>(
                reinterpret_cast<byte*>(const_cast<PolymorphicLightInfo*>(payload->primitive_light_infos.data(
                ))),
                payload->primitive_light_infos.size() * sizeof(PolymorphicLightInfo)
            ),
            payload->resources.prim_light_buf->GetView(),
            "Upload prim light infos"
        );
    }
    cmd_list.AddCallback([payload(std::move(payload))]() {});
}

void PrepareLightPass::RecordClears(CommandList& cmd_list, SharedPtr<const RecordPayload> payload) {
    cmd_list.ClearResource(payload->resources.light_mapping_buf->GetView(), 0u);
    if (payload->reset_light_history) {
        cmd_list.ClearResource(payload->resources.light_data_buf->GetView(), 0u);
    }
    cmd_list.ClearResource(
        payload->resources.local_light_pdf_tex->GetView(
            0, payload->resources.local_light_pdf_tex->GetNumMips()
        ),
        float4(0.f)
    );
    cmd_list.AddCallback([payload(std::move(payload))]() {});
}

void PrepareLightPass::RecordSceneInputTransitions(
    CommandList&                   cmd_list,
    SharedPtr<const RecordPayload> payload
) {
    if (!payload->reads_scene_geometry) {
        return;
    }

    Array<ReadBuffer> buffers;
    buffers.reserve(6);
    for (const BufferRef& buffer : {
             payload->resources.primitive_buf,
             payload->resources.instance_buf,
             payload->resources.material_buf,
             payload->resources.position_buf,
             payload->resources.index_buf,
         }) {
        buffers.emplace_back(ReadBuffer{buffer->GetView(), EBufferState::SHADER_RESOURCE});
    }
    if (payload->resources.texcoord0_buf) {
        buffers.emplace_back(
            ReadBuffer{payload->resources.texcoord0_buf->GetView(), EBufferState::SHADER_RESOURCE}
        );
    }
    cmd_list.BufferBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Compute,
        std::move(buffers),
        Array<WriteBuffer>{}
    );

    if (!payload->resources.material_textures.empty()) {
        Array<ReadTexture> textures;
        textures.reserve(payload->resources.material_textures.size());
        for (const TextureRef& texture : payload->resources.material_textures) {
            textures.emplace_back(
                ReadTexture{texture->GetView(0, texture->GetNumMips()), ETextureState::SAMPLE}
            );
        }
        cmd_list.TextureBarriers(
            EQueueType::Graphics, EQueueType::Graphics, EPassType::Compute, std::move(textures)
        );
    }
}

void PrepareLightPass::RecordDispatch(CommandList& cmd_list, SharedPtr<const RecordPayload> payload) {
    if (payload->dispatch_light_count == 0) {
        return;
    }
    cmd_list
        .Compute(
            prepare_light_pipeline,
            payload->params,
            payload->resources.light_data_buf->GetView(),
            payload->resources.light_mapping_buf->GetView(),
            payload->resources.local_light_pdf_tex->GetView(),
            payload->resources.prim_light_buf->GetView(),
            payload->resources.task_buf->GetView(),
            payload->resources.bindless_array
        )
        .Dispatch(uint3((payload->dispatch_light_count + 255) / 256, 1, 1), "PrepareLights");
    cmd_list.AddCallback([payload(std::move(payload))]() {});
}

void PrepareLightPass::RecordGenerateMips(
    CommandList&                   cmd_list,
    SharedPtr<const RecordPayload> payload,
    MipDispatch                    dispatch
) {
    std::span<TextureView> mips(
        const_cast<TextureView*>(payload->resources.local_light_pdf_mips.data()) + dispatch.source_mip,
        dispatch.bound_mip_count
    );
    payload->resources.shader_utils->GenerateMipsChunk(cmd_list, mips);
    cmd_list.AddCallback([payload(std::move(payload))]() {});
}

void PrepareLightPass::Process(CommandList& cmd_list, const PreparedCommand& command) {
    const auto payload = command.record;
    cmd_list.PushScopeWithTimeScope("PrepareLights");
    RecordUploads(cmd_list, payload);
    RecordClears(cmd_list, payload);
    RecordSceneInputTransitions(cmd_list, payload);
    RecordDispatch(cmd_list, payload);
    for (const MipDispatch dispatch : payload->mip_dispatches) {
        RecordGenerateMips(cmd_list, payload, dispatch);
    }
    cmd_list.PopScopeWithTimeScope();
}

void PrepareLightPass::RecordLightingInputTransitions(CommandList& cmd_list, const PreparedCommand& command) {
    const auto payload = command.record;
    cmd_list.Barriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Compute,
        ReadBuffer{payload->resources.primitive_to_light_buf->GetView(), EBufferState::SHADER_RESOURCE},
        ReadBuffer{payload->resources.light_mapping_buf->GetView(), EBufferState::SHADER_RESOURCE},
        ReadBuffer{payload->resources.light_data_buf->GetView(), EBufferState::SHADER_RESOURCE},
        ReadTexture{
            payload->resources.local_light_pdf_tex->GetView(
                0, payload->resources.local_light_pdf_tex->GetNumMips()
            ),
            ETextureState::SAMPLE
        }
    );
}

void PrepareLightPass::RecordAcceptedBoundary(CommandList& cmd_list, const PreparedCommand& command) {
    const auto payload = command.record;

    Array<ReadBuffer> buffers;
    buffers.reserve(5);
    for (const BufferRef& buffer : {
             payload->resources.primitive_to_light_buf,
             payload->resources.task_buf,
             payload->resources.prim_light_buf,
             payload->resources.light_mapping_buf,
             payload->resources.light_data_buf,
         }) {
        buffers.emplace_back(ReadBuffer{buffer->GetView(), EBufferState::SHADER_RESOURCE});
    }
    cmd_list.BufferBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Compute,
        std::move(buffers),
        Array<WriteBuffer>{}
    );

    cmd_list.TextureBarriers(
        EQueueType::Graphics,
        EQueueType::Graphics,
        EPassType::Compute,
        Array<ReadTexture>{ReadTexture{
            payload->resources.local_light_pdf_tex->GetView(
                0, payload->resources.local_light_pdf_tex->GetNumMips()
            ),
            ETextureState::SHADER_RESOURCE
        }}
    );
}

bool PrepareLightPass::AddPasses(RenderGraph& graph, const PreparedCommand& command) {
    const auto payload = command.record;
    if (!payload || !payload->resources.primitive_to_light_buf || !payload->resources.task_buf ||
        !payload->resources.prim_light_buf || !payload->resources.light_mapping_buf ||
        !payload->resources.light_data_buf || !payload->resources.local_light_pdf_tex ||
        !payload->resources.bindless_array || !payload->resources.shader_utils) {
        return false;
    }
    if (payload->reads_scene_geometry &&
        (!payload->resources.primitive_buf || !payload->resources.instance_buf ||
         !payload->resources.material_buf || !payload->resources.position_buf ||
         !payload->resources.index_buf)) {
        return false;
    }

    const auto primitive_to_light = ImportRTGraphBuffer(
        graph, "RT.PrepareLights.primitive_to_light", payload->resources.primitive_to_light_buf
    );
    const auto tasks = ImportRTGraphBuffer(graph, "RT.PrepareLights.tasks", payload->resources.task_buf);
    const auto primitive_lights =
        ImportRTGraphBuffer(graph, "RT.PrepareLights.primitive_lights", payload->resources.prim_light_buf);
    const auto light_mapping =
        ImportRTGraphBuffer(graph, "RT.PrepareLights.light_mapping", payload->resources.light_mapping_buf);
    const auto light_data =
        ImportRTGraphBuffer(graph, "RT.PrepareLights.light_data", payload->resources.light_data_buf);
    const auto local_light_pdf = ImportRTGraphTexture(
        graph, "RT.PrepareLights.local_light_pdf", payload->resources.local_light_pdf_tex
    );
    const auto bindless =
        graph.ImportToken("RT.PrepareLights.bindless", payload->resources.bindless_array.Get());

    RenderGraph::BufferHandle         primitive_buf{};
    RenderGraph::BufferHandle         instance_buf{};
    RenderGraph::BufferHandle         material_buf{};
    RenderGraph::BufferHandle         position_buf{};
    RenderGraph::BufferHandle         index_buf{};
    RenderGraph::BufferHandle         texcoord0_buf{};
    Array<RenderGraph::TextureHandle> material_textures;
    if (payload->reads_scene_geometry) {
        primitive_buf =
            ImportRTGraphBuffer(graph, "RT.PrepareLights.scene_primitives", payload->resources.primitive_buf);
        instance_buf =
            ImportRTGraphBuffer(graph, "RT.PrepareLights.scene_instances", payload->resources.instance_buf);
        material_buf =
            ImportRTGraphBuffer(graph, "RT.PrepareLights.scene_materials", payload->resources.material_buf);
        position_buf =
            ImportRTGraphBuffer(graph, "RT.PrepareLights.scene_positions", payload->resources.position_buf);
        index_buf =
            ImportRTGraphBuffer(graph, "RT.PrepareLights.scene_indices", payload->resources.index_buf);
        if (payload->resources.texcoord0_buf) {
            texcoord0_buf = ImportRTGraphBuffer(
                graph, "RT.PrepareLights.scene_texcoords", payload->resources.texcoord0_buf
            );
        }
        material_textures.reserve(payload->resources.material_textures.size());
        for (size_t index = 0; index < payload->resources.material_textures.size(); ++index) {
            material_textures.emplace_back(ImportRTGraphTexture(
                graph,
                std::format("RT.PrepareLights.material_texture.{}", index),
                payload->resources.material_textures[index]
            ));
        }
    }

    const auto initial_persistent_buffer = [&](RenderGraph::BufferHandle buffer, bool initialized) {
        graph.SetInitialState(
            buffer,
            initialized ? RenderGraph::BufferState::ShaderResource : RenderGraph::BufferState::Undefined,
            initialized ? RenderGraph::QueueRole::Graphics : RenderGraph::QueueRole::None,
            initialized ? RenderGraph::AccessMode::Read : RenderGraph::AccessMode::None
        );
    };
    initial_persistent_buffer(primitive_to_light, payload->primitive_to_light_initialized);
    initial_persistent_buffer(tasks, payload->tasks_initialized);
    initial_persistent_buffer(primitive_lights, payload->primitive_lights_initialized);
    initial_persistent_buffer(light_mapping, payload->light_mapping_initialized);
    initial_persistent_buffer(light_data, payload->light_data_initialized);
    graph.SetInitialState(
        local_light_pdf,
        payload->local_light_pdf_initialized ? RenderGraph::TextureState::ShaderResource :
                                               RenderGraph::TextureState::Undefined,
        payload->local_light_pdf_initialized ? RenderGraph::QueueRole::Graphics :
                                               RenderGraph::QueueRole::None,
        payload->local_light_pdf_initialized ? RenderGraph::AccessMode::Read : RenderGraph::AccessMode::None
    );

    const auto initial_scene_buffer = [&](RenderGraph::BufferHandle buffer) {
        graph.SetInitialState(
            buffer,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
        graph.Export(
            buffer,
            RenderGraph::BufferState::ShaderResource,
            RenderGraph::QueueRole::Graphics,
            RenderGraph::AccessMode::Read
        );
    };
    if (payload->reads_scene_geometry) {
        for (const auto buffer : {
                 primitive_buf,
                 instance_buf,
                 material_buf,
                 position_buf,
                 index_buf,
             }) {
            initial_scene_buffer(buffer);
        }
        if (texcoord0_buf.IsValid()) {
            initial_scene_buffer(texcoord0_buf);
        }
        for (size_t index = 0; index < material_textures.size(); ++index) {
            const RenderGraph::TextureHandle texture = material_textures[index];
            const RenderGraph::TextureState  read_state =
                PreferredReadState(payload->resources.material_textures[index]);
            graph.SetInitialState(
                texture, read_state, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read
            );
            graph.Export(
                texture, read_state, RenderGraph::QueueRole::Graphics, RenderGraph::AccessMode::Read
            );
        }
    }

    graph.AddRecordPass(
        "RT.PrepareLights.UploadInputs",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy);
            if (!payload->primitive_to_light.empty()) {
                builder.Write(primitive_to_light, RenderGraph::BufferState::TransferDestination);
            }
            if (!payload->tasks.empty() || !payload->tasks_initialized) {
                builder.Write(tasks, RenderGraph::BufferState::TransferDestination);
            }
            if (!payload->primitive_light_infos.empty() || !payload->primitive_lights_initialized) {
                builder.Write(primitive_lights, RenderGraph::BufferState::TransferDestination);
            }
        },
        [this, payload](CommandList& cmd_list) {
            ScopedGpuMarker marker(cmd_list, "Pass: RT Prepare Lights Upload", GpuMarkerPalette::Transfer());
            RecordUploads(cmd_list, payload);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    graph.AddRecordPass(
        "RT.PrepareLights.ClearOutputs",
        [=](RenderGraph::PassBuilder& builder) {
            builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Copy)
                .Write(light_mapping, RenderGraph::BufferState::TransferDestination)
                .Write(local_light_pdf, RenderGraph::TextureState::TransferDestination);
            if (payload->reset_light_history) {
                builder.Write(light_data, RenderGraph::BufferState::TransferDestination);
            }
        },
        [this, payload](CommandList& cmd_list) {
            ScopedGpuMarker marker(cmd_list, "Pass: RT Prepare Lights Clear", GpuMarkerPalette::Transfer());
            RecordClears(cmd_list, payload);
        },
        RenderGraph::PassExecutionClass::ParallelRecordEligible
    );

    if (payload->dispatch_light_count != 0) {
        graph.AddRecordPass(
            "RT.PrepareLights.Dispatch",
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                    .Read(tasks, RenderGraph::BufferState::ShaderResource)
                    .ReadWrite(light_data, RenderGraph::BufferState::UnorderedAccess)
                    .ReadWrite(light_mapping, RenderGraph::BufferState::UnorderedAccess)
                    .ReadWrite(
                        local_light_pdf,
                        RenderGraph::TextureState::UnorderedAccess,
                        RenderGraph::TextureRange::Mips(0, 1)
                    );
                if (!payload->primitive_light_infos.empty()) {
                    builder.Read(primitive_lights, RenderGraph::BufferState::ShaderResource);
                }
                if (payload->reads_scene_geometry) {
                    builder.Read(primitive_buf, RenderGraph::BufferState::ShaderResource)
                        .Read(instance_buf, RenderGraph::BufferState::ShaderResource)
                        .Read(material_buf, RenderGraph::BufferState::ShaderResource)
                        .Read(position_buf, RenderGraph::BufferState::ShaderResource)
                        .Read(index_buf, RenderGraph::BufferState::ShaderResource)
                        .Read(bindless);
                    if (texcoord0_buf.IsValid()) {
                        builder.Read(texcoord0_buf, RenderGraph::BufferState::ShaderResource);
                    }
                    for (const auto texture : material_textures) {
                        builder.Read(texture, RenderGraph::TextureState::Sampled);
                    }
                }
            },
            [this, payload](CommandList& cmd_list) {
                ScopedGpuMarker marker(
                    cmd_list, "Pass: RT Prepare Lights Dispatch", GpuMarkerPalette::Subpass()
                );
                RecordDispatch(cmd_list, payload);
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    }

    for (const MipDispatch dispatch : payload->mip_dispatches) {
        graph.AddRecordPass(
            std::format("RT.PrepareLights.GenerateMips.{}", dispatch.source_mip),
            [=](RenderGraph::PassBuilder& builder) {
                builder.ExecuteOn(RenderGraph::QueueRole::Graphics, RenderGraph::PipelineType::Compute)
                    .Read(
                        local_light_pdf,
                        RenderGraph::TextureState::UnorderedAccess,
                        RenderGraph::TextureRange::Mips(dispatch.source_mip, 1)
                    )
                    .Write(
                        local_light_pdf,
                        RenderGraph::TextureState::UnorderedAccess,
                        RenderGraph::TextureRange::Mips(dispatch.source_mip + 1, dispatch.bound_mip_count - 1)
                    );
            },
            [this, payload, dispatch](CommandList& cmd_list) {
                ScopedGpuMarker marker(
                    cmd_list, "Pass: RT Prepare Lights Generate Mips", GpuMarkerPalette::Subpass()
                );
                RecordGenerateMips(cmd_list, payload, dispatch);
            },
            RenderGraph::PassExecutionClass::ParallelRecordEligible
        );
    }

    graph.Export(
        tasks,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    graph.Export(
        primitive_lights,
        RenderGraph::BufferState::ShaderResource,
        RenderGraph::QueueRole::Graphics,
        RenderGraph::AccessMode::Read
    );
    return true;
}

void PrepareLightPass::CommitAcceptedFrame(RTContext& rt_ctx, PreparedCommand&& command) noexcept {
    instance_light_buffer_offsets.swap(command.next_instance_light_buffer_offsets);
    primitive_light_buffer_offsets.swap(command.next_primitive_light_buffer_offsets);
    b_odd_frame                          = command.next_odd_frame;
    accepted_primitive_to_light_identity = command.next_primitive_to_light_identity;
    accepted_tasks_identity              = command.next_tasks_identity;
    accepted_primitive_lights_identity   = command.next_primitive_lights_identity;
    accepted_light_mapping_identity      = command.next_light_mapping_identity;
    accepted_light_data_identity         = command.next_light_data_identity;
    accepted_local_light_pdf_identity    = command.next_local_light_pdf_identity;
    accepted_scene_revision              = command.next_scene_revision;
    rt_ctx.is_ctx.CommitAcceptedLightBufferParams(command.record->light_buffer_params);
}

} // namespace Moer::Render::Raytracing
