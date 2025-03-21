#include "PreprocessLightPass.h"

#include "misc/Hash.h"
#include "misc/STL.h"
#include "misc/Timer.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/Entity.h"
#include "scene/MaterialInstance.h"
#include "scene/RenderableManager.h"
#include "scene/Scene.h"
#include "scene/light/LightComponent.h"
#include "scene/light/LightComponentManager.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "taskgraph/TaskGraph.h"

#include <algorithm>
#include <cmath>
#include <ranges>

namespace Moer::Render::Raytracing {

template<typename T>
struct CachelineStruct {
    union {
        byte padding[PLATFORM_CACHELINE_SIZE];
        T    data;
    };
    CachelineStruct() noexcept { memset(padding, 0, sizeof(padding)); }
    CachelineStruct(const T& _data) : data(_data) {}
    CachelineStruct(const CachelineStruct& _rhs) : data(_rhs.data) {}
    CachelineStruct& operator=(const CachelineStruct& _rhs) {
        data = _rhs.data;
        return *this;
    }

    operator T&() { return data; }
    operator const T&() const { return data; }
};

PrepareLightPass::PrepareLightPass(RenderDevice& _device, ShaderManager& _manager, Scene& _scene) :
    device(_device),
    manager(_manager),
    scene(_scene),
    prepare_light_pipeline(
        ShaderManager::Get().Compute<PrepareLightShaderPipeline>("lighting/PrepareLights.hlsl")
    ) {}

void PrepareLightPass::CountEmissiveInstances(uint& _num_emissive_meshes, uint& _num_emissive_triangles) {
    _num_emissive_meshes    = 0;
    _num_emissive_triangles = 0;
    scene.ForEach([&](Entity _entity) {
        const MeshInfo&                mesh_info     = *RenderableManager::Get().GetMeshInfo(_entity);
        std::span<MaterialInstanceRef> mat_instances = RenderableManager::Get().GetMaterialInstances(_entity);
        for (uint i = 0; i < mesh_info.geometries.size(); ++i) {
            const MaterialInstanceRef& mat_instance = mat_instances[i];
            float3                     emissive     = mat_instance->GetParameter<float3>("emissive_factor");
            if (emissive != float3(0.f)) {
                _num_emissive_meshes++;
                _num_emissive_triangles += mesh_info.geometries[i]->local_idx_count / 3;
            }
        }
    });
}

static uint LightPriority(LightComponentRef _light) {
    switch (_light->GetType()) {
        case ELightComponentType::DIRECTIONAL: return 1;
        case ELightComponentType::ENVIRONMENT: return 2;
        default: return 0;
    }
}

static inline uint FloatToUInt(float _v, float _scale) { return (uint)floor(_v * _scale + 0.5f); }

static inline float Saturate(float _v) { return std::clamp(_v, 0.f, 1.f); }

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
    if (max_radiance < 0.f) return;

