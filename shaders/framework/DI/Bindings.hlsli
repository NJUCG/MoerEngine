#ifndef DI_BINDINGS_HLSLI
#define DI_BINDINGS_HLSLI

#ifndef DI_BINDING_SLOT 
#define DI_BINDING_SLOT 0

#include <framework/DI/Reservoirs.hlsli>
#include <shared/Geometry.h>
#include <framework/Bindless.hlsl>
#include <shared/utils/MoerMath.hlsli>
#include <framework/Math.hlsli>
#include <framework/Common.hlsl>
#include <hwrt/GBufferUtils.hlsli>

[[vk::binding(0, DI_BINDING_SLOT)]] RaytracingAccelerationStructure tlas;
[[vk::binding(1, DI_BINDING_SLOT)]] RaytracingAccelerationStructure prev_tlas;

[[vk::binding(2, DI_BINDING_SLOT)]] ConstantBuffer<Moer::ResampleConstants> resample_params;
[[vk::binding(3, DI_BINDING_SLOT)]] RWStructuredBuffer<PackedDIReservoir> light_reservoirs;
[[vk::binding(4, DI_BINDING_SLOT)]] RWTexture2D<float4> rw_diffuse_lighting;
[[vk::binding(5, DI_BINDING_SLOT)]] RWTexture2D<float4> rw_specular_lighting;
[[vk::binding(6, DI_BINDING_SLOT)]] RWTexture2DArray<float4> rw_gradients;
[[vk::binding(7, DI_BINDING_SLOT)]] RWTexture2D<float2> rw_restir_luminance;

[[vk::binding(8, DI_BINDING_SLOT)]] RWTexture2D<float4> rw_diffuse_lighting_prev;

[[vk::binding(9, DI_BINDING_SLOT)]] RWBuffer<float2> rw_ris_buffer;
[[vk::binding(10, DI_BINDING_SLOT)]] RWBuffer<uint4> rw_ris_light_data_buffer;

BINDLESS_BINDINGS(3, 2, 4, 5)
#include <framework/Material.hlsl>
#include <framework/PolymorphicLight.hlsli>

namespace Moer{

    typedef Math::Rng::Hash RandomState;

    float3 DiffuseTerm(float3 _v, float3 _n, float3 _l, float _roughness){
        float nol = saturate(dot(_n, _l));
        float nov = saturate(dot(_n, _v));
        float voh = saturate(dot(_v, _l));
        
        //use lambert here to reduce the cost
        return STL::BRDF::DiffuseTerm_Lambert(_roughness, nol, nov, voh) * max(nol, 0.f);
    }

    float3 SpecularTerm(float3 _v, float3 _l, float3 _n, float _roughness, float3 _f0){
        float3 h = normalize(_v + _l);
        float noh = saturate(dot(_n, h));
        float nov = saturate(dot(_n, _v));
        float voh = saturate(dot(_v, h));
        float nol = saturate(dot(_n, _l));

        if(nol <= 0.f) return 0.f;

        float3 fresnel = STL::BRDF::FresnelTerm(_f0, voh);
        float ndf = STL::BRDF::DistributionTerm(_roughness, noh);
        float g = STL::BRDF::GeometryTermMod( _roughness, nol, nov, voh, noh )
        return fresnel * ndf * g * nol;
    }

    struct LightSample{
        float3 x;
        float3 n;
        float3 radiance;
        float solid_angle_pdf;
        EPolyLightType type;

        static LightSample EmptyLightSample(){
            LightSample s = (LightSample)0;
            return s;
        }

        bool IsAnalytic(){
            return type != EPolyLightType::ELTriangle && type != EPolyLightType::ELEnv;
        }

        float SolidAnglePdf(){
            return solid_angle_pdf;
        }



    };

    struct Surface{
        float3 x;
        float3 v;
        float3 v_z;
        float3 n;
        float3 diffuse_albedo;
        float3 specular_f0;
        float roughness;
        float diffuse_prob;

        float GetDiffuseProbability(){
            float diffuse_weight = STL::Color::Luminance(diffuse_albedo);
            float specular_weight = STL::Color::Luminance(STL::BRDF::FresnelTerm_Schlick(specular_f0, dot(v, n)));
            float sum_weight = diffuse_weight + specular_weight;
            return sum_weight < 1e-6f ? 1.f : diffuse_weight / sum_weight;
        }

        bool IsValid(){
            return v_z != FP16_MAX;
        }

        float3 GetWorldPos(){
            return x;
        }

        float3 GetNormal(){
            return n;
        }

        float GetLinearDepth(){
            return v_z;
        }

