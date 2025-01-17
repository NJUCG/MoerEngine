#include "PreprocessLightPass.h"
#include "misc/Hash.h"
#include "misc/STL.h"
#include "scene/MaterialInstance.h"
#include "scene/RenderableManager.h"
#include "scene/light/DirectionalLightComponent.h"
#include "scene/light/LightComponent.h"
#include "scene/light/LightComponentManager.h"
#include "scene/light/PointLightComponent.h"
#include "scene/light/SpotLightComponent.h"
#include "shader/ShaderResourceManager.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/Scene.h"
#include <algorithm>
#include <cmath>
namespace Moer::Render {

    PrepareLightPass::PrepareLightPass(RenderDevice&  _device,
                                       ShaderManager& _manager,
                                       Scene&         _scene) : device(_device),
                                                        manager(_manager),
                                                        scene(_scene),
                                                        prepare_light_pipeline(ShaderManager::Get().Compute<PrepareLightShaderPipeline>("lighting/PrepareLights.hlsl")) {
    }

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
            case ELightComponentType::ENV: return 2;
            default: return 0;
        }
    }

    static inline uint FloatToUInt(float _v, float _scale) {
        return (uint)floor(_v * _scale + 0.5f);
    }

    static inline float Saturate(float _v) {
        return std::clamp(_v, 0.f, 1.f);
    }

    static inline uint FloaT3ToR8G8B8Unorm(float _unpacked_input_x, float _unpacked_input_y, float _unpacked_input_z) {
        return (FloatToUInt(Saturate(_unpacked_input_x), 0xFF) & 0xFF) |
               ((FloatToUInt(Saturate(_unpacked_input_y), 0xFF) & 0xFF) << 8) |
               ((FloatToUInt(Saturate(_unpacked_input_z), 0xFF) & 0xFF) << 16);
    }

    static uint16_t Fp32ToFp16(float _v) {
        // Multiplying by 2^-112 causes exponents below -14 to denormalize
        static const union FU {
            uint  ui;
            float f;
        } multiple = {0x07800000};// 2**-112 0(1) 15(8) 0(23)

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

        float log_radiance     = (std::log2f(max_radiance) - g_poly_morphic_light_min_log2_radiance) / (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance);
        log_radiance           = std::clamp(log_radiance, 0.f, 1.f);
        uint packed_radiance   = std::min(uint(ceilf(log_radiance * 65534.f)) + 1, 0xffffu);
        uint unpacked_radiance = std::exp2f((float(packed_radiance - 1) / 65534.f) * (g_poly_morphic_light_max_log2_radiance - g_poly_morphic_light_min_log2_radiance) + g_poly_morphic_light_min_log2_radiance);

        _info.color_type_flags |= FloaT3ToR8G8B8Unorm(_color.x / unpacked_radiance, _color.y / unpacked_radiance, _color.z / unpacked_radiance);
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

    static bool ConvertLight(LightComponent& _light, PolymorphicLightInfo& _info) {
        switch (_light.GetType()) {
            case Moer::ELightComponentType::DIRECTIONAL: {
                DirectionalLightComponent* dir_light             = static_cast<DirectionalLightComponent*>(&_light);
                float                      half_angluar_size_rad = Angle::DegreeToRadian(dir_light->angluar_size);
                float                      solid_angle           = std::max(2 * PI * (1 - cos(half_angluar_size_rad)), 0.001f);
                float3                     radiance              = dir_light->GetColor() * dir_light->GetIntensity() / std::max(solid_angle, 1e-6f);

                _info.color_type_flags = (uint)EPolyLightType::ELDirectional << g_poly_morphic_light_type_shift;
                PackPolyLightColor(radiance, _info);
                _info.direction1 = PackNormalizedVector(Normalizef(dir_light->GetDirection()));
                _info.scalars    = Fp32ToFp16(half_angluar_size_rad) | (Fp32ToFp16(solid_angle) << 16);
                return true;
                break;
            }
            case Moer::ELightComponentType::SPOT: {
                SpotLightComponent* spot_light = static_cast<SpotLightComponent*>(&_light);
                float3              flux       = spot_light->GetColor() * spot_light->GetIntensity();
                float               softness   = Saturate(1.f / (spot_light->GetOuterConeAngle() - spot_light->GetInnerConeAngle()));

                _info.color_type_flags = (uint)EPolyLightType::ELSphere << g_poly_morphic_light_type_shift;
                _info.color_type_flags |= g_poly_morphic_light_shaping_bit;
                PackPolyLightColor(flux, _info);
                _info.center                  = spot_light->GetPosition();
                _info.primary_axis            = PackNormalizedVector(Normalizef(spot_light->GetDirection()));
                _info.cos_cone_angle_softness = Fp32ToFp16(Angle::DegreeToRadian(spot_light->GetOuterConeAngle())) | (Fp32ToFp16(softness) << 16);
                return true;
                break;
            }
            case Moer::ELightComponentType::POINT: {
                PointLightComponent* point_light = static_cast<PointLightComponent*>(&_light);
                float3               flux        = point_light->GetColor() * point_light->GetIntensity();
                _info.color_type_flags           = (uint)EPolyLightType::ELSphere << g_poly_morphic_light_type_shift;
                PackPolyLightColor(flux, _info);
                _info.center = point_light->GetPosition();
                return true;
                break;
            }
            case Moer::ELightComponentType::ENV: {
                EnvironmentLightComponent* env_light = static_cast<EnvironmentLightComponent*>(&_light);
                if (!env_light->bdls_handle) return false;
                _info.color_type_flags = (uint)EPolyLightType::ELEnv << g_poly_morphic_light_type_shift;
                PackPolyLightColor(env_light->GetColorScale(), _info);
                _info.direction1 = env_light->bdls_handle;
                _info.direction2 = env_light->size.x | (env_light->size.y << 16);
                _info.scalars    = Fp32ToFp16(env_light->rotation);
                _info.scalars |= g_poly_morphic_light_env_is_scalar_bit;
                return true;
                break;
            }
            default: {
                return false;
            }
        }
    }

    void PrepareLightPass::Process(CommandList& _cmd_list, RTContext& _rt_ctx) {

        Array<PrepareLightsTask>    tasks;
        Array<PolymorphicLightInfo> prim_light_infos;

        uint light_buf_offset = 0;

        Array<uint> geo_instance_to_light(scene.GetEntityCount());

        uint idx = 0;

        uint num_prim_lights = 0;
        scene.ForEach([&](Entity _entity) {
            {
                const MeshInfo& mesh_info   = *RenderableManager::Get().GetMeshInfo(_entity).get();
                uint            instance_id = RenderableManager::Get().GetInstanceID(_entity);
                std::span<MaterialInstanceRef>
                    mat_instances = RenderableManager::Get().GetMaterialInstances(_entity);

                for (uint i = 0; i < mesh_info.geometries.size(); ++i) {
                    const MaterialInstanceRef& mat_instance      = mat_instances[i];
                    const MeshGeometry&        geom_info         = *mesh_info.geometries[i];
                    float3                     emissive          = mat_instance->GetParameter<float3>("emissive_factor");
                    uint64                     instance_geo_hash = 0;
                    HashCombine(instance_geo_hash, &_entity, i);

                    if (emissive == float3(0.f)) {
                        instance_light_buffer_offsets.erase(instance_geo_hash);
                        continue;
                    }
                    geo_instance_to_light[idx] = light_buf_offset;

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

        auto light_entities = scene.GetLights();
        for (auto& entity : light_entities) {
            LightComponentRef light_data = LightComponentManager::Get().Get(entity);
        }
        std::sort(light_entities.begin(), light_entities.end(), [](Entity _lhs, Entity _rhs) {
            return LightPriority(LightComponentManager::Get().Get(_lhs)) < LightPriority(LightComponentManager::Get().Get(_rhs));
        });
        num_prim_lights               = light_buf_offset;
        uint num_finite_prim_lights   = 0;
        uint num_infinite_prim_lights = 0;
        uint num_is_env_lights        = 0;

        for (auto& entity : light_entities) {
            LightComponentRef light_data = LightComponentManager::Get().Get(entity);

            PolymorphicLightInfo light_info{};

            if (!ConvertLight(*light_data, light_info)) {
                continue;
            }

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
            } else if (light_data->GetType() == ELightComponentType::ENV) {
                num_is_env_lights++;
            } else {
                num_finite_prim_lights++;
            }
        }

        _cmd_list.CopyFrom(std::span<byte>((byte*)geo_instance_to_light.data(), geo_instance_to_light.size() * sizeof(uint)), _rt_ctx.geo_instance_to_light_buf->GetView());
        _cmd_list.CopyFrom(std::span<byte>((byte*)tasks.data(), tasks.size() * sizeof(PrepareLightsTask)), _rt_ctx.task_buf->GetView(0, tasks.size() * sizeof(PrepareLightsTask)));
        if (!prim_light_infos.empty()) {
            _cmd_list.CopyFrom(std::span<byte>((byte*)prim_light_infos.data(), prim_light_infos.size() * sizeof(PolymorphicLightInfo)), _rt_ctx.prim_light_buf->GetView());
        }

        _cmd_list.ClearResource(_rt_ctx.light_mapping_buf->GetView(), 0u);
        _cmd_list.ClearResource(_rt_ctx.local_light_pdf_tex->GetView(0, _rt_ctx.local_light_pdf_tex->GetNumMips()), float4(0.f));

        PrepareLightsParams param{};
        param.num_tasks            = tasks.size();
        param.geometry_data_handle = _rt_ctx.GetBindlessHandles().geom_data;
        param.instance_data_handle = _rt_ctx.GetBindlessHandles().instance_data;
        param.material_data_handle = _rt_ctx.GetBindlessHandles().material_data;

        uint max_lights_in_buffer = uint(_rt_ctx.light_data_buf->GetNumElement() / 2);
        param.cur_light_offset    = max_lights_in_buffer * b_odd_frame;
        param.prev_light_offset   = max_lights_in_buffer * !b_odd_frame;

        _rt_ctx.is_ctx.SetLightBufferParams(
            param.cur_light_offset,
            num_finite_prim_lights + num_prim_lights,
            num_infinite_prim_lights,
            num_is_env_lights);

        _cmd_list.Compute(prepare_light_pipeline, param, _rt_ctx.light_data_buf->GetView(), _rt_ctx.light_mapping_buf->GetView(), _rt_ctx.local_light_pdf_tex->GetView(), _rt_ctx.prim_light_buf->GetView(), _rt_ctx.task_buf->GetView(), scene.GetBindlessArray())
            .Dispatch(uint3((light_buf_offset + 255) / 256, 1, 1), "PrepareLights");

        _rt_ctx.sd_utils.GenerateMips(_cmd_list, _rt_ctx.local_light_pdf_mips);
        // device.GetCommandQueue(EQueueType::Graphics).Execute(_cmd_list.Submit());
        // device.GetCommandQueue(EQueueType::Graphics).Sync();
        // uint num_lights = num_finite_prim_lights + num_infinite_prim_lights + num_is_env_lights;
        _cmd_list.AddCallback([geo_instance_to_light(std::move(geo_instance_to_light)),
                               prim_light_infos(std::move(prim_light_infos)),
                               tasks(std::move(tasks))]() {

        });
        b_odd_frame = !b_odd_frame;
    }

}// namespace Moer::Render