    float log_radiance = (std::log2f(max_radiance) - g_poly_morphic_light_min_log2_radiance) /
                         (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance);
    log_radiance           = std::clamp(log_radiance, 0.f, 1.f);
    uint packed_radiance   = std::min(uint(ceilf(log_radiance * 65534.f)) + 1, 0xffffu);
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
static bool CanConvert(LightComponent& _light) {
    switch (_light.GetType()) {
        case Moer::ELightComponentType::DIRECTIONAL:
        case Moer::ELightComponentType::SPOT:
        case Moer::ELightComponentType::POINT:
        case Moer::ELightComponentType::ENVIRONMENT: return true;
        default: return false;
    }
}

static bool ConvertLight(LightComponent& _light, PolymorphicLightInfo& _info) {
    if (!CanConvert(_light)) return false;
    switch (_light.GetType()) {
        case Moer::ELightComponentType::DIRECTIONAL: {
            DirectionalLightComponent* dir_light = static_cast<DirectionalLightComponent*>(&_light);
            float  half_angluar_size_rad = Angle::DegreeToRadian(std::max(dir_light->GetAngularSize(), 0.1f));
            float  solid_angle           = 2 * PI * (1 - cos(half_angluar_size_rad));
            float3 radiance =
                dir_light->GetColor() * dir_light->GetIntensity() / std::max(solid_angle, 1e-6f);

            _info.color_type_flags = (uint)EPolyLightType::ELDirectional << g_poly_morphic_light_type_shift;
            PackPolyLightColor(radiance, _info);
            _info.direction1 = PackNormalizedVector(Normalizef(dir_light->GetDirection()));
            _info.scalars    = Fp32ToFp16(half_angluar_size_rad) | (Fp32ToFp16(solid_angle) << 16);
            break;
        }
        case Moer::ELightComponentType::SPOT: {
            SpotLightComponent* spot_light = static_cast<SpotLightComponent*>(&_light);
            float3              flux       = spot_light->GetColor() * spot_light->GetIntensity();
            float               softness =
                Saturate(1.f / (spot_light->GetOuterConeAngle() - spot_light->GetInnerConeAngle()));

            _info.color_type_flags = (uint)EPolyLightType::ELSphere << g_poly_morphic_light_type_shift;
            _info.color_type_flags |= g_poly_morphic_light_shaping_bit;
            PackPolyLightColor(flux, _info);
            _info.center       = spot_light->GetPosition();
            _info.primary_axis = PackNormalizedVector(Normalizef(spot_light->GetDirection()));
            _info.cos_cone_angle_softness =
                Fp32ToFp16(Angle::DegreeToRadian(spot_light->GetOuterConeAngle())) |
                (Fp32ToFp16(softness) << 16);
            break;
        }
        case Moer::ELightComponentType::POINT: {
            PointLightComponent* point_light = static_cast<PointLightComponent*>(&_light);
            float3               flux        = point_light->GetColor() * point_light->GetIntensity();
            _info.color_type_flags = (uint)EPolyLightType::ELPoint << g_poly_morphic_light_type_shift;
            PackPolyLightColor(flux, _info);
            _info.center = point_light->GetPosition();
            break;
        }
        case Moer::ELightComponentType::ENVIRONMENT: {
            EnvironmentLightComponent* env_light = static_cast<EnvironmentLightComponent*>(&_light);
            if (!env_light->bdls_handle) return false;
            _info.color_type_flags = (uint)EPolyLightType::ELEnv << g_poly_morphic_light_type_shift;
            PackPolyLightColor(env_light->GetColorScale(), _info);
            _info.direction1 = env_light->bdls_handle;
            _info.direction2 = env_light->size.x | (env_light->size.y << 16);
            _info.scalars    = Fp32ToFp16(env_light->rotation);
            _info.scalars |= g_poly_morphic_light_env_is_scalar_bit;
            break;
        }
        default: {
            LOG_INFO("Light Type {} not supported", uint(_light.GetType()));
            return false;
        }
    }
    return true;
}

void PrepareLightPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {

    Array<PrepareLightsTask>    tasks;
    Array<PolymorphicLightInfo> prim_light_infos;

    uint light_buf_offset = 0;

    Array<uint> geo_instance_to_light(scene.GetGeometryInstances().size(), s_invalid_light_idx);

    uint idx = 0;

    uint num_prim_lights = 0;
    scene.ForEach([&](Entity _entity) {
        {
            const MeshInfo&                mesh_info   = *RenderableManager::Get().GetMeshInfo(_entity).get();
            uint                           instance_id = RenderableManager::Get().GetInstanceID(_entity);
            std::span<MaterialInstanceRef> mat_instances =
                RenderableManager::Get().GetMaterialInstances(_entity);

            for (uint i = 0; i < mesh_info.geometries.size(); ++i) {
                const MaterialInstanceRef& mat_instance = mat_instances[i];
                const MeshGeometry&        geom_info    = *mesh_info.geometries[i];
                float3                     emissive = mat_instance->GetParameter<float3>("emissive_factor");
                uint64                     instance_geo_hash = 0;
                HashCombine(instance_geo_hash, uint64(_entity.getId()), i);

                if (emissive == float3(0.f)) {
                    instance_light_buffer_offsets.erase(instance_geo_hash);
                    continue;
                }
                geo_instance_to_light[scene.GetInstanceDatas()[instance_id].first_geom_instance_idx + i] =
                    light_buf_offset;
                // scene.GetInstanceDatas()[instance_id].first_geom_instance_idx

                auto iter = instance_light_buffer_offsets.find(instance_geo_hash);

                PrepareLightsTask task{};
                task.instance_geo_idx  = (instance_id << 12 | uint(i & 0xfff));
                task.light_offset      = light_buf_offset;
                task.num_triangles     = geom_info.local_idx_count / 3;
                task.prev_light_offset = iter == instance_light_buffer_offsets.end() ? -1 : iter->second;

                instance_light_buffer_offsets[instance_geo_hash] = light_buf_offset;

                light_buf_offset += task.num_triangles;
                tasks.emplace_back(task);
            }
        }
        idx++;
    });

    Timer timer;

    std::span<const Entity> light_entities_src = scene.GetLights();

    // LOG_INFO("PrepareLightPass::Process, sort time:{}",
    // timer.ElapsedMilliseconds());
    timer.Start();
    num_prim_lights               = light_buf_offset;
    uint num_finite_prim_lights   = 0;
    uint num_infinite_prim_lights = 0;
    uint num_is_env_lights        = 0;

    uint chunk_size    = 1024;
    uint parrallel_cnt = std::max(_rt_ctx.num_threads, 1u);
    parrallel_cnt      = std::min(parrallel_cnt, (uint)light_entities_src.size() / chunk_size + 1);
    if (_rt_ctx.b_parallel_process_light) {
        Array<CachelineStruct<uint2>> light_cnt(
            parrallel_cnt,
            CachelineStruct<uint2>(uint2(0.f))
        ); // normal light, inf light

        Array<StaticArray<Array<CachelineStruct<uint>>, 2>> entity_idx_parrallel(parrallel_cnt);
        Array<UnorderedMap<uint64, uint>>                   prev_light_buffer_offsets_parallel(parrallel_cnt);
        for (uint i = 0; i < parrallel_cnt; i++) {
            // reserve space for normal lights
            entity_idx_parrallel[i][0].reserve(light_entities_src.size() / parrallel_cnt);
        }

        ParallelFor(parrallel_cnt, [&](uint _idx) {
            uint region_start = _idx * light_entities_src.size() / parrallel_cnt;
            uint region_end =
                std::min((_idx + 1) * light_entities_src.size() / parrallel_cnt, light_entities_src.size());
            uint norm_light_cnt = 0;
            uint inf_light_cnt  = 0;
            for (uint i = region_start; i < region_end; i++) {
                Entity            entity     = light_entities_src[i];
                LightComponentRef light_data = LightComponentManager::Get().Get(entity);

                PolymorphicLightInfo light_info{};

                if (!ConvertLight(*light_data, light_info)) { continue; }

                if (light_data->GetType() == ELightComponentType::DIRECTIONAL) {
                    inf_light_cnt++;

                } else if (light_data->GetType() == ELightComponentType::ENVIRONMENT) {
                    continue;
                } else {
                    norm_light_cnt++;
                }

                switch (light_data->GetType()) {
                    case ELightComponentType::DIRECTIONAL: {
                        // tasks_parrallel[_idx][1].emplace_back(task);
                        // prim_light_infos_parrallel[_idx][1].emplace_back(light_info);
                        entity_idx_parrallel[_idx][1].emplace_back(i);
                        break;
                    }
                    case ELightComponentType::ENVIRONMENT: {
                        break;
                    }
                    default: {
                        // tasks_parrallel[_idx][0].emplace_back(task);
                        // prim_light_infos_parrallel[_idx][0].emplace_back(light_info);
                        entity_idx_parrallel[_idx][0].emplace_back(i);
                        break;
                    }
                }
            }
            light_cnt[_idx].data.x = norm_light_cnt;
            light_cnt[_idx].data.y = inf_light_cnt;
        });

        // calculate offsets
        for (uint i = 1; i < parrallel_cnt; i++) { light_cnt[i].data.x += light_cnt[i - 1].data.x; }
        num_finite_prim_lights = light_cnt[parrallel_cnt - 1].data.x;

        light_cnt[0].data.y += light_cnt[parrallel_cnt - 1].data.x;
        for (uint i = 1; i < parrallel_cnt; i++) { light_cnt[i].data.y += light_cnt[i - 1].data.y; }
        num_infinite_prim_lights = light_cnt[parrallel_cnt - 1].data.y - num_finite_prim_lights;

        uint total_task_cnt = light_cnt[parrallel_cnt - 1].data.y;

        uint task_offset = tasks.size();
        tasks.resize(total_task_cnt + task_offset);
        tasks.reserve(total_task_cnt + task_offset + 1);
        prim_light_infos.resize(total_task_cnt);
        prim_light_infos.reserve(total_task_cnt + 1);
        timer.Stop();
        float sort_time = timer.ElapsedMilliseconds();
        timer.Start();
        ParallelFor(parrallel_cnt, [&](uint _idx) {
            for (uint light_type = 0; light_type < 2; light_type++) {
                if (entity_idx_parrallel[_idx][light_type].size() > 0) {
                    uint cur_offset =
                        light_cnt[_idx].data[light_type] - entity_idx_parrallel[_idx][light_type].size();
                    uint cur_task_offset = light_cnt[_idx].data[light_type] -
                                           entity_idx_parrallel[_idx][light_type].size() + task_offset;
                    uint total_light_offset = cur_offset + light_buf_offset;
                    for (uint i = 0; i < entity_idx_parrallel[_idx][light_type].size(); i++) {
                        PrepareLightsTask task{};
                        task.light_offset     = total_light_offset + i;
                        task.instance_geo_idx = (cur_offset + i) | g_task_prim_light_bit;
                        task.num_triangles    = 1;
                        Entity entity = light_entities_src[entity_idx_parrallel[_idx][light_type][i].data];
                        auto   light_data = LightComponentManager::Get().Get(entity);

                        uint64 key  = uint64(light_data.Get());
                        auto   iter = primitive_light_buffer_offsets.find(key);
                        task.prev_light_offset =
                            iter == primitive_light_buffer_offsets.end() ? -1 : iter->second;
                        tasks[cur_task_offset + i]                    = task;
                        prev_light_buffer_offsets_parallel[_idx][key] = total_light_offset + i;
                    }

                    for (uint i = 0; i < entity_idx_parrallel[_idx][light_type].size(); i++) {
                        PolymorphicLightInfo& light_info = prim_light_infos[cur_offset + i];
                        if (!ConvertLight(
                                *LightComponentManager::Get().Get(
                                    light_entities_src[entity_idx_parrallel[_idx][light_type][i].data]
                                ),
                                light_info
                            )) {
                            assert(false && "ConvertLight");
                        }
                    }
                }
            }
        });

        timer.Stop();
        float convert_time = timer.ElapsedMilliseconds();

        // merge prev light buffer offsets
        for (uint i = 0; i < parrallel_cnt; i++) {
            primitive_light_buffer_offsets.insert(
                prev_light_buffer_offsets_parallel[i].begin(), prev_light_buffer_offsets_parallel[i].end()
            );
        }

        light_buf_offset += light_cnt[parrallel_cnt - 1].data.y;

        if (auto cur_env = this->scene.GetCurrentEnvMap(); cur_env.texture) {
            auto                 env_comp = LightComponentManager::Get().Get(cur_env.entity);
            PolymorphicLightInfo light_info{};
            auto                 pre_iter = primitive_light_buffer_offsets.find(uint64(env_comp.Get()));
            if (ConvertLight(*env_comp, light_info)) {
                PrepareLightsTask task{};
                task.instance_geo_idx = prim_light_infos.size() | g_task_prim_light_bit;
                task.light_offset     = light_buf_offset;
                task.num_triangles    = 1;
                task.prev_light_offset =
                    pre_iter == primitive_light_buffer_offsets.end() ? -1 : pre_iter->second;
                primitive_light_buffer_offsets[uint64(env_comp.Get())] = light_buf_offset;
                light_buf_offset += task.num_triangles;
                tasks.emplace_back(task);
                prim_light_infos.emplace_back(light_info);

                num_is_env_lights++;
            }
        }
    } else {
        Array<Entity> light_entities(light_entities_src.begin(), light_entities_src.end());
        std::ranges::sort(light_entities, [&](Entity _lhs, Entity _rhs) {
            return LightPriority(LightComponentManager::Get().Get(_lhs)) <
                   LightPriority(LightComponentManager::Get().Get(_rhs));
        });

        std::span<Entity> view = light_entities;

        for (auto entity : view) {
            LightComponentRef light_data = LightComponentManager::Get().Get(entity);

            PolymorphicLightInfo light_info{};

            if (!ConvertLight(*light_data, light_info)) { continue; }

            auto pre_iter = primitive_light_buffer_offsets.find(uint64(light_data.Get()));

            PrepareLightsTask task{};
            task.instance_geo_idx  = prim_light_infos.size() | g_task_prim_light_bit;
            task.light_offset      = light_buf_offset;
            task.num_triangles     = 1;
            task.prev_light_offset = pre_iter == primitive_light_buffer_offsets.end() ? -1 : pre_iter->second;

            primitive_light_buffer_offsets[uint64(light_data.Get())] = light_buf_offset;
            light_buf_offset += task.num_triangles;
            tasks.emplace_back(task);
            prim_light_infos.emplace_back(light_info);

            if (light_data->GetType() == ELightComponentType::DIRECTIONAL) {
                num_infinite_prim_lights++;
            } else if (light_data->GetType() == ELightComponentType::ENVIRONMENT) {
                num_is_env_lights++;
            } else {
                num_finite_prim_lights++;
            }
        }
    }
    timer.Stop();
    auto time = timer.ElapsedMilliseconds();
    // LOG_INFO("PrepareLightPass::Process, convert time:{}",
    // timer.ElapsedMilliseconds());
    _cmd_list.PushScope("PrepareLights");
    _cmd_list.CopyFrom(
        std::span<byte>((byte*)geo_instance_to_light.data(), geo_instance_to_light.size() * sizeof(uint)),
        _rt_ctx.geo_instance_to_light_buf->GetView()
    );
    _cmd_list.CopyFrom(
        std::span<byte>((byte*)tasks.data(), tasks.size() * sizeof(PrepareLightsTask)),
        _rt_ctx.task_buf->GetView(0, tasks.size() * sizeof(PrepareLightsTask))
    );
    if (!prim_light_infos.empty()) {
        _cmd_list.CopyFrom(
            std::span<byte>(
                (byte*)prim_light_infos.data(), prim_light_infos.size() * sizeof(PolymorphicLightInfo)
            ),
            _rt_ctx.prim_light_buf->GetView()
        );
    }

    _cmd_list.ClearResource(_rt_ctx.light_mapping_buf->GetView(), 0u);
    _cmd_list.ClearResource(
        _rt_ctx.local_light_pdf_tex->GetView(0, _rt_ctx.local_light_pdf_tex->GetNumMips()), float4(0.f)
    );

    PrepareLightsParams param{};
    param.num_tasks            = tasks.size();
    param.geometry_data_handle = _rt_ctx.GetBindlessHandles().geom_data;
    param.instance_data_handle = _rt_ctx.GetBindlessHandles().instance_data;
    param.material_data_handle = _rt_ctx.GetBindlessHandles().material_data;

    uint max_lights_in_buffer = uint(_rt_ctx.light_data_buf->GetNumElement() / 2);
    param.cur_light_offset    = max_lights_in_buffer * b_odd_frame;
    param.prev_light_offset   = max_lights_in_buffer * !b_odd_frame;

    // test
    param.geom_inst_to_light = _rt_ctx.GetBindlessHandles().geo_instance_to_light;

    _rt_ctx.is_ctx.SetLightBufferParams(
        param.cur_light_offset,
        num_finite_prim_lights + num_prim_lights,
        num_infinite_prim_lights,
        num_is_env_lights
    );

    _cmd_list
        .Compute(
            prepare_light_pipeline,
            param,
            _rt_ctx.light_data_buf->GetView(),
            _rt_ctx.light_mapping_buf->GetView(),
            _rt_ctx.local_light_pdf_tex->GetView(),
            _rt_ctx.prim_light_buf->GetView(),
            _rt_ctx.task_buf->GetView(),
            scene.GetBindlessArray()
        )
        .Dispatch(uint3((light_buf_offset + 255) / 256, 1, 1), "PrepareLights");

    _rt_ctx.sd_utils.GenerateMips(_cmd_list, _rt_ctx.local_light_pdf_mips);
    // device.GetCommandQueue(EQueueType::Graphics).Execute(_cmd_list.Submit());
    // device.GetCommandQueue(EQueueType::Graphics).Sync();
    // uint num_lights = num_finite_prim_lights + num_infinite_prim_lights +
    // num_is_env_lights;
    _cmd_list.AddCallback([geo_instance_to_light(std::move(geo_instance_to_light)),
                           prim_light_infos(std::move(prim_light_infos)),
                           tasks(std::move(tasks))]() {

    });
    _cmd_list.PopScope();
    b_odd_frame = !b_odd_frame;
}

} // namespace Moer::Render::Raytracing