        static Surface EmptySurface(){
            Surface s = (Surface)0;
            s.v_z = FP16_MAX;
            return s;
        }

        float3 WorldToTangent(float3 _w){
            float3 t, b;
            Math::BranchlessONB(n, t, b);
            return float3(dot(b, _w), dot(t, _w), dot(n, _w));
        }

        float3 TangentToWorld(float3 _h){
            float3 t, b;
            Math::BranchlessONB(n, t, b);
            return b * _h.x + t * _h.y + n * _h.z;
        }

        bool GetBrdfSample(out float3 _dir, inout RandomState _rng){
            float3 rnd;
            rnd.x = rng.GetFloat();
            rnd.y = rng.GetFloat();
            rnd.z = rng.GetFloat();

            if(rnd.z < diffuse_prob){
                float pdf;
                float3 h = SampleHemisphereCosineWithPdf(rnd.xy, pdf);
                _dir = TangentToWorld(h);
            }else{
                //specular
                float3 ve = normalize(WorldToTangent(v));
                float3 h = STL::ImportanceSampling::VNDF::GetRay(
                    rnd.xy, float2(roughness), ve
                );
                h = normalize(h);
                _dir = reflect(-v, TangentToWorld(h));

            }
            return dot(_dir, n) > 0.f;
        }

        float GetBrdfPdf(float3 _dir){
            float cos_theta = saturate(dot(_dir, n));
            float diffuse_pdf = cos_theta / PI;

            float3 h = normalize(_dir + v);
            float noh = saturate(dot(n, h));
            float3 nov = saturate(dot(n, v));
            float specular_pdf = STL::ImportanceSampling::VNDF::GetPDF(
                nov, noh, roughness
            );

            return cos_theta > 0.f ? lerp(diffuse_pdf, specular_pdf, 1.f - diffuse_prob) : 0.f;
        }

        float GetLightSampleTargetPdf(LightSample _sample){
            if(_sample.solid_angle_pdf <= 0.f) return 0.f;

            float3 l = normalize(_sample.x - x);
            if(dot(l, n) <= 0.f) return 0.f;

            float d = DiffuseTerm(v, n, l, roughness);
            
            float3 s;
            if(roughness == 0.f) s = 0.f;
            else{
                s = SpecularTerm(v, l, n, roughness, specular_f0);
            }
            float3 reflect_radiance = (d * diffuse_albedo + s) * _sample.radiance;

            return STL::Color::Luminance(reflect_radiance) / _sample.solid_angle_pdf;
        }

        float GetLightTargetPdfForVolume(PolymorphicLightInfo _light, float3 _vol_center, float _vol_radius){
            return PolymorphicLight::GetVolumeWeight(_light, _vol_center, _vol_radius);
        }

        LightSample SamplePolymorphicLight(PolymorphicLightInfo _light, float2 _uv){
            PolymorphicLightSample light_sample = PolymorphicLight::Sample(_light, _uv, x);
            LightSample res;
            res.x = light_sample.pos;
            res.n = light_sample.normal;
            res.radiance = light_sample.radiance;
            res.solid_angle_pdf = light_sample.solid_angle_pdf;
            res.type = GetLightType(_light);

            return res;
        }

        void GetLightDirDistance(LightSample _light, out float3 _dir, out float _distance){
            
            if(_light.type == EPolyLightType::ELEnv){
                _dir = -_light.n;
                _distance = s_light_max_distance;
            }
            else{
                float3 l = _light.x - x;
                _distance = length(l);
                _dir = l / _distance;
            }
        }

        RayDesc SetupVisibilityRay(float3 _sample_pos, float _x_offset = 0.001f){
            
            float3 l = _sample_pos - x;
            
            RayDesc ray;
            ray.Origin = x;
            ray.Direction = normalize(l);
            ray.TMin = _x_offset;
            ray.TMax = max(_x_offset, length(l) - 2 * _x_offset);
            return ray;
        }
    };

    Surface GetGBufferSurface(
        int2 _pixel_pos,
        ViewParam _view_param,
        Texture2D<float> _gbuffer_depth,
        Texture2D<uint> _gbuffer_normal,
        Texture2D<uint> _gbuffer_diffuse_albedo,
        Texture2D<uint> _gbuffer_specular_roughness
    ){
        Surface s = Surface::EmptySurface();
        if(any(_pixel_pos >= int2(_view_param.rect))) return s;
        s.v_z = _gbuffer_depth[_pixel_pos];

        if(s.v_z == FP16_MAX) return s;

        s.n = Moer::OctToNdirUnorm32(_gbuffer_normal[_pixel_pos]);
        s.diffuse_albedo = Moer::Unpack_R11G11B10_UFLOAT(_gbuffer_diffuse_albedo[_pixel_pos]);
        float4 specular_roughness = Moer::Unpack_R8G8B8A8_Gamma_UFLOAT(_gbuffer_specular_roughness[_pixel_pos]);
        s.specular_f0 = specular_roughness.xyz;
        s.roughness = specular_roughness.w;
        s.x = Moer::ViewdepthToWorldPos(_view_param, _pixel_pos, s.v_z);
        s.v = normalize(_view_param.dir_or_pos.xyz - s.x);
        s.diffuse_prob = s.GetDiffuseProbability();

        return s;
    }

