#include "PreprocessLightPass.h"

#include "misc/STL.h"
#include "misc/Timer.h"
#include "platform/Platform.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/Scene.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"

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
    CachelineStruct() noexcept {
        memset(padding, 0, sizeof(padding));
    }
    CachelineStruct(const T& _data) : data(_data) {}
    CachelineStruct(const CachelineStruct& _rhs) : data(_rhs.data) {}
    CachelineStruct& operator=(const CachelineStruct& _rhs) {
        data = _rhs.data;
        return *this;
    }

    operator T&() {
        return data;
    }
    operator const T&() const {
        return data;
    }
};

PrepareLightPass::PrepareLightPass(RenderDevice& _device, ShaderManager& _manager, Scene& _scene) :
    device(_device),
    manager(_manager),
    scene(_scene),
    prepare_light_pipeline(
        ShaderManager::Get().Compute<PrepareLightShaderPipeline>(
            "pipelines/raytracing/lighting/precompute/PrepareLights.hlsl"
        )
    ) {}

void PrepareLightPass::CountEmissiveInstances(uint& _num_emissive_meshes, uint& _num_emissive_triangles) {
    _num_emissive_meshes    = 0;
    _num_emissive_triangles = 0;

    auto& r = scene.r();

    // 遍历所有有 CRenderable 的 entity
    r.view<const ecs::CRenderable>().each([&](const auto entity, const ecs::CRenderable& c_renderable) {
        // 获取 CMesh
        if (!r.valid(c_renderable.mesh_entt) || !r.all_of<ecs::CMesh>(c_renderable.mesh_entt)) {
            return; // Skip invalid mesh
        }

        const ecs::CMesh& c_mesh = r.get<ecs::CMesh>(c_renderable.mesh_entt);

        // 遍历该 Mesh 的所有 Primitive
        for (const entt::entity primitive_entt : c_mesh.primitive_entts) {
            if (!r.valid(primitive_entt) || !r.all_of<ecs::CPrimitive>(primitive_entt)) {
                continue; // Skip invalid primitive
            }

            const ecs::CPrimitive& c_primitive = r.get<ecs::CPrimitive>(primitive_entt);

            // 获取材质
            if (!r.valid(c_primitive.material_entt) || !r.all_of<ecs::CMaterial>(c_primitive.material_entt)) {
                continue; // Skip invalid material
            }

            const ecs::CMaterial& c_material = r.get<ecs::CMaterial>(c_primitive.material_entt);

            // 检查 emissive_factor 是否为非零
            if (c_material.emissive_factor != float3(0.f)) {
                _num_emissive_meshes++;
                _num_emissive_triangles += c_primitive.index_count / 3;
            }
        }
    });
}

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
            return r.all_of<ecs::CLightDirectional>(entity);
        case Moer::ELightType::Point:
            return r.all_of<ecs::CLightPoint>(entity);
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

            // 0.533度是太阳的视角直径（默认值，因为新系统中没有 angular_size 字段）
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

            _info.direction1 = PackNormalizedVector(Normalizef(c_light_dir.d_direction));
            _info.scalars    = Fp32ToFp16(half_angular_size_rad) | (Fp32ToFp16(solid_angle) << 16);
            break;
        }
        case Moer::ELightType::Spot: {
            // TODO: CLightSpot 还未实现
            LOG_INFO("Spot light not yet supported in new ECS system");
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

            _info.center = c_light_point.d_position;
            break;
        }
        case Moer::ELightType::Environment: {
            // TODO: CLightEnvironment 还未实现
            LOG_INFO("Environment light not yet supported in new ECS system");
            return false;
        }
        default: {
            LOG_INFO("Light Type {} not supported", uint(c_light.type));
            return false;
        }
    }
    return true;
}

void PrepareLightPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {
    Array<PrepareLightsTask>    tasks;
    Array<PolymorphicLightInfo> prim_light_infos;

    uint light_buf_offset = 0;

    // 1. 遍历所有 CPrimitive，处理自发光三角形
    UnorderedMap<uint, uint> primitive_to_light_offset_map;

    auto& r = scene.r();
    r.view<const ecs::CPrimitive>().each([&](const auto primitive_entt, const ecs::CPrimitive& c_primitive) {
        // 获取材质
        if (!r.valid(c_primitive.material_entt) || !r.all_of<ecs::CMaterial>(c_primitive.material_entt)) {
            return;
        }

        const ecs::CMaterial& c_material = r.get<ecs::CMaterial>(c_primitive.material_entt);

        // 检查自发光（对应原始代码的 emissive_factor 检查）
        if (c_material.emissive_factor == float3(0.f)) {
            uint primitive_id = scene.GetCpuScene().GetPrimitiveId(primitive_entt);
            if (primitive_id != UINT_MAX) {
                primitive_to_light_offset_map.erase(primitive_id);
            }
            return;
        }

        // 获取 primitive_id
        uint primitive_id = scene.GetCpuScene().GetPrimitiveId(primitive_entt);
        if (primitive_id == UINT_MAX) {
            LOG_ERROR("Primitive entity {} not found in CpuScene", entt::to_integral(primitive_entt));
            return;
        }

        // 检查是否已经处理过
        uint64 primitive_hash = static_cast<uint64>(primitive_id);
        auto   iter           = instance_light_buffer_offsets.find(primitive_hash);

        // 创建 Task
        PrepareLightsTask task{};
        task.primitive_id      = primitive_id;
        task.light_offset      = light_buf_offset;
        task.num_triangles     = c_primitive.index_count / 3;
        task.prev_light_offset = iter == instance_light_buffer_offsets.end() ? -1 : iter->second;

        task.index_start_idx    = c_primitive.index.is_valid ? c_primitive.index.start_idx : 0;
        task.first_instance_idx = scene.GetCpuScene().GetFirstInstanceIndex(primitive_id);

        // 记录映射关系（对应原始代码的 geo_instance_to_light[first_geom_instance_idx + i] = light_buf_offset）
        primitive_to_light_offset_map[primitive_id]   = light_buf_offset;
        instance_light_buffer_offsets[primitive_hash] = light_buf_offset;

        light_buf_offset += task.num_triangles;
        tasks.emplace_back(task);
    });

    uint num_prim_lights = light_buf_offset; // 记录自发光三角形的数量

    // 2. 将 primitive_to_light_offset_map 转换为数组

    uint max_primitive_id = 0;
    if (!primitive_to_light_offset_map.empty()) {
        for (const auto& [primitive_id, light_offset] : primitive_to_light_offset_map) {
            max_primitive_id = std::max(max_primitive_id, primitive_id);
        }
    }
    uint num_primitives = scene.GetCpuScene().GetPrimitiveCount();
    max_primitive_id    = std::max(max_primitive_id, num_primitives > 0 ? num_primitives - 1 : 0);

    // 创建数组（primitive_id -> light_offset 映射）
    Array<uint> primitive_to_light(max_primitive_id + 1, s_invalid_light_idx);
    for (const auto& [primitive_id, light_offset] : primitive_to_light_offset_map) {
        if (primitive_id <= max_primitive_id) {
            primitive_to_light[primitive_id] = light_offset;
        }
    }

    // 3. 处理场景光源（Directional Light, Point Light 等）

    Timer timer;
    timer.Start();

    uint num_finite_prim_lights   = 0;
    uint num_infinite_prim_lights = 0;
    uint num_is_env_lights        = 0;

    auto                light_view = scene.r().view<const ecs::CLight>();
    Array<entt::entity> light_entities(light_view.begin(), light_view.end());

    // 下面注释掉的这一段代码，是原来的多线程处理光线的代码
    // - 场景管理在重构后，目前没有时间进行迁移，所以保留此处的注释
    {



    // uint chunk_size    = 1024;
    // uint parrallel_cnt = std::max(_rt_ctx.num_threads, 1u);
    // parrallel_cnt      = std::min(parrallel_cnt, (uint)light_entities_src.size() / chunk_size + 1);
    // if (_rt_ctx.b_parallel_process_light) {
    //     Array<CachelineStruct<uint2>> light_cnt(
    //         parrallel_cnt,
    //         CachelineStruct<uint2>(uint2(0.f))
    //     ); // normal light, inf light

    //     Array<StaticArray<Array<CachelineStruct<uint>>, 2>> entity_idx_parrallel(parrallel_cnt);
    //     Array<UnorderedMap<uint64, uint>>                   prev_light_buffer_offsets_parallel(parrallel_cnt);
    //     for (uint i = 0; i < parrallel_cnt; i++) {
    //         // reserve space for normal lights
    //         entity_idx_parrallel[i][0].reserve(light_entities_src.size() / parrallel_cnt);
    //     }

    //     ParallelFor(parrallel_cnt, [&](uint _idx) {
    //         uint region_start = _idx * light_entities_src.size() / parrallel_cnt;
    //         uint region_end =
    //             std::min((_idx + 1) * light_entities_src.size() / parrallel_cnt, light_entities_src.size());
    //         uint norm_light_cnt = 0;
    //         uint inf_light_cnt  = 0;
    //         for (uint i = region_start; i < region_end; i++) {
    //             Entity            entity     = light_entities_src[i];
    //             LightComponentRef light_data = LightComponentManager::Get().Get(entity);

    //             PolymorphicLightInfo light_info{};

    //             if (!ConvertLight(*light_data, light_info)) {
    //                 continue;
    //             }

    //             if (light_data->GetType() == ELightComponentType::DIRECTIONAL) {
    //                 inf_light_cnt++;

    //             } else if (light_data->GetType() == ELightComponentType::ENVIRONMENT) {
    //                 continue;
    //             } else {
    //                 norm_light_cnt++;
    //             }

    //             switch (light_data->GetType()) {
    //                 case ELightComponentType::DIRECTIONAL: {
    //                     // tasks_parrallel[_idx][1].emplace_back(task);
    //                     // prim_light_infos_parrallel[_idx][1].emplace_back(light_info);
    //                     entity_idx_parrallel[_idx][1].emplace_back(i);
    //                     break;
    //                 }
    //                 case ELightComponentType::ENVIRONMENT: {
    //                     break;
    //                 }
    //                 default: {
    //                     // tasks_parrallel[_idx][0].emplace_back(task);
    //                     // prim_light_infos_parrallel[_idx][0].emplace_back(light_info);
    //                     entity_idx_parrallel[_idx][0].emplace_back(i);
    //                     break;
    //                 }
    //             }
    //         }
    //         light_cnt[_idx].data.x = norm_light_cnt;
    //         light_cnt[_idx].data.y = inf_light_cnt;
    //     });

    //     // calculate offsets
    //     for (uint i = 1; i < parrallel_cnt; i++) {
    //         light_cnt[i].data.x += light_cnt[i - 1].data.x;
    //     }
    //     num_finite_prim_lights = light_cnt[parrallel_cnt - 1].data.x;

    //     light_cnt[0].data.y += light_cnt[parrallel_cnt - 1].data.x;
    //     for (uint i = 1; i < parrallel_cnt; i++) {
    //         light_cnt[i].data.y += light_cnt[i - 1].data.y;
    //     }
    //     num_infinite_prim_lights = light_cnt[parrallel_cnt - 1].data.y - num_finite_prim_lights;

    //     uint total_task_cnt = light_cnt[parrallel_cnt - 1].data.y;

    //     uint task_offset = tasks.size();
    //     tasks.resize(total_task_cnt + task_offset);
    //     tasks.reserve(total_task_cnt + task_offset + 1);
    //     prim_light_infos.resize(total_task_cnt);
    //     prim_light_infos.reserve(total_task_cnt + 1);
    //     timer.Stop();
    //     float sort_time = timer.ElapsedMilliseconds();
    //     timer.Start();
    //     ParallelFor(parrallel_cnt, [&](uint _idx) {
    //         for (uint light_type = 0; light_type < 2; light_type++) {
    //             if (entity_idx_parrallel[_idx][light_type].size() > 0) {
    //                 uint cur_offset =
    //                     light_cnt[_idx].data[light_type] - entity_idx_parrallel[_idx][light_type].size();
    //                 uint cur_task_offset = light_cnt[_idx].data[light_type] -
    //                                        entity_idx_parrallel[_idx][light_type].size() + task_offset;
    //                 uint total_light_offset = cur_offset + light_buf_offset;
    //                 for (uint i = 0; i < entity_idx_parrallel[_idx][light_type].size(); i++) {
    //                     PrepareLightsTask task{};
    //                     task.light_offset     = total_light_offset + i;
    //                     task.primitive_id = (cur_offset + i) | g_task_prim_light_bit;
    //                     task.num_triangles    = 1;
    //                     Entity entity = light_entities_src[entity_idx_parrallel[_idx][light_type][i].data];
    //                     auto   light_data = LightComponentManager::Get().Get(entity);

    //                     uint64 key  = uint64(light_data.Get());
    //                     auto   iter = primitive_light_buffer_offsets.find(key);
    //                     task.prev_light_offset =
    //                         iter == primitive_light_buffer_offsets.end() ? -1 : iter->second;
    //                     tasks[cur_task_offset + i]                    = task;
    //                     prev_light_buffer_offsets_parallel[_idx][key] = total_light_offset + i;
    //                 }

    //                 for (uint i = 0; i < entity_idx_parrallel[_idx][light_type].size(); i++) {
    //                     PolymorphicLightInfo& light_info = prim_light_infos[cur_offset + i];
    //                     if (!ConvertLight(
    //                             *LightComponentManager::Get().Get(
    //                                 light_entities_src[entity_idx_parrallel[_idx][light_type][i].data]
    //                             ),
    //                             light_info
    //                         )) {
    //                         assert(false && "ConvertLight");
    //                     }
    //                 }
    //             }
    //         }
    //     });

    //     timer.Stop();
    //     float convert_time = timer.ElapsedMilliseconds();

    //     // merge prev light buffer offsets
    //     for (uint i = 0; i < parrallel_cnt; i++) {
    //         primitive_light_buffer_offsets.insert(
    //             prev_light_buffer_offsets_parallel[i].begin(), prev_light_buffer_offsets_parallel[i].end()
    //         );
    //     }

    //     light_buf_offset += light_cnt[parrallel_cnt - 1].data.y;

    //     if (auto cur_env = this->scene.GetCurrentEnvMap(); cur_env.texture) {
    //         auto                 env_comp = LightComponentManager::Get().Get(cur_env.entity);
    //         PolymorphicLightInfo light_info{};
    //         auto                 pre_iter = primitive_light_buffer_offsets.find(uint64(env_comp.Get()));
    //         if (ConvertLight(*env_comp, light_info)) {
    //             PrepareLightsTask task{};
    //             task.primitive_id = prim_light_infos.size() | g_task_prim_light_bit;
    //             task.light_offset     = light_buf_offset;
    //             task.num_triangles    = 1;
    //             task.prev_light_offset =
    //                 pre_iter == primitive_light_buffer_offsets.end() ? -1 : pre_iter->second;
    //             primitive_light_buffer_offsets[uint64(env_comp.Get())] = light_buf_offset;
    //             light_buf_offset += task.num_triangles;
    //             tasks.emplace_back(task);
    //             prim_light_infos.emplace_back(light_info);

    //             num_is_env_lights++;
    //         }
    //     }
    // } else {
    //     Array<Entity> light_entities(light_entities_src.begin(), light_entities_src.end());
    //     std::ranges::sort(light_entities, [&](Entity _lhs, Entity _rhs) {
    //         return LightPriority(LightComponentManager::Get().Get(_lhs)) <
    //                LightPriority(LightComponentManager::Get().Get(_rhs));
    //     });

    //     std::span<Entity> view = light_entities;

    //     for (auto entity : view) {
    //         LightComponentRef light_data = LightComponentManager::Get().Get(entity);

    //         PolymorphicLightInfo light_info{};

    //         if (!ConvertLight(*light_data, light_info)) {
    //             continue;
    //         }

    //         auto pre_iter = primitive_light_buffer_offsets.find(uint64(light_data.Get()));

    //         PrepareLightsTask task{};
    //         task.primitive_id  = prim_light_infos.size() | g_task_prim_light_bit;
    //         task.light_offset      = light_buf_offset;
    //         task.num_triangles     = 1;
    //         task.prev_light_offset = pre_iter == primitive_light_buffer_offsets.end() ? -1 : pre_iter->second;

    //         primitive_light_buffer_offsets[uint64(light_data.Get())] = light_buf_offset;
    //         light_buf_offset += task.num_triangles;
    //         tasks.emplace_back(task);
    //         prim_light_infos.emplace_back(light_info);

    //         if (light_data->GetType() == ELightComponentType::DIRECTIONAL) {
    //             num_infinite_prim_lights++;
    //         } else if (light_data->GetType() == ELightComponentType::ENVIRONMENT) {
    //             num_is_env_lights++;
    //         } else {
    //             num_finite_prim_lights++;
    //         }
    //     }
    // }

    }

    std::ranges::sort(light_entities, [&](entt::entity _lhs, entt::entity _rhs) {
        return LightPriority(_lhs, scene) < LightPriority(_rhs, scene);
    });

    // 处理每个光源
    for (auto entity : light_entities) {
        PolymorphicLightInfo light_info{};

        // 转换光源
        if (!ConvertLight(entity, scene, light_info)) {
            continue;
        }

        // 查找上一帧的 offset
        auto pre_iter = primitive_light_buffer_offsets.find(uint64(entity));

        // 创建 Task
        PrepareLightsTask task{};
        task.primitive_id      = prim_light_infos.size() | g_task_prim_light_bit;
        task.light_offset      = light_buf_offset;
        task.num_triangles     = 1;
        task.prev_light_offset = pre_iter == primitive_light_buffer_offsets.end() ? -1 : pre_iter->second;

        primitive_light_buffer_offsets[uint64(entity)] = light_buf_offset;
        light_buf_offset += task.num_triangles;
        tasks.emplace_back(task);
        prim_light_infos.emplace_back(light_info);

        // 统计光源类型
        auto c_light = scene.r().get<ecs::CLight>(entity);
        switch (c_light.type) {
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

    // 处理环境光（直接从 RTContext 获取数据，绕过 ECS）
    // 注意：rt_ctx.env_map 未被赋值（是 renderer 局部变量），用 env_pdf_tex 获取尺寸
    if (_rt_ctx.scene_params.enable_env_map && _rt_ctx.env_pdf_tex) {
        PolymorphicLightInfo light_info{};
        light_info.color_type_flags = (uint)EPolyLightType::ELEnv << g_poly_morphic_light_type_shift;

        float3 color_scale = float3(_rt_ctx.scene_params.env_map_scale);
        PackPolyLightColor(color_scale, light_info);

        light_info.direction1 = _rt_ctx.scene_params.env_map_handle;
        uint3 env_extent      = _rt_ctx.env_pdf_tex->GetExtent(); // env_pdf_tex 与 env_map 同尺寸
        light_info.direction2 = env_extent.x | (env_extent.y << 16);
        light_info.scalars    = Fp32ToFp16(_rt_ctx.scene_params.env_map_rotation);
        light_info.scalars |= g_poly_morphic_light_env_is_scalar_bit;

        constexpr uint64 env_light_key = ~0ull; // 固定 key，用于跨帧追踪
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

    timer.Stop();
    auto time = timer.ElapsedMilliseconds();

    // 4. 上传数据到 GPU

    _cmd_list.PushScopeWithTimeScope("PrepareLights");

    // 上传 primitive_to_light
    if (!primitive_to_light.empty()) {
        _cmd_list.CopyFrom(
            std::span<byte>((byte*)primitive_to_light.data(), primitive_to_light.size() * sizeof(uint)),
            _rt_ctx.primitive_to_light_buf->GetView(),
            "Upload primitive_to_light"
        );
    }

    // 上传 tasks
    _cmd_list.CopyFrom(
        std::span<byte>((byte*)tasks.data(), tasks.size() * sizeof(PrepareLightsTask)),
        _rt_ctx.task_buf->GetView(0, tasks.size() * sizeof(PrepareLightsTask)),
        "Upload tasks"
    );

    // 上传 prim_light_infos
    if (!prim_light_infos.empty()) {
        _cmd_list.CopyFrom(
            std::span<byte>(
                (byte*)prim_light_infos.data(), prim_light_infos.size() * sizeof(PolymorphicLightInfo)
            ),
            _rt_ctx.prim_light_buf->GetView(),
            "Upload prim light infos"
        );
    }

    // 清除资源
    _cmd_list.ClearResource(_rt_ctx.light_mapping_buf->GetView(), 0u);
    _cmd_list.ClearResource(
        _rt_ctx.local_light_pdf_tex->GetView(0, _rt_ctx.local_light_pdf_tex->GetNumMips()), float4(0.f)
    );

    // 5. 设置 Shader 参数并 Dispatch

    PrepareLightsParams param{};
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

    // 保持数据在回调中存活（对应原始代码）
    _cmd_list.AddCallback([primitive_to_light(std::move(primitive_to_light)),
                           prim_light_infos(std::move(prim_light_infos)),
                           tasks(std::move(tasks))]() {
        // 保持数据在回调中存活，直到 GPU 完成使用
    });

    _cmd_list.PopScopeWithTimeScope();
    b_odd_frame = !b_odd_frame;
}

} // namespace Moer::Render::Raytracing