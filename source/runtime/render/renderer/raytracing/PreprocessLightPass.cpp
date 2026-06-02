#include "PreprocessLightPass.h"

#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/Scene.h"
#include "ShaderUtils.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "taskgraph/TaskGraph.h"
#include "trace/Trace.h"

#include <algorithm>
#include <cmath>

namespace Moer::Render::Raytracing {

namespace {

struct RGPrepareLightsUploadParams {
    DEFINE_RG_BUFFER_ACCESS(primitive_to_light, EBufferState::TRANSFER_DST);
    DEFINE_RG_BUFFER_ACCESS(tasks, EBufferState::TRANSFER_DST);
    DEFINE_RG_BUFFER_ACCESS(prim_lights, EBufferState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(primitive_to_light, tasks, prim_lights);
};

struct RGPrepareLightsClearParams {
    DEFINE_RG_BUFFER_ACCESS(light_mapping, EBufferState::TRANSFER_DST);
    DEFINE_RG_TEXTURE_ACCESS(local_light_pdf, ETextureState::TRANSFER_DST);
    DEFINE_RG_PARAMETER_ACCESS(light_mapping, local_light_pdf);
};

struct RGPrepareLightsDispatchParams {
    DEFINE_RG_BUFFER_ACCESS(light_data, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(light_mapping, EBufferState::UNORDERED_ACCESS);
    DEFINE_RG_TEXTURE_ACCESS(local_light_pdf, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_BUFFER_ACCESS(prim_lights, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(tasks, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_BUFFER_ACCESS(primitive_to_light, EBufferState::SHADER_RESOURCE);
    DEFINE_RG_PARAMETER_ACCESS(light_data, light_mapping, local_light_pdf, prim_lights, tasks, primitive_to_light);
};

struct RGPrepareLightsGenerateMipsParams {
    DEFINE_RG_TEXTURE_ACCESS(local_light_pdf, ETextureState::UNORDERED_ACCESS);
    DEFINE_RG_PARAMETER_ACCESS(local_light_pdf);
};

bool HasUploadWork(const PrepareLightPass::PreparedCommand& command) {
    return !command.primitive_to_light.empty() || !command.tasks.empty() || !command.prim_light_infos.empty();
}

uint RoundUpToChunk(uint value, uint chunk) {
    return (value + chunk - 1) & ~(chunk - 1);
}

constexpr uint s_prepare_light_parallel_threshold = 256;

struct SceneLightCandidate {
    bool                 valid             = false;
    entt::entity         entity            = entt::null;
    ELightType           type              = ELightType::None;
    uint                 prev_light_offset = uint(-1);
    PolymorphicLightInfo info{};
};

uint ResolvePrepareWorkerCount(const RTContext& rt_ctx, uint item_count) {
    if (!rt_ctx.b_parallel_process_light || item_count < s_prepare_light_parallel_threshold) {
        return 1;
    }

    uint worker_count = TaskGraph::GetInterface().GetWorkerThreadCount();
    if (worker_count == 0) {
        return 1;
    }

    const uint requested_count = std::max(1u, rt_ctx.num_threads);
    worker_count              = std::min(worker_count, requested_count);
    worker_count              = std::min(worker_count, item_count);
    return std::max(1u, worker_count);
}

template<typename FunctionType>
void RunPrepareRanges(uint item_count, uint worker_count, FunctionType&& function) {
    if (item_count == 0) {
        return;
    }

    if (worker_count <= 1) {
        function(0, 0, item_count);
        return;
    }

    const uint chunk_size = (item_count + worker_count - 1) / worker_count;

    GraphEventArray events;
    events.reserve(worker_count);
    for (uint worker_index = 0; worker_index < worker_count; ++worker_index) {
        const uint begin = worker_index * chunk_size;
        const uint end   = std::min(begin + chunk_size, item_count);
        if (begin >= end) {
            break;
        }

        events.push_back(LambdaTask::Dispatch([worker_index, begin, end, &function]() {
            function(worker_index, begin, end);
        }));
    }

    TaskGraph::GetInterface().WaitUntilTasksComplete(events, EThread::UNKNOWN_THREAD);
}

} // namespace

PrepareLightPass::PrepareLightPass(RenderDevice& _device, ShaderManager& _manager, Scene& _scene) :
    device(_device),
    manager(_manager),
    scene(_scene),
    prepare_light_pipeline(
        ShaderManager::Get().Compute<PrepareLightShaderPipeline>(
            "pipelines/raytracing/lighting/precompute/PrepareLights.hlsl"
        )
    ) {}

static uint LightPriority(entt::entity entity, const Moer::Scene& scene) {
    auto& r = scene.r();
    if (!r.valid(entity) || !r.all_of<ecs::CLight>(entity)) {
        return 0;
    }

    const auto& c_light = r.get<ecs::CLight>(entity);
    switch (c_light.type) {
        case Moer::ELightType::Directional:
            return 1;
        case Moer::ELightType::Environment:
            return 2;
        default:
            return 0;
    }
}

static inline uint FloatToUInt(float _v, float _scale) {
    return (uint)floor(_v * _scale + 0.5f);
}

static inline float Saturate(float _v) {
    return std::clamp(_v, 0.f, 1.f);
}

static inline uint
FloaT3ToR8G8B8Unorm(float _unpacked_input_x, float _unpacked_input_y, float _unpacked_input_z) {
    return (FloatToUInt(Saturate(_unpacked_input_x), 0xFF) & 0xFF) |
           ((FloatToUInt(Saturate(_unpacked_input_y), 0xFF) & 0xFF) << 8) |
           ((FloatToUInt(Saturate(_unpacked_input_z), 0xFF) & 0xFF) << 16);
}

static uint16_t Fp32ToFp16(float _v) {
    // Multiplying by 2^-112 causes exponents below -14 to denormalize
    static const union FU {
        uint  ui;
        float f;
    } multiple = {0x07800000}; // 2**-112 0(1) 15(8) 0(23)

    FU biased_float;
    biased_float.f = _v * multiple.f;
    const uint u   = biased_float.ui;

    const uint sign = u & 0x80000000;
    uint       body = u & 0x0fffffff;

    return (uint16_t)(sign >> 16 | body >> 13) & 0xFFFF;
}

static void PackPolyLightColor(float3 _color, PolymorphicLightInfo& _info) {
    float max_radiance = Max(Max(_color.x, _color.y), _color.z);
    if (max_radiance < 0.f)
        return;

    float log_radiance   = (std::log2f(max_radiance) - g_poly_morphic_light_min_log2_radiance) /
                           (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance);
    log_radiance         = std::clamp(log_radiance, 0.f, 1.f);
    uint packed_radiance = std::min(uint(ceilf(log_radiance * 65534.f)) + 1, 0xffffu);
    uint unpacked_radiance = std::exp2f(
        (float(packed_radiance - 1) / 65534.f) *
            (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance) +
        g_poly_morphic_light_min_log2_radiance
    );

    _info.color_type_flags |= FloaT3ToR8G8B8Unorm(
        _color.x / unpacked_radiance, _color.y / unpacked_radiance, _color.z / unpacked_radiance
    );
    _info.log_radiance = packed_radiance;
}

static float2 UnitVectorToOctahedron(const float3 _n) {
    float  m  = abs(_n.x) + abs(_n.y) + abs(_n.z);
    float2 xy = {_n.x, _n.y};
    xy.x /= m;
    xy.y /= m;
    if (_n.z <= 0.0f) {
        float2 signs;
        signs.x = xy.x >= 0.0f ? 1.0f : -1.0f;
        signs.y = xy.y >= 0.0f ? 1.0f : -1.0f;
        float x = (1.0f - abs(xy.y)) * signs.x;
        float y = (1.0f - abs(xy.x)) * signs.y;
        xy.x    = x;
        xy.y    = y;
    }
    return {xy.x, xy.y};
}

static uint32_t PackNormalizedVector(const float3 _x) {
    float2 xy          = UnitVectorToOctahedron(_x);
    xy.x               = xy.x * .5f + .5f;
    xy.y               = xy.y * .5f + .5f;
    uint x             = FloatToUInt(Saturate(xy.x), (1 << 16) - 1);
    uint y             = FloatToUInt(Saturate(xy.y), (1 << 16) - 1);
    uint packed_output = x;
    packed_output |= y << 16;
    return packed_output;
}
static bool CanConvert(entt::entity entity, const Moer::Scene& scene) {
    auto& r = scene.r();
    if (!r.valid(entity))
        return false;

    // 检查是否有 CLight 组件
    if (!r.all_of<ecs::CLight>(entity))
        return false;

    const auto& c_light = r.get<ecs::CLight>(entity);
    switch (c_light.type) {
        case Moer::ELightType::Directional:
            return r.all_of<ecs::CLightDirectional, ecs::CTransform>(entity);
        case Moer::ELightType::Point:
            return r.all_of<ecs::CLightPoint, ecs::CTransform>(entity);
        case Moer::ELightType::Spot:
            // TODO: CLightSpot 还未实现
            return false;
        case Moer::ELightType::Environment:
            // TODO: CLightEnvironment 还未实现
            return false;
        default:
            return false;
    }
}

static bool ConvertLight(entt::entity entity, const Moer::Scene& scene, PolymorphicLightInfo& _info) {
    if (!CanConvert(entity, scene))
        return false;

    auto&       r       = scene.r();
    const auto& c_light = r.get<ecs::CLight>(entity);

    static int first_time = 12;
    first_time -= 1;

    switch (c_light.type) {
        case Moer::ELightType::Directional: {
            const auto& c_light_dir = r.get<ecs::CLightDirectional>(entity);

            float angular_size = 0.533f;

            float half_angular_size_rad = Angle::DegreeToRadian(std::max(angular_size, 0.1f));
            float solid_angle           = 2 * PI * (1 - cos(half_angular_size_rad));

            float3 radiance = c_light_dir.color * c_light_dir.intensity / std::max(solid_angle, 1e-6f);

            float max_radiance = Max(Max(radiance.x, radiance.y), radiance.z);
            if (max_radiance > g_poly_morphic_light_max_radiance) {
                radiance = radiance / max_radiance * g_poly_morphic_light_max_radiance;
            }

            _info.color_type_flags = (uint)EPolyLightType::ELDirectional << g_poly_morphic_light_type_shift;
            PackPolyLightColor(radiance, _info);

            // Fetch the directional light direction from LogicalScene.
            float3 direction = scene.GetLogicalScene().GetDirectionalLightDirection(entity);
            _info.direction1 = PackNormalizedVector(Normalizef(direction));
            _info.scalars    = Fp32ToFp16(half_angular_size_rad) | (Fp32ToFp16(solid_angle) << 16);
            break;
        }
        case Moer::ELightType::Spot: {
            // TODO: CLightSpot 还未实现
            LOG_INFO(MOER_TEXT("Spot light not yet supported in new ECS system"));
            return false;
        }
        case Moer::ELightType::Point: {
            const auto& c_light_point = r.get<ecs::CLightPoint>(entity);
            float3      flux          = c_light_point.color * c_light_point.intensity;
            _info.color_type_flags    = (uint)EPolyLightType::ELPoint << g_poly_morphic_light_type_shift;

            float max_flux = Max(Max(flux.x, flux.y), flux.z);
            if (max_flux > g_poly_morphic_light_max_flux) {
                flux = flux / max_flux * g_poly_morphic_light_max_flux;
            }

            PackPolyLightColor(flux, _info);

            _info.center = scene.GetLogicalScene().GetPointLightPosition(entity);
            break;
        }
        case Moer::ELightType::Environment: {
            // TODO: CLightEnvironment 还未实现
            LOG_INFO(MOER_TEXT("Environment light not yet supported in new ECS system"));
            return false;
        }
        default: {
            LOG_INFO(MOER_TEXT("Light Type {} not supported"), uint(c_light.type));
            return false;
        }
    }
    return true;
}

void PrepareLightPass::RebuildEmissivePrimitiveCache() {
    TRACE_SCOPE_CAT("Raytracing.ProcessLights.RebuildEmissivePrimitiveCache", "Frame");

    auto& r = scene.r();
    const uint primitive_count = scene.GetCpuScene().GetPrimitiveCount();

    cached_emissive_primitives.clear();
    cached_primitive_to_light = Array<uint>(std::max(1u, primitive_count), s_invalid_light_idx);
    cached_emissive_triangle_count = 0;
    cached_primitive_count = primitive_count;
    cached_primitive_to_light_dirty = true;
    cached_primitive_to_light_upload_target = nullptr;

    r.view<const ecs::CPrimitive>().each([&](const auto primitive_entt, const ecs::CPrimitive& c_primitive) {
        if (!r.valid(c_primitive.material_entt) || !r.all_of<ecs::CMaterial>(c_primitive.material_entt)) {
            return;
        }

        const ecs::CMaterial& c_material = r.get<ecs::CMaterial>(c_primitive.material_entt);
        if (c_material.emissive_factor == float3(0.f)) {
            return;
        }

        const uint primitive_id = scene.GetCpuScene().GetPrimitiveId(primitive_entt);
        if (primitive_id == UINT_MAX) {
            LOG_ERROR(MOER_TEXT("Primitive entity {} not found in CpuScene"), entt::to_integral(primitive_entt));
            return;
        }

        EmissivePrimitiveEntry entry{};
        entry.primitive_id       = primitive_id;
        entry.light_offset       = cached_emissive_triangle_count;
        entry.num_triangles      = c_primitive.index_count / 3;
        entry.index_start_idx    = c_primitive.index.is_valid ? c_primitive.index.start_idx : 0;
        entry.first_instance_idx = scene.GetCpuScene().GetFirstInstanceIndex(primitive_id);

        if (primitive_id >= cached_primitive_to_light.size()) {
            cached_primitive_to_light.resize(primitive_id + 1, s_invalid_light_idx);
        }
        cached_primitive_to_light[primitive_id] = entry.light_offset;

        cached_emissive_triangle_count += entry.num_triangles;
        cached_emissive_primitives.emplace_back(entry);
    });
}

PrepareLightPass::PreparedCommand PrepareLightPass::Prepare(RTContext& _rt_ctx) {

    TRACE_SCOPE_CAT("Raytracing.ProcessLights", "Frame");

    Array<PrepareLightsTask>    tasks;
    Array<PolymorphicLightInfo> prim_light_infos;

    uint light_buf_offset = 0;

    auto& r = scene.r();
    const uint primitive_count = scene.GetCpuScene().GetPrimitiveCount();
    if (cached_primitive_count != primitive_count) {
        RebuildEmissivePrimitiveCache();
    }

    const uint num_emissive_meshes    = static_cast<uint>(cached_emissive_primitives.size());
    const uint num_emissive_triangles = cached_emissive_triangle_count;
    {
        TRACE_SCOPE_CAT("Raytracing.ProcessLights.BuildEmissiveTasksFromCache", "Frame");
        for (const EmissivePrimitiveEntry& entry : cached_emissive_primitives) {
            const auto iter = instance_light_buffer_offsets.find(static_cast<uint64>(entry.primitive_id));

            PrepareLightsTask task{};
            task.primitive_id       = entry.primitive_id;
            task.light_offset       = entry.light_offset;
            task.num_triangles      = entry.num_triangles;
            task.prev_light_offset  = iter == instance_light_buffer_offsets.end() ? uint(-1) : iter->second;
            task.index_start_idx    = entry.index_start_idx;
            task.first_instance_idx = entry.first_instance_idx;

            instance_light_buffer_offsets[static_cast<uint64>(entry.primitive_id)] = entry.light_offset;

            tasks.emplace_back(task);
        }
        light_buf_offset = cached_emissive_triangle_count;
    }

    uint num_finite_prim_lights   = 0;
    uint num_infinite_prim_lights = 0;
    uint num_is_env_lights        = 0;

    Array<entt::entity> light_entities{};
    {
        TRACE_SCOPE_CAT("Raytracing.ProcessLights.CollectLightEntities", "Frame");
        auto light_view = scene.r().view<const ecs::CLight>();
        light_entities  = Array<entt::entity>(light_view.begin(), light_view.end());
    }

    {
        TRACE_SCOPE_CAT("Raytracing.ProcessLights.SortLights", "Frame");
        std::ranges::sort(light_entities, [&](entt::entity _lhs, entt::entity _rhs) {
            return LightPriority(_lhs, scene) < LightPriority(_rhs, scene);
        });
    }

    Array<SceneLightCandidate> scene_light_candidates(light_entities.size());
    {
        const uint worker_count = ResolvePrepareWorkerCount(_rt_ctx, static_cast<uint>(light_entities.size()));
        TRACE_SCOPE_CAT(
            worker_count > 1 ? "Raytracing.ProcessLights.ConvertSceneLights.Parallel" :
                               "Raytracing.ProcessLights.ConvertSceneLights.Serial",
            "Frame"
        );
        RunPrepareRanges(static_cast<uint>(light_entities.size()), worker_count, [&](uint, uint begin, uint end) {
            for (uint index = begin; index < end; ++index) {
                const entt::entity entity = light_entities[index];
                PolymorphicLightInfo light_info{};
                if (!ConvertLight(entity, scene, light_info)) {
                    continue;
                }

                const auto pre_iter = primitive_light_buffer_offsets.find(uint64(entity));
                scene_light_candidates[index] = SceneLightCandidate{
                    .valid             = true,
                    .entity            = entity,
                    .type              = scene.r().get<ecs::CLight>(entity).type,
                    .prev_light_offset = pre_iter == primitive_light_buffer_offsets.end() ? uint(-1) : pre_iter->second,
                    .info              = light_info
                };
            }
        });
    }

    {
        TRACE_SCOPE_CAT("Raytracing.ProcessLights.MergeSceneLights", "Frame");
        prim_light_infos.reserve(scene_light_candidates.size());
        for (const SceneLightCandidate& candidate : scene_light_candidates) {
            if (!candidate.valid) {
                continue;
            }

            PrepareLightsTask task{};
            task.primitive_id      = prim_light_infos.size() | g_task_prim_light_bit;
            task.light_offset      = light_buf_offset;
            task.num_triangles     = 1;
            task.prev_light_offset = candidate.prev_light_offset;

            primitive_light_buffer_offsets[uint64(candidate.entity)] = light_buf_offset;
            light_buf_offset += task.num_triangles;
            tasks.emplace_back(task);
            prim_light_infos.emplace_back(candidate.info);

            switch (candidate.type) {
                case Moer::ELightType::Directional: {
                    num_infinite_prim_lights++;
                    break;
                }
                case Moer::ELightType::Environment: {
                    num_is_env_lights++;
                    break;
                }
                default: {
                    num_finite_prim_lights++;
                    break;
                }
            }
        }
    }

    // Handle env lighting from RTContext directly. rt_ctx.env_map is not assigned here, so use env_pdf_tex for dimensions.
    {
        TRACE_SCOPE_CAT("Raytracing.ProcessLights.AddEnvironmentLight", "Frame");
        if (_rt_ctx.scene_params.enable_env_map && _rt_ctx.env_pdf_tex) {
            PolymorphicLightInfo light_info{};
            light_info.color_type_flags = (uint)EPolyLightType::ELEnv << g_poly_morphic_light_type_shift;

            float3 color_scale = float3(_rt_ctx.scene_params.env_map_scale);
            PackPolyLightColor(color_scale, light_info);

            light_info.direction1 = _rt_ctx.scene_params.env_map_handle;
            uint3 env_extent      = _rt_ctx.env_pdf_tex->GetExtent();
            light_info.direction2 = env_extent.x | (env_extent.y << 16);
            light_info.scalars    = Fp32ToFp16(_rt_ctx.scene_params.env_map_rotation);
            light_info.scalars |= g_poly_morphic_light_env_is_scalar_bit;

            constexpr uint64 env_light_key = ~0ull;
            auto             pre_iter      = primitive_light_buffer_offsets.find(env_light_key);

            PrepareLightsTask task{};
            task.primitive_id      = prim_light_infos.size() | g_task_prim_light_bit;
            task.light_offset      = light_buf_offset;
            task.num_triangles     = 1;
            task.prev_light_offset = pre_iter == primitive_light_buffer_offsets.end() ? -1 : pre_iter->second;

            primitive_light_buffer_offsets[env_light_key] = light_buf_offset;
            light_buf_offset += task.num_triangles;
            tasks.emplace_back(task);
            prim_light_infos.emplace_back(light_info);

            num_is_env_lights++;
        }
    }

    {
        TRACE_SCOPE_CAT("Raytracing.ProcessLights.CreateBuffersIfNeeded", "Frame");
        static constexpr uint s_mesh_alloc_chunk      = 128;
        static constexpr uint s_triangle_alloc_chunk  = 1024;
        static constexpr uint s_primitive_alloc_chunk = 128;
        _rt_ctx.CreateBuffersIfNeeded(
            RoundUpToChunk(num_emissive_meshes, s_mesh_alloc_chunk),
            RoundUpToChunk(num_emissive_triangles, s_triangle_alloc_chunk),
            RoundUpToChunk(static_cast<uint>(prim_light_infos.size()), s_primitive_alloc_chunk),
            std::max(1u, static_cast<uint>(cached_primitive_to_light.size()))
        );
    }

    const bool upload_primitive_to_light = cached_primitive_to_light_dirty ||
                                           cached_primitive_to_light_upload_target !=
                                               RTRHI(_rt_ctx.primitive_to_light_buf).Get();

    PrepareLightsParams param{};
    {
        TRACE_SCOPE_CAT("Raytracing.ProcessLights.BuildParams", "Frame");
        param.num_tasks = tasks.size();

        param.primitive_buf_hdl  = _rt_ctx.GetBindlessHandles().primitive_buf_hdl;
        param.instance_buf_hdl   = _rt_ctx.GetBindlessHandles().instance_buf_hdl;
        param.material_buf_hdl   = _rt_ctx.GetBindlessHandles().material_buf_hdl;
        param.position_buf_hdl   = _rt_ctx.GetBindlessHandles().position_buf_hdl;
        param.index_buf_hdl      = _rt_ctx.GetBindlessHandles().index_buf_hdl;
        param.texcoord0_buf_hdl  = _rt_ctx.GetBindlessHandles().texcoord0_buf_hdl;
        param.primitive_to_light = _rt_ctx.GetBindlessHandles().primitive_to_light;

        uint max_lights_in_buffer = uint(_rt_ctx.light_data_buf->GetNumElement() / 2);
        param.cur_light_offset    = max_lights_in_buffer * b_odd_frame;
        param.prev_light_offset   = max_lights_in_buffer * !b_odd_frame;

        _rt_ctx.is_ctx.SetLightBufferParams(
            param.cur_light_offset,
            num_finite_prim_lights + num_emissive_triangles,
            num_infinite_prim_lights,
            num_is_env_lights
        );
    }

    b_odd_frame = !b_odd_frame;

    PreparedCommand command{};
    if (upload_primitive_to_light) {
        command.primitive_to_light = cached_primitive_to_light;
        cached_primitive_to_light_dirty = false;
        cached_primitive_to_light_upload_target = RTRHI(_rt_ctx.primitive_to_light_buf).Get();
    }
    command.tasks                = std::move(tasks);
    command.prim_light_infos     = std::move(prim_light_infos);
    command.params               = param;
    command.dispatch_light_count = light_buf_offset;
    return command;
}

PrepareLightPass::RecordResources PrepareLightPass::CaptureResources(RTContext& _rt_ctx) {
    return RecordResources{
        .primitive_to_light_buf = RTRHI(_rt_ctx.primitive_to_light_buf),
        .task_buf               = RTRHI(_rt_ctx.task_buf),
        .prim_light_buf         = RTRHI(_rt_ctx.prim_light_buf),
        .light_mapping_buf      = RTRHI(_rt_ctx.light_mapping_buf),
        .light_data_buf         = RTRHI(_rt_ctx.light_data_buf),
        .local_light_pdf_tex    = RTRHI(_rt_ctx.local_light_pdf_tex),
        .local_light_pdf_mips   = _rt_ctx.local_light_pdf_mips,
        .bindless_array         = _rt_ctx.GetBindlessArray(),
        .shader_utils           = &_rt_ctx.sd_utils
    };
}

void PrepareLightPass::RecordUploads(
    CommandList&           _cmd_list,
    const PreparedCommand& _command,
    const RecordResources& _resources
) {
    if (!_command.primitive_to_light.empty()) {
        _cmd_list.CopyFrom(
            std::span<byte>(
                (byte*)_command.primitive_to_light.data(),
                _command.primitive_to_light.size() * sizeof(uint)
            ),
            _resources.primitive_to_light_buf->GetView(),
            MOER_TEXT("Upload primitive_to_light")
        );
    }

    if (!_command.tasks.empty()) {
        _cmd_list.CopyFrom(
            std::span<byte>((byte*)_command.tasks.data(), _command.tasks.size() * sizeof(PrepareLightsTask)),
            _resources.task_buf->GetView(0, _command.tasks.size() * sizeof(PrepareLightsTask)),
            MOER_TEXT("Upload tasks")
        );
    }

    if (!_command.prim_light_infos.empty()) {
        _cmd_list.CopyFrom(
            std::span<byte>(
                (byte*)_command.prim_light_infos.data(),
                _command.prim_light_infos.size() * sizeof(PolymorphicLightInfo)
            ),
            _resources.prim_light_buf->GetView(),
            MOER_TEXT("Upload prim light infos")
        );
    }
}

void PrepareLightPass::RecordClears(CommandList& _cmd_list, const RecordResources& _resources) {
    const TextureView full_local_light_pdf = _resources.local_light_pdf_tex->GetView(
        _resources.local_light_pdf_tex->GetFormat(),
        0,
        static_cast<uint8>(_resources.local_light_pdf_tex->GetNumMips())
    );
    _cmd_list.ClearResource(_resources.light_mapping_buf->GetView(), 0u);
    _cmd_list.ClearResource(full_local_light_pdf, float4(0.f));
}

void PrepareLightPass::RecordPrepareLights(
    CommandList&           _cmd_list,
    const PreparedCommand& _command,
    const RecordResources& _resources
) {
    _cmd_list
        .Compute(
            prepare_light_pipeline,
            _command.params,
            _resources.light_data_buf->GetView(),
            _resources.light_mapping_buf->GetView(),
            _resources.local_light_pdf_tex->GetView(),
            _resources.prim_light_buf->GetView(),
            _resources.task_buf->GetView(),
            _resources.bindless_array
        )
        .Dispatch(uint3((_command.dispatch_light_count + 255) / 256, 1, 1), MOER_TEXT("PrepareLights"));
}

void PrepareLightPass::RecordGenerateMips(CommandList& _cmd_list, RecordResources& _resources) {
    std::span<TextureView> mips(_resources.local_light_pdf_mips.data(), _resources.local_light_pdf_mips.size());
    _resources.shader_utils->GenerateMips(_cmd_list, mips, ETextureState::SHADER_RESOURCE);
}

void PrepareLightPass::AddPasses(
    RenderGraph& _graph,
    const RTGraphFrameResources& _rg,
    RTContext& _rt_ctx,
    PreparedCommand&& _command
) {
    auto* command   = _graph.Alloc<PreparedCommand>(std::move(_command));
    auto* resources = _graph.Alloc<RecordResources>(CaptureResources(_rt_ctx));

    if (HasUploadWork(*command)) {
        auto* upload_params                 = _graph.Alloc<RGPrepareLightsUploadParams>();
        upload_params->primitive_to_light   = RGBufferView{.buffer = _rg.primitive_to_light_buf};
        upload_params->tasks                = RGBufferView{.buffer = _rg.task_buf};
        upload_params->prim_lights          = RGBufferView{.buffer = _rg.prim_light_buf};
        _graph.AddPass(
            MOER_TEXT("RT.PrepareLights.Upload"),
            upload_params,
            ERGPassFlags::Graphics,
            [this, command, resources](RHICommandList& cmd_list, RGContext) {
                RecordUploads(cmd_list, *command, *resources);
            }
        );
    }

    auto* clear_params            = _graph.Alloc<RGPrepareLightsClearParams>();
    clear_params->light_mapping   = RGBufferView{.buffer = _rg.light_mapping_buf};
    clear_params->local_light_pdf = RTWholeTextureView(_rg.local_light_pdf_tex);
    _graph.AddPass(
        MOER_TEXT("RT.PrepareLights.Clear"),
        clear_params,
        s_rt_graph_graphics_compute_pass,
        [this, resources](RHICommandList& cmd_list, RGContext) {
            RecordClears(cmd_list, *resources);
        }
    );

    if (command->dispatch_light_count > 0) {
        auto* dispatch_params               = _graph.Alloc<RGPrepareLightsDispatchParams>();
        dispatch_params->light_data         = RGBufferView{.buffer = _rg.light_data_buf};
        dispatch_params->light_mapping      = RGBufferView{.buffer = _rg.light_mapping_buf};
        dispatch_params->local_light_pdf    = RTWholeTextureView(_rg.local_light_pdf_tex);
        dispatch_params->prim_lights        = RGBufferView{.buffer = _rg.prim_light_buf};
        dispatch_params->tasks              = RGBufferView{.buffer = _rg.task_buf};
        dispatch_params->primitive_to_light = RGBufferView{.buffer = _rg.primitive_to_light_buf};
        _graph.AddPass(
            MOER_TEXT("RT.PrepareLights.Dispatch"),
            dispatch_params,
            s_rt_graph_graphics_compute_pass,
            [this, command, resources](RHICommandList& cmd_list, RGContext) {
                RecordPrepareLights(cmd_list, *command, *resources);
            }
        );
    }

    if (!resources->local_light_pdf_mips.empty()) {
        auto* mip_params            = _graph.Alloc<RGPrepareLightsGenerateMipsParams>();
        mip_params->local_light_pdf = RTWholeTextureView(_rg.local_light_pdf_tex);
        _graph.AddPass(
            MOER_TEXT("RT.PrepareLights.GenerateMips"),
            mip_params,
            s_rt_graph_graphics_compute_pass,
            [this, resources](RHICommandList& cmd_list, RGContext) {
                RecordGenerateMips(cmd_list, *resources);
            }
        );
    }
}

} // namespace Moer::Render::Raytracing