    template<bool _prev_frame = false>
    Surface GetGBufferSurface(int2 _pixel_pos){

        TextureHandle gbuffer_depth;
        TextureHandle gbuffer_normal;
        TextureHandle gbuffer_diffuse_albedo;
        TextureHandle gbuffer_specular_roughness;
        if (_prev_frame){
            gbuffer_depth = TextureHandle(resample_params.bindless_handles.gbuffer_prev_depth);
            gbuffer_normal = TextureHandle(resample_params.bindless_handles.gbuffer_prev_normal);
            gbuffer_diffuse_albedo = TextureHandle(resample_params.bindless_handles.gbuffer_prev_diffuse_albedo);
            gbuffer_specular_roughness = TextureHandle(resample_params.bindless_handles.gbuffer_prev_specular_roughness);
            
        }else{

            gbuffer_depth = TextureHandle(resample_params.bindless_handles.gbuffer_depth);
            gbuffer_normal = TextureHandle(resample_params.bindless_handles.gbuffer_normal);
            gbuffer_diffuse_albedo = TextureHandle(resample_params.bindless_handles.gbuffer_diffuse_albedo);
            gbuffer_specular_roughness = TextureHandle(resample_params.bindless_handles.gbuffer_specular_roughness);
        }

        Texture2D<float> gbuffer_depth_tex = gbuffer_depth.GetTexture2D<float>();
        Texture2D<uint> gbuffer_normal_tex = gbuffer_normal.GetTexture2D<uint>();
        Texture2D<uint> gbuffer_diffuse_albedo_tex = gbuffer_diffuse_albedo.GetTexture2D<uint>();
        Texture2D<uint> gbuffer_specular_roughness_tex = gbuffer_specular_roughness.GetTexture2D<uint>();

        return GetGBufferSurface(
            _pixel_pos,
            resample_params.view_param,
            gbuffer_depth_tex,
            gbuffer_normal_tex,
            gbuffer_diffuse_albedo_tex,
            gbuffer_specular_roughness_tex
        );
    }


    float2 GetEnvironmentMapXYFromDir(float3 _dir){
        float2 uv = Math::DirToEquirectangularUV(_dir);
        uv.x -= resample_params.scene_params.env_map_rotation;
        uv = frac(uv);
        return uv;
    }

    float EvalEnvMapPdf(float3 _dir){
        if(!resample_params.scene_params.enable_env_map){
            return 1.f;
        }
        float2 uv = GetEnvironmentMapXYFromDir(_dir);
        uint2 pdf_tex_size = resample_params.env_pdf_size;
        uint2 texel_pos = uint2(uv * float2(pdf_tex_size));

        TextureHandle env_map_pdf = TextureHandle(resample_params.bindless_handles.env_map_pdf);
        Texture2D<float> env_map_pdf_tex = env_map_pdf.GetTexture2D<float>();
        float texel_val = env_map_pdf_tex[texel_pos];

        int last_mip = max(0, int(floor(log2(max(pdf_tex_size.x, pdf_tex_size.y)))));
        float avg_val = env_map_pdf_tex.mips[last_mip][uint2(0u, 0u)];

        float sum = avg_val * square( 1 << last_mip );
        return texel_val / sum;
    }

    float EvalLocalLightSrcPdf(uint _light_idx){
        uint2 pdf_tex_size = resample_params.local_light_pdf_size;
        uint2 texel_pos = Math::LinearIndexToZCurve(_light_idx);
        Texture2D<float> local_light_pdf_tex = TextureHandle(resample_params.bindless_handles.local_light_pdf).GetTexture2D<float>();

        int last_mip = max(0, int(floor(log2(max(pdf_tex_size.x, pdf_tex_size.y)))));
        float avg_val = local_light_pdf_tex.mips[last_mip][uint2(0u, 0u)];

        float sum = avg_val * square( 1 << last_mip );
        return local_light_pdf_tex[texel_pos] / sum;
    }

