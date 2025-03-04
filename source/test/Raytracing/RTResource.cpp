#include "RTResource.h"
#include "Configs.h"
#include "PixelFormat.h"
#include "PreprocessLightPass.h"
#include "ShaderUtils.h"
#include "config/ConfigManager.h"
#include <cstdio>
#include <filesystem>
#include <stb_image.h>
#include <utility>
#include "math/Function.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"

#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/lighting/ShaderParameters.h"
#include "tinyexr.h"

namespace Moer::Render {
    RTResource::RTResource(const std::filesystem::path& _resouce_path)
        : b_loaded(false), resource_path(_resouce_path) {
        //check valid path
        if (!std::filesystem::exists(_resouce_path)) {
            resource_path = ConfigManager::GetInstance().GetEditorResourcePath();
        }
    }

    RTResource::~RTResource() {
    }

    static uint CalcMaxMipCount(uint2 _extent) {
        uint max_dim = std::max(_extent.x, _extent.y);
        return 1 + static_cast<uint>(std::floor(std::log2(max_dim)));
    }

    void RTResource::LoadResources() {
        //load resources

        //textures
        Array<ExportTexture> exp_textures;
        {
            auto texture_path = resource_path / "textures";

            auto register_image = [this](TextureRef _tex, const std::string& _name) {
                //register image
                auto& image = this->textures[_name];
                image       = _tex;
            };

            CommandList cmd_list;
            if (std::filesystem::exists(texture_path)) {
                for (auto& entry : std::filesystem::directory_iterator(texture_path)) {
                    LOG_INFO("Load texture {}", entry.path().string());
                    if (entry.path().extension() == ".png") {
                        FILE* file = nullptr;
                        fopen_s(&file, entry.path().string().c_str(), "rb");
                        int width, height, channels;
                        if (file) {
                            ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

                            TextureRef texture = RenderDevice::Get().CreateTexture(
                                entry.path().filename().string(),
                                Extent2D(width, height),
                                PF_R8G8B8A8_UNORM,
                                ETextureUsageFlags::SAMPLED);
                            exp_textures.emplace_back(texture, ETextureState::SAMPLE);
                            cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4), texture);
                            cmd_list.AddCallback([data]() {
                                stbi_image_free(data);
                            });
                            register_image(texture, entry.path().filename().string());
                        }
                    }

                    else if (entry.path().extension() == ".exr") {
                        //load exr
                        int         width = 0, height = 0, channels = 0;
                        float*      data = nullptr;
                        const char* err  = nullptr;
                        auto        ret  = LoadEXR(&data, &width, &height, entry.path().string().c_str(), &err);
                        if (ret != TINYEXR_SUCCESS) {
                            if (err) {
                                fprintf(stderr, "ERR : %s\n", err);
                                FreeEXRErrorMessage(err);// release memory of error message.
                            }
                        }
                        TextureRef texture = RenderDevice::Get().CreateTexture(
                            entry.path().filename().string(),
                            Extent2D(width, height),
                            PF_R32G32B32A32_SFLOAT,
                            ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS,
                            CalcMaxMipCount(uint2(width, height)));
                        exp_textures.emplace_back(texture, ETextureState::SAMPLE);

                        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4 * sizeof(float)), texture);
                        cmd_list.AddCallback([data]() {
                            free(data);
                        });
                        register_image(texture, entry.path().filename().string());
                        default_env_map_name = textures.find(entry.path().filename().string())->first;
                    }
                }
            }
            RenderDevice& device = RenderDevice::Get();
            // auto          sync_time = device.GetCopyQueue().Execute(cmd_list.Submit());
            // device.GetCopyQueue().Sync(sync_time.timeline);

            cmd_list.ExportTextureToQueue(EQueueType::Graphics, std::move(exp_textures));

            auto sync_time = device.GetCopyQueue().Execute(cmd_list.Submit());
            device.GetCopyQueue().Sync(sync_time.timeline);
        }
    }

    TextureRef RTResource::GetDefaultEnvMap() {
        return GetTexture(default_env_map_name);
    }

    void RTResource::UnloadResources() {
        //unload resources
    }

    TextureRef RTResource::GetTexture(std::string_view _name) const {
        auto it = textures.find(std::string(_name));
        if (it != textures.end()) {
            return it->second;
        }
        return nullptr;
    }

    BufferRef RTResource::GetBuffer(std::string_view _name) const {
        return nullptr;
    }

    RTContext::RTContext(
        ShaderUtils&               _sd_utils,
        ImportanceSamplingContext& _is_ctx,
        BindlessArrayRef           _bdls_array) : max_emissive_meshes(0),
                                        max_emissive_triangles(0),
                                        max_geom_instance(0),
                                        max_prim_lights(0),
                                        sd_utils(_sd_utils),
                                        is_ctx(_is_ctx),
                                        bdls(_bdls_array) {

        RenderDevice& device = RenderDevice::Get();
        neighbor_offset_buf  = device.CreateBuffer<byte>(
            is_ctx.GetNeighborOffsetCnt() * 2 * sizeof(float),
            EBufferUsageFlags::UNORDERED_ACCESS);

        neighbor_offset_buf->SetName("neighbor_offset_buf");

        AllocateAndFreeBdlsIfNeeded(bindless_handles.neighbor_offset, neighbor_offset_buf->GetView());
    }

    void RTContext::SetBindlessHandles(uint _geom_data_buf_handle, uint _instance_data_buf_handle, uint _material_data_buf_handle) {

        bindless_handles.geom_data     = _geom_data_buf_handle;
        bindless_handles.instance_data = _instance_data_buf_handle;
        bindless_handles.material_data = _material_data_buf_handle;
    }

    void RTContext::FillFrameResources(uint2 _resolution) {
        RenderDevice& device        = RenderDevice::Get();
        frame_rt.view_depth         = device.CreateTexture("view_depth", Extent2D(_resolution), PF_R32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.diffuse_albedo     = device.CreateTexture("diffuse_albedo", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.specular_roughness = device.CreateTexture("specular_roughness", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.normal             = device.CreateTexture("normal", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.emission           = device.CreateTexture("emission", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.motion             = device.CreateTexture("motion", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.clip_depth         = device.CreateTexture("clip_depth", Extent2D(_resolution), PF_R32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

        frame_rt.prev_view_depth         = device.CreateTexture("prev_view_depth", Extent2D(_resolution), PF_R32_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.prev_diffuse_albedo     = device.CreateTexture("prev_diffuse_albedo", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.prev_specular_roughness = device.CreateTexture("prev_specular_roughness", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.prev_normal             = device.CreateTexture("prev_normal", Extent2D(_resolution), PF_R32_UINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.prev_luminance          = device.CreateTexture("prev_luminance", Extent2D(_resolution), PF_R16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

        frame_rt.normal_roughness           = device.CreateTexture("normal_roughness", Extent2D(_resolution), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.diffuse_lighting           = device.CreateTexture("diffuse_lighting", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.prev_diffuse_lighting      = device.CreateTexture("prev_diffuse_lighting", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.specular_lighting          = device.CreateTexture("specular_lighting", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.prev_specular_lighting     = device.CreateTexture("prev_specular_lighting", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.temporal_sample_pos        = device.CreateTexture("temporal_sample_pos", Extent2D(_resolution), PF_R16G16_SINT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.gradients                  = device.CreateTexture("gradients", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED, 1, 2);
        frame_rt.restir_luminance           = device.CreateTexture("restir_luminance", Extent2D(_resolution), PF_R16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.denoised_diffuse_lighting  = device.CreateTexture("denoised_diffuse_lighting", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.denoised_specular_lighting = device.CreateTexture("denoised_specular_lighting", Extent2D(_resolution), PF_R16G16B16A16_SFLOAT, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

        frame_rt.debug_color = device.CreateTexture("debug_color", Extent2D(_resolution), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);
        frame_rt.scene_color = device.CreateTexture("scene_color", Extent2D(_resolution), PF_R8G8B8A8_UNORM, ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED);

        Sampler spl{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE};
        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_depth, frame_rt.view_depth->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_normal, frame_rt.normal->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_diffuse_albedo, frame_rt.diffuse_albedo->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_specular_roughness, frame_rt.specular_roughness->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.motion, frame_rt.motion->GetView(), spl);

        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_prev_depth, frame_rt.prev_view_depth->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_prev_normal, frame_rt.prev_normal->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_prev_diffuse_albedo, frame_rt.prev_diffuse_albedo->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.gbuffer_prev_specular_roughness, frame_rt.prev_specular_roughness->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.restir_prev_luminance, frame_rt.prev_luminance->GetView(), spl);
        AllocateAndFreeBdlsIfNeeded(bindless_handles.denoiser_normal_roughness, frame_rt.normal_roughness->GetView(), spl);
    }

    void RTContext::SetResolution(uint2 _resolution) {
        FillFrameResources(_resolution);
        b_current_frame = true;
    }

    void RTContext::FillLowDiscrepancySequence(CommandList& _cmd_list) {
        if (b_has_neighbor_offset) {
            return;
        }
        GenLowDiscrepancySequenceParam param;
        param.num_dimensions = 2;
        param.num_samples    = is_ctx.GetNeighborOffsetCnt();
        sd_utils.GenerateLowDiscrepancySequence(_cmd_list, param, neighbor_offset_buf->GetView());
        b_has_neighbor_offset = true;
        // _cmd_list.Compute(sd_utils.gen_low_discrepancy_pipeline, _param, _output).Dispatch(uint3(DivCeil(_param.num_samples, 256), 1, 1), "GenerateLowDiscrepancySequence");
    }

    void RTContext::CreateEnvMapResources(TextureRef _env_map, CommandList& _cmd_list) {

        uint2         extent = _env_map->GetExtent().xy;
        RenderDevice& device = RenderDevice::Get();

        env_pdf_mips.clear();
        env_pdf_tex = device.CreateTexture("env_pdf_tex",
                                           Extent2D(extent.x, extent.y),
                                           PF_R16_SFLOAT,
                                           ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED,
                                           uint(ceilf(log2f(float(std::max(extent.x, extent.y))))));

        for (int i = 0; i < env_pdf_tex->GetNumMips(); ++i) {
            env_pdf_mips.push_back(env_pdf_tex->GetView(i));
        }
        sd_utils.GenerateMipPdf(_cmd_list, _env_map, env_pdf_mips);

        AllocateAndFreeBdlsIfNeeded(bindless_handles.env_pdf, env_pdf_tex->GetView(0, env_pdf_tex->GetNumMips()), Sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE});
        AllocateAndFreeBdlsIfNeeded(scene_params.env_map_handle, _env_map->GetView(0, _env_map->GetNumMips()), Sampler{ESamplerFilter::SF_CUBIC, ESamplerAddressMode::SAM_REPEAT});
        scene_params.enable_env_map = 1;
        SetEnvMapInfos(1.f, 0.f);
    }

    void RTContext::CreateBuffersIfNeeded(

        uint _num_emissive_meshes,
        uint _num_emissive_triangles,
        uint _num_prim_lights,
        uint _num_geom_instance) {

        RenderDevice& device   = RenderDevice::Get();
        uint          task_num = _num_emissive_meshes + _num_prim_lights;

        if (!task_buf || task_num > task_buf->GetNumElement()) {
            task_buf = device.CreateBuffer<PrepareLightsTask>(task_num, EBufferUsageFlags::UNORDERED_ACCESS);
            task_buf->SetName("task_buf");
        }

        // task_buf             = device.CreateBuffer<PrepareLightsTask>(max_emissive_meshes + max_prim_lights, EBufferUsageFlags::UNORDERED_ACCESS);
        if (!geo_instance_to_light_buf || _num_geom_instance > max_geom_instance) {
            geo_instance_to_light_buf = device.CreateBuffer<uint>(_num_geom_instance, EBufferUsageFlags::UNORDERED_ACCESS);
            geo_instance_to_light_buf->SetName("geo_instance_to_light_buf");

            AllocateAndFreeBdlsIfNeeded(bindless_handles.geo_instance_to_light, geo_instance_to_light_buf->GetView());
        }
        max_geom_instance      = _num_geom_instance;
        max_prim_lights        = _num_prim_lights;
        max_emissive_meshes    = _num_emissive_meshes;
        max_emissive_triangles = _num_emissive_triangles;

        uint max_local_lights  = max_emissive_triangles + max_prim_lights;
        uint light_buf_element = max_local_lights * 2;

        if (!light_mapping_buf || light_buf_element > light_data_buf->GetNumElement()) {
            light_mapping_buf = device.CreateBuffer<uint>(light_buf_element, EBufferUsageFlags::UNORDERED_ACCESS);
            light_data_buf    = device.CreateBuffer<PolymorphicLightInfo>(light_buf_element, EBufferUsageFlags::UNORDERED_ACCESS);

            light_mapping_buf->SetName("light_mapping_buf");
            light_data_buf->SetName("light_data_buf");

            AllocateAndFreeBdlsIfNeeded(bindless_handles.light_index, light_mapping_buf->GetView());
            AllocateAndFreeBdlsIfNeeded(bindless_handles.poly_light_data, light_data_buf->GetView());
        }

        if (!prim_light_buf || max_prim_lights > prim_light_buf->GetNumElement()) {
            prim_light_buf = device.CreateBuffer<PolymorphicLightInfo>(max_prim_lights, EBufferUsageFlags::UNORDERED_ACCESS);
            prim_light_buf->SetName("prim_light_buf");
        }
        //
        {
            uint texture_width  = RoundUpToPowerOf2(uint(ceil(sqrt(double(light_buf_element)))));
            uint texture_height = RoundUpToPowerOf2(uint(ceil(double(light_buf_element) / texture_width)));
            uint mips           = Max(1u, uint(log2(Max(texture_width, texture_height))) + 1u);

            if (!local_light_pdf_tex || texture_height != local_light_pdf_tex->GetExtent().y || texture_width != local_light_pdf_tex->GetExtent().x) {
                local_light_pdf_mips.clear();
                local_light_pdf_tex = device.CreateTexture("local_light_pdf_tex",
                                                           Extent2D(texture_width, texture_height),
                                                           PF_R32_SFLOAT,
                                                           ETextureUsageFlags::UNORDERED_ACCESS | ETextureUsageFlags::SAMPLED,
                                                           mips);

                for (int i = 0; i < local_light_pdf_tex->GetNumMips(); ++i) {
                    local_light_pdf_mips.push_back(local_light_pdf_tex->GetView(i));
                }

                AllocateAndFreeBdlsIfNeeded(bindless_handles.local_light_pdf, local_light_pdf_tex->GetView(0, local_light_pdf_tex->GetNumMips()), Sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_CLAMP_TO_EDGE});
            }
        }
    }

    void RTContext::AllocateAndFreeBdlsIfNeeded(uint& _target, const TextureView& _view, Sampler _sampler) {
        if (_target) {
            bdls->FreeTexture(_target);
        }
        _target = bdls->AllocateTexture(_view, _sampler);
    }

    void RTContext::AllocateAndFreeBdlsIfNeeded(uint& _target, const BufferView& _view) {
        if (_target) {
            bdls->FreeBuffer(_target);
        }
        _target = bdls->AllocateBuffer(_view);
    }

    void RTContext::SetEnvMapInfos(float _scale, float _rotation) {
        scene_params.env_map_scale    = _scale;
        scene_params.env_map_rotation = _rotation;
    }

    void
    RTContext::Tick(CameraRef _camera) {
        auto& device = RenderDevice::Get();
        prev_view    = main_view;

        main_view.view2world        = Transpose(_camera->GetToWorldMatrix());
        main_view.world2view        = Transpose(_camera->GetViewMatrix());
        main_view.world2clip        = Transpose(_camera->GetViewProjectionMatrix());
        main_view.view2clip         = Transpose(_camera->GetProjectionMatrix());
        main_view.clip2view         = Transpose(_camera->GetProjectionMatrixInv());
        main_view.clip2world        = Transpose(_camera->GetViewProjectionMatrixInv());
        main_view.frustum           = _camera->GetFrustum();
        main_view.near_far          = float2(_camera->GetNearClip(), _camera->GetFarClip());
        main_view.rect              = float2(is_ctx.GetReSTIRDIConfig().render_width, is_ctx.GetReSTIRDIConfig().render_height);
        main_view.inv_rect          = float2(1.f / main_view.rect.x, 1.f / main_view.rect.y);
        main_view.dir_or_pos        = float4(_camera->GetPosition(), 1.f);
        main_view.clip2window_scale = float2(0.5f * main_view.rect.x, -0.5f * main_view.rect.y);
        main_view.clip2window_bias  = float2(0.5f * main_view.rect.x, 0.5f * main_view.rect.y);
        main_view.window2clip_scale = float2(2.f / main_view.rect.x, -2.f / main_view.rect.y);
        main_view.window2clip_bias  = float2(-1.f, 1.f);
        //restir
        {

            if (!light_reservoir_buf || is_ctx.GetReSTIRDIRuntimeConfig().reservoir_buffer_params.block_array_pitch * s_num_restirdi_reservoir_buffer > light_reservoir_buf->GetNumElement()) {
                light_reservoir_buf = device.CreateBuffer<DI::PackedReservoir>(
                    is_ctx.GetReSTIRDIRuntimeConfig().reservoir_buffer_params.block_array_pitch * s_num_restirdi_reservoir_buffer,
                    EBufferUsageFlags::UNORDERED_ACCESS);

                light_reservoir_buf->SetName("light_reservoir_buf");
            }

            if (!ris_buf || 2 * std::max(is_ctx.GetSegmentAllocator().GetTotalSize(), 1u) > ris_buf->GetNumElement()) {
                ris_buf = device.CreateBuffer<uint>(2 * std::max(is_ctx.GetSegmentAllocator().GetTotalSize(), 1u), EBufferUsageFlags::UNORDERED_ACCESS);
                ris_buf->SetName("ris_buf");

                ris_light_data_buf = device.CreateBuffer<uint4>(2 * std::max(is_ctx.GetSegmentAllocator().GetTotalSize(), 1u), EBufferUsageFlags::UNORDERED_ACCESS);
                ris_light_data_buf->SetName("ris_light_data_buf");
            }
        }
    }

    void RTContext::AdvanceFrame() {
        b_current_frame = !b_current_frame;
        std::swap(bindless_handles.gbuffer_normal, bindless_handles.gbuffer_prev_normal);
        std::swap(bindless_handles.gbuffer_depth, bindless_handles.gbuffer_prev_depth);
        std::swap(bindless_handles.gbuffer_diffuse_albedo, bindless_handles.gbuffer_prev_diffuse_albedo);
        std::swap(bindless_handles.gbuffer_specular_roughness, bindless_handles.gbuffer_prev_specular_roughness);
        std::swap(bindless_handles.restir_luminance, bindless_handles.restir_prev_luminance);
    }
};// namespace Moer::Render