    PolymorphicLightInfo LoadLightInfo(uint _idx){
        ArrayBuffer light_buffer = ArrayBuffer(resample_params.bindless_handles.light_buffer);
        return light_buffer.Load<PolymorphicLightInfo>(_idx);
    }

    PolymorphicLightInfo LoadCompactLightInfo(uint _idx){
        uint4 pack1, pack2;
        pack1 = rw_ris_light_data_buffer[_idx * 2];
        pack2 = rw_ris_light_data_buffer[_idx * 2 + 1];
        return UnpackCompactLightInfo(pack1, pack2);
    }

    bool StoreCompactLightInfo(uint _idx, PolymorphicLightInfo _info){
        uint4 dat1, uint4 dat2;
        if(!PackCompactLightInfo(_info, pack1, pack2)) return false;

        rw_ris_light_data_buffer[_idx * 2] = dat1;
        rw_ris_light_data_buffer[_idx * 2 + 1] = dat2;
        return true;
    }


    float3 GetEnvMapRadiance(float3 _dir){
        if(!resample_params.scene_params.env_map_handle) return 0;

        TextureHandle tex_handle = TextureHandle(resample_params.scene_params.env_map_handle);

        float2 uv = Math::DirToEquirectangularUV(_dir);
        uv.x -= resample_params.scene_params.env_map_rotation;

        float3 env_radiance = tex_handle.SampleLevel<float4>(uv, 0).rgb;
        return env_radiance * resample_params.scene_params.env_map_scale;
    }

    uint GetLightIndex(uint _instance_idx, uint _geom_idx, uint _prim_idx){
        uint light_idx = s_invalid_light_idx;
        ByteAddressBuffer inst_buf = ((ArrayBuffer)resample_params.bindless_handles.instance_data).GetByteAddressBuffer();
        InstanceData instance = Lo adInstanceData(inst_buf, _instance_idx * sizeof(InstanceData));
        uint geom_idx = instance.first_geom_idx + _geom_idx;

        ArrayBuffer geom_to_light_arr = (ArrayBuffer)resample_params.bindless_handles.geom_to_light;
        light_idx = geom_to_light_arr.Load<uint>(geom_idx);
        if(light_idx == s_invalid_light_idx) return light_idx;
        return light_idx + _prim_idx;
    }

    bool RaytraceLocalLightVisibility(
        float3 _origin,
        float3 _direction,
        float _tmin,
        float _tmax,
        out uint _light_idx,
        out float2 _rand)
    {

        _light_idx = s_invalid_light_idx;
        _rand = 0.f;

        RayDesc ray_desc;
        ray_desc.Origin = _origin;
        ray_desc.Direction = _direction;
        ray_desc.TMin = _tmin;
        ray_desc.TMax = _tmax;

        float2 uv;
        bool b_hit;

        #if USE_RAYQUERY
        RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> ray_query;
        ray_query.TraceRayInline(tlas, RAY_FLAG_NONE, INSTANCE_FLAG_GEOMETRY_ALL, ray_desc);
        ray_query.Proceed();

        b_hit = ray_query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
        if(b_hit){
            _light_idx = GetLightIndex(ray_query.CommittedInstanceID(), ray_query.CommittedGeometryIndex(), ray_query.CommittedPrimitiveIndex());
            uv = ray_query.CommittedTriangleBarycentrics();
        }
        #else
        #endif

        if(_light_idx == s_invalid_light_idx) return b_hit;
        _rand = Math::BaryCentricsToRand2(Math::HitUVToBarycentrics(uv));
        return b_hit;
    }

    bool RaytraceConservativeVisibility(
        RaytracingAccelerationStructure _tlas,
        Surface _surface,
        float3 _sample_pos
    ){
        RayDesc ray = _surface.SetupVisibilityRay(_sample_pos);
        
        bool b_visible = false;

        #if USE_RAYQUERY
        RayQuery<RAY_FLAG_CULL_NON_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> ray_query;

        ray_query.TraceRayInline(_tlas, RAY_FLAG_NONE, RTVM_OPAQUE, ray);
        ray_query.Proceed();
        b_visible = ray_query.CommittedStatus() == COMMITTED_NOTHING;
        
        #else
        #endif

        return b_visible;
    }

    bool GetCurrentConservativeVisibility(Surface _surface, float3 _sample_pos){
        return RaytraceConservativeVisibility(tlas, _surface, _sample_pos);
    }

    bool GetPreviousConservativeVisibility(Surface _surface, float3 _sample_pos){
        if(!resample_params.enable_prev_tlas)
            return RaytraceConservativeVisibility(tlas, _surface, _sample_pos);
        else
            return RaytraceConservativeVisibility(prev_tlas, _surface, _sample_pos);
    }
}



#endif