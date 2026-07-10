#include "ProbeVolumeResource.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"

#include <cmath>
#include <cstring>

namespace Moer::Render::Raster {
namespace {

uint ClampProbeCount(int value) {
    return static_cast<uint>(Clamp(value, 1, 16));
}

void ClampProbeBudget(uint& count_x, uint& count_y, uint& count_z, uint max_count) {
    while (count_x * count_y * count_z > max_count) {
        if (count_z >= count_x && count_z >= count_y && count_z > 1) {
            --count_z;
        } else if (count_x >= count_y && count_x > 1) {
            --count_x;
        } else if (count_y > 1) {
            --count_y;
        } else {
            break;
        }
    }
}

float3 SanitizeExtent(float3 extent) {
    return Max(extent, float3(0.1f));
}

float3 SafeNormalize(float3 value, float3 fallback) {
    if (SquaredLengthf(value) <= 1e-6f) {
        return fallback;
    }
    return Normalizef(value);
}

bool NearlyEqual(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 1e-5f;
}

bool NearlyEqual(float3 lhs, float3 rhs) {
    return NearlyEqual(lhs.x, rhs.x) && NearlyEqual(lhs.y, rhs.y) && NearlyEqual(lhs.z, rhs.z);
}

float3 GetMainLightColor(const Scene& scene, float3& out_direction, float& out_intensity) {
    out_direction = float3(0.0f, -1.0f, 0.0f);
    out_intensity = 0.0f;

    const entt::entity light_entity = scene.GetMainDirectionalLightEntity();
    if (light_entity == entt::null) {
        return float3(1.0f);
    }

    const auto& light = scene.GetMainDirectionalLight();
    out_direction     = SafeNormalize(light.d_direction, out_direction);
    out_intensity     = light.intensity;
    return light.color;
}

} // namespace

void ProbeVolumeResource::Create(RenderDevice& device, BindlessArrayRef& bdls) {
    if (m_probe_buffer.buf != nullptr) {
        return;
    }

    constexpr uint buffer_byte_size = sizeof(ProbeGridProbeData) * RASTER_PROBE_MAX_COUNT;
    m_probe_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::ProbeData",
        buffer_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_probe_buffer.hdl = bdls->AllocateBuffer(m_probe_buffer.buf->GetView());

    constexpr uint volume_buffer_byte_size = sizeof(ProbeVolumeGpuDesc) * RASTER_PROBE_VOLUME_MAX_COUNT;
    m_volume_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::VolumeDescriptors",
        volume_buffer_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_volume_buffer.hdl = bdls->AllocateBuffer(m_volume_buffer.buf->GetView());

    constexpr uint visibility_atlas_byte_size =
        sizeof(ProbeGridVisibilityTexel) * RASTER_PROBE_MAX_COUNT * RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT;
    m_visibility_atlas_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::VisibilityAtlas",
        visibility_atlas_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_visibility_atlas_buffer.hdl = bdls->AllocateBuffer(m_visibility_atlas_buffer.buf->GetView());

    constexpr uint irradiance_atlas_byte_size =
        sizeof(ProbeGridIrradianceTexel) * RASTER_PROBE_MAX_COUNT * RASTER_PROBE_IRRADIANCE_ATLAS_TEXEL_COUNT;
    m_irradiance_atlas_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::IrradianceAtlas",
        irradiance_atlas_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_irradiance_atlas_buffer.hdl = bdls->AllocateBuffer(m_irradiance_atlas_buffer.buf->GetView());

    const ETextureUsageFlags atlas_usage = ETextureUsageFlags::SAMPLED | ETextureUsageFlags::UNORDERED_ACCESS;
    const Extent3D           atlas_size(
                  RASTER_PROBE_ATLAS_TEXTURE_WIDTH,
                  RASTER_PROBE_ATLAS_TEXTURE_HEIGHT,
                  1
              );
    const Sampler atlas_sampler(SF_LINEAR, SAM_CLAMP_TO_EDGE);

    m_visibility_atlas_texture.tex = device.CreateTexture(
        "Raster::ProbeVolume::VisibilityTextureAtlas",
        atlas_size,
        PF_R16G16B16A16_SFLOAT,
        atlas_usage
    );
    m_visibility_atlas_texture.hdl =
        bdls->AllocateTexture(m_visibility_atlas_texture.tex->GetView(), atlas_sampler);

    m_irradiance_atlas_texture.tex = device.CreateTexture(
        "Raster::ProbeVolume::IrradianceTextureAtlas",
        atlas_size,
        PF_R16G16B16A16_SFLOAT,
        atlas_usage
    );
    m_irradiance_atlas_texture.hdl =
        bdls->AllocateTexture(m_irradiance_atlas_texture.tex->GetView(), atlas_sampler);

    constexpr uint scene_data_byte_size = sizeof(GBufferPassParams);
    m_scene_data_buffer = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::SceneData",
        scene_data_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_scene_data_upload.resize(scene_data_byte_size);

    LOG_DEBUG(
        "[ProbeGI] Created probe buffers: max_volumes={}, max_count={}, probe_byte_size={}, probe_handle={}, volume_desc_byte_size={}, volume_desc_handle={}, visibility_dim={}x{}, visibility_byte_size={}, visibility_handle={}, irradiance_dim={}x{}, irradiance_byte_size={}, irradiance_handle={}, atlas_texture={}x{}, visibility_texture_handle={}, irradiance_texture_handle={}, scene_data_byte_size={}",
        RASTER_PROBE_VOLUME_MAX_COUNT,
        RASTER_PROBE_MAX_COUNT,
        buffer_byte_size,
        m_probe_buffer.hdl,
        volume_buffer_byte_size,
        m_volume_buffer.hdl,
        RASTER_PROBE_VISIBILITY_ATLAS_DIM,
        RASTER_PROBE_VISIBILITY_ATLAS_DIM,
        visibility_atlas_byte_size,
        m_visibility_atlas_buffer.hdl,
        RASTER_PROBE_IRRADIANCE_ATLAS_DIM,
        RASTER_PROBE_IRRADIANCE_ATLAS_DIM,
        irradiance_atlas_byte_size,
        m_irradiance_atlas_buffer.hdl,
        RASTER_PROBE_ATLAS_TEXTURE_WIDTH,
        RASTER_PROBE_ATLAS_TEXTURE_HEIGHT,
        m_visibility_atlas_texture.hdl,
        m_irradiance_atlas_texture.hdl,
        scene_data_byte_size
    );
}

void ProbeVolumeResource::Destroy(BindlessArrayRef& bdls) {
    if (m_probe_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_probe_buffer.hdl);
        m_probe_buffer.hdl = 0;
    }
    if (m_volume_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_volume_buffer.hdl);
        m_volume_buffer.hdl = 0;
    }
    if (m_visibility_atlas_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_visibility_atlas_buffer.hdl);
        m_visibility_atlas_buffer.hdl = 0;
    }
    if (m_irradiance_atlas_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_irradiance_atlas_buffer.hdl);
        m_irradiance_atlas_buffer.hdl = 0;
    }
    if (m_visibility_atlas_texture.hdl != 0) {
        bdls->UnbindTexture(m_visibility_atlas_texture.hdl);
        m_visibility_atlas_texture.hdl = 0;
    }
    if (m_irradiance_atlas_texture.hdl != 0) {
        bdls->UnbindTexture(m_irradiance_atlas_texture.hdl);
        m_irradiance_atlas_texture.hdl = 0;
    }
    m_probe_buffer.buf = nullptr;
    m_volume_buffer.buf = nullptr;
    m_visibility_atlas_buffer.buf = nullptr;
    m_irradiance_atlas_buffer.buf = nullptr;
    m_visibility_atlas_texture.tex = nullptr;
    m_irradiance_atlas_texture.tex = nullptr;
    m_scene_data_buffer = nullptr;
    m_scene_data_upload.clear();
    m_volume_data_upload.clear();
    m_has_snapshot = false;
    m_history_valid.fill(false);
}

void ProbeVolumeResource::UpdateSceneData(CommandList& cmd_list, const Scene& scene) {
    if (m_scene_data_buffer == nullptr || m_volume_buffer.buf == nullptr) {
        return;
    }

    if (!m_volume_data_upload.empty()) {
        cmd_list.CopyFrom(
            std::move(m_volume_data_upload),
            m_volume_buffer.buf->GetView(),
            "Raster::ProbeVolume::Volume Descriptor Upload"
        );
        m_volume_data_upload.clear();
    }

    const auto& gpu_scene_res = scene.GetGpuSceneRes();

    GBufferPassParams params{};
    params.instance_buf_hdl  = gpu_scene_res.instance_buf.hdl;
    params.primitive_buf_hdl = gpu_scene_res.primitive_buf.hdl;
    params.material_buf_hdl  = gpu_scene_res.material_buf.hdl;

    params.position_buf_hdl       = gpu_scene_res.position_buf.hdl;
    params.packed_normal_buf_hdl  = gpu_scene_res.packed_normal_buf.hdl;
    params.packed_tangent_buf_hdl = gpu_scene_res.packed_tangent_buf.hdl;
    params.texcoord0_buf_hdl      = gpu_scene_res.texcoord0_buf.hdl;
    params.index_buf_hdl          = gpu_scene_res.index_buf.hdl;

    params.rt_instance_buf_hdl        = gpu_scene_res.rt_instance_buf.hdl;
    params.rt_primitive_table_buf_hdl = gpu_scene_res.rt_primitive_table_buf.hdl;

    m_scene_data_upload.resize(sizeof(GBufferPassParams));
    std::memcpy(m_scene_data_upload.data(), &params, sizeof(GBufferPassParams));
    cmd_list.CopyFrom(
        std::move(m_scene_data_upload),
        m_scene_data_buffer->GetView(),
        "Raster::ProbeVolume::SceneData Upload"
    );
}

ProbeVolumeResource::Snapshot ProbeVolumeResource::BuildSnapshot(const RasterConfig& config) const {
    Snapshot snapshot{};
    snapshot.enabled    = config.probe_gi_enabled;
    snapshot.debug_mode = static_cast<uint>(Clamp(config.probe_gi_debug_mode, 0, 4));
    snapshot.trace_distance     = Max(config.probe_gi_trace_distance, 0.1f);
    snapshot.trace_ray_count =
        static_cast<uint>(Clamp(config.probe_gi_trace_ray_count, 1, int(RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT)));
    snapshot.visibility_bias    = Max(config.probe_gi_visibility_bias, 0.0f);
    snapshot.visibility_power   = Max(config.probe_gi_visibility_power, 0.1f);
    snapshot.visibility_min_weight = Clamp(config.probe_gi_visibility_min_weight, 0.0f, 1.0f);
    snapshot.visibility_strength   = Clamp(config.probe_gi_visibility_strength, 0.0f, 1.0f);
    snapshot.irradiance_hysteresis = Clamp(config.probe_gi_irradiance_hysteresis, 0.0f, 0.99f);
    snapshot.visibility_hysteresis = Clamp(config.probe_gi_visibility_hysteresis, 0.0f, 0.99f);
    snapshot.debug_scale        = Max(config.probe_gi_debug_scale, 0.0f);
    snapshot.sky_intensity      = Max(config.probe_gi_sky_intensity, 0.0f);
    snapshot.directional_bounce = Max(config.probe_gi_directional_bounce, 0.0f);
    snapshot.sky_color          = Max(config.probe_gi_sky_color, float3(0.0f));
    snapshot.ground_color       = Max(config.probe_gi_ground_color, float3(0.0f));

    const uint configured_volume_count =
        static_cast<uint>(Clamp(config.probe_gi_volume_count, 1, int(RASTER_PROBE_VOLUME_MAX_COUNT)));
    for (uint config_index = 0; config_index < configured_volume_count; ++config_index) {
        const ProbeVolumeConfig& volume_config = config.probe_gi_volumes[config_index];
        if (!volume_config.enabled || snapshot.total_count >= RASTER_PROBE_MAX_COUNT) {
            continue;
        }

        VolumeSnapshot& volume = snapshot.volumes[snapshot.volume_count];
        volume.config_index = config_index;
        volume.count_x      = ClampProbeCount(volume_config.count_x);
        volume.count_y      = ClampProbeCount(volume_config.count_y);
        volume.count_z      = ClampProbeCount(volume_config.count_z);

        const uint requested_count = volume.count_x * volume.count_y * volume.count_z;
        const uint remaining_budget = RASTER_PROBE_MAX_COUNT - snapshot.total_count;
        ClampProbeBudget(
            volume.count_x,
            volume.count_y,
            volume.count_z,
            Min(RASTER_PROBE_MAX_COUNT_PER_VOLUME, remaining_budget)
        );

        volume.total_count = volume.count_x * volume.count_y * volume.count_z;
        volume.probe_offset = snapshot.total_count;
        volume.origin       = volume_config.origin;
        volume.extent       = SanitizeExtent(volume_config.extent);
        volume.spacing      = float3(
            volume.count_x > 1 ? volume.extent.x / float(volume.count_x - 1) : volume.extent.x,
            volume.count_y > 1 ? volume.extent.y / float(volume.count_y - 1) : volume.extent.y,
            volume.count_z > 1 ? volume.extent.z / float(volume.count_z - 1) : volume.extent.z
        );
        volume.intensity   = Max(config.probe_gi_intensity * volume_config.intensity_scale, 0.0f);
        volume.normal_bias = Max(config.probe_gi_normal_bias, 0.0f);
        const float max_blend_distance =
            Max(Min(volume.extent.x, Min(volume.extent.y, volume.extent.z)) * 0.5f, 0.01f);
        volume.blend_distance = Clamp(volume_config.blend_distance, 0.01f, max_blend_distance);

        snapshot.total_count += volume.total_count;
        ++snapshot.volume_count;

        if (requested_count != volume.total_count) {
            LOG_DEBUG(
                "[ProbeGI] Volume {} probe count clamped from {} to {} (per_volume_max={}, remaining_global_budget={}).",
                config_index,
                requested_count,
                volume.total_count,
                RASTER_PROBE_MAX_COUNT_PER_VOLUME,
                remaining_budget
            );
        }
    }

    return snapshot;
}

bool ProbeVolumeResource::HasSnapshotChanged(const Snapshot& snapshot) const {
    if (!m_has_snapshot) {
        return true;
    }

    if (m_snapshot.enabled != snapshot.enabled || m_snapshot.debug_mode != snapshot.debug_mode ||
        m_snapshot.volume_count != snapshot.volume_count || m_snapshot.total_count != snapshot.total_count ||
        !NearlyEqual(m_snapshot.trace_distance, snapshot.trace_distance) ||
           m_snapshot.trace_ray_count != snapshot.trace_ray_count ||
           !NearlyEqual(m_snapshot.visibility_bias, snapshot.visibility_bias) ||
           !NearlyEqual(m_snapshot.visibility_power, snapshot.visibility_power) ||
           !NearlyEqual(m_snapshot.visibility_min_weight, snapshot.visibility_min_weight) ||
           !NearlyEqual(m_snapshot.visibility_strength, snapshot.visibility_strength) ||
           !NearlyEqual(m_snapshot.irradiance_hysteresis, snapshot.irradiance_hysteresis) ||
           !NearlyEqual(m_snapshot.visibility_hysteresis, snapshot.visibility_hysteresis) ||
           !NearlyEqual(m_snapshot.debug_scale, snapshot.debug_scale) ||
           !NearlyEqual(m_snapshot.sky_intensity, snapshot.sky_intensity) ||
           !NearlyEqual(m_snapshot.directional_bounce, snapshot.directional_bounce) ||
           !NearlyEqual(m_snapshot.sky_color, snapshot.sky_color) ||
        !NearlyEqual(m_snapshot.ground_color, snapshot.ground_color)) {
        return true;
    }

    for (uint volume_index = 0; volume_index < snapshot.volume_count; ++volume_index) {
        const VolumeSnapshot& lhs = m_snapshot.volumes[volume_index];
        const VolumeSnapshot& rhs = snapshot.volumes[volume_index];
        if (lhs.config_index != rhs.config_index || lhs.count_x != rhs.count_x || lhs.count_y != rhs.count_y ||
            lhs.count_z != rhs.count_z || lhs.total_count != rhs.total_count ||
            lhs.probe_offset != rhs.probe_offset || !NearlyEqual(lhs.origin, rhs.origin) ||
            !NearlyEqual(lhs.extent, rhs.extent) || !NearlyEqual(lhs.spacing, rhs.spacing) ||
            !NearlyEqual(lhs.intensity, rhs.intensity) || !NearlyEqual(lhs.normal_bias, rhs.normal_bias) ||
            !NearlyEqual(lhs.blend_distance, rhs.blend_distance)) {
            return true;
        }
    }

    return false;
}

bool ProbeVolumeResource::RequiresHistoryReset(const Snapshot& snapshot, uint volume_index) const {
    if (!m_has_snapshot || volume_index >= m_snapshot.volume_count || !m_history_valid[volume_index]) {
        return true;
    }

    const VolumeSnapshot& previous = m_snapshot.volumes[volume_index];
    const VolumeSnapshot& current  = snapshot.volumes[volume_index];
    return m_snapshot.enabled != snapshot.enabled || previous.config_index != current.config_index ||
           previous.count_x != current.count_x || previous.count_y != current.count_y ||
           previous.count_z != current.count_z || previous.total_count != current.total_count ||
           previous.probe_offset != current.probe_offset || !NearlyEqual(previous.origin, current.origin) ||
           !NearlyEqual(previous.extent, current.extent) || !NearlyEqual(previous.spacing, current.spacing) ||
           !NearlyEqual(m_snapshot.trace_distance, snapshot.trace_distance) ||
           m_snapshot.trace_ray_count != snapshot.trace_ray_count ||
           !NearlyEqual(m_snapshot.visibility_bias, snapshot.visibility_bias) ||
           !NearlyEqual(m_snapshot.sky_intensity, snapshot.sky_intensity) ||
           !NearlyEqual(m_snapshot.directional_bounce, snapshot.directional_bounce) ||
           !NearlyEqual(m_snapshot.sky_color, snapshot.sky_color) ||
           !NearlyEqual(m_snapshot.ground_color, snapshot.ground_color);
}

void ProbeVolumeResource::StageVolumeUpload(const Snapshot& snapshot) {
    m_gpu_volume_descs.fill({});
    for (uint volume_index = 0; volume_index < snapshot.volume_count; ++volume_index) {
        const VolumeSnapshot& volume = snapshot.volumes[volume_index];
        ProbeVolumeGpuDesc&   desc   = m_gpu_volume_descs[volume_index];
        desc.origin_bias = float4(volume.origin.x, volume.origin.y, volume.origin.z, volume.normal_bias);
        desc.spacing_intensity =
            float4(volume.spacing.x, volume.spacing.y, volume.spacing.z, volume.intensity);
        desc.extent_blend =
            float4(volume.extent.x, volume.extent.y, volume.extent.z, volume.blend_distance);
        desc.counts = uint4(volume.count_x, volume.count_y, volume.count_z, volume.total_count);
        desc.allocation = uint4(volume.probe_offset, volume.config_index, 0u, 0u);
        desc.visibility = float4(
            snapshot.visibility_bias,
            snapshot.visibility_power,
            snapshot.visibility_min_weight,
            snapshot.visibility_strength
        );
    }

    m_volume_data_upload.resize(sizeof(m_gpu_volume_descs));
    std::memcpy(m_volume_data_upload.data(), m_gpu_volume_descs.data(), sizeof(m_gpu_volume_descs));
}

ProbeUpdateParam ProbeVolumeResource::BuildUpdateParam(
    const Snapshot& snapshot,
    const VolumeSnapshot& volume,
    const Scene&    scene,
    uint64          frame_index,
    bool            history_valid
) const {
    float3 main_light_direction;
    float  main_light_intensity = 0.0f;
    float3 main_light_color     = GetMainLightColor(scene, main_light_direction, main_light_intensity);

    ProbeUpdateParam param{};
    param.probe_volume_counts  = uint4(volume.count_x, volume.count_y, volume.count_z, volume.probe_offset);
    param.probe_volume_origin  = float4(
        volume.origin.x,
        volume.origin.y,
        volume.origin.z,
        history_valid ? snapshot.irradiance_hysteresis : 0.0f
    );
    param.probe_volume_spacing = float4(
        volume.spacing.x,
        volume.spacing.y,
        volume.spacing.z,
        history_valid ? snapshot.visibility_hysteresis : 0.0f
    );
    param.probe_sky_color      = float4(snapshot.sky_color.x, snapshot.sky_color.y, snapshot.sky_color.z, snapshot.sky_intensity);
    param.probe_ground_color =
        float4(snapshot.ground_color.x, snapshot.ground_color.y, snapshot.ground_color.z, snapshot.directional_bounce);
    param.main_light_direction =
        float4(main_light_direction.x, main_light_direction.y, main_light_direction.z, float(frame_index % 1024u) / 1024.0f);
    param.main_light_color = float4(main_light_color.x, main_light_color.y, main_light_color.z, main_light_intensity);
    param.probe_trace_config =
        float4(snapshot.trace_distance, snapshot.visibility_bias, float(snapshot.trace_ray_count), 0.0f);
    return param;
}

ProbeVolumeResource::UpdateInfo
ProbeVolumeResource::PrepareUpdate(const RasterConfig& config, const Scene& scene, const uint64 frame_index) {
    UpdateInfo update_info{};

    if (m_probe_buffer.buf == nullptr || m_volume_buffer.buf == nullptr ||
        m_visibility_atlas_buffer.buf == nullptr || m_irradiance_atlas_buffer.buf == nullptr) {
        return update_info;
    }

    const Snapshot snapshot = BuildSnapshot(config);
    const bool     changed  = HasSnapshotChanged(snapshot);

    if (!snapshot.enabled || snapshot.volume_count == 0 || snapshot.total_count == 0) {
        m_history_valid.fill(false);
        m_snapshot     = snapshot;
        m_has_snapshot = true;
        if (changed) {
            StageVolumeUpload(snapshot);
        }
        if (changed) {
            LOG_DEBUG("[ProbeGI] Probe volume system disabled or has no active volumes.");
        }
        return update_info;
    }

    update_info.enabled           = true;
    update_info.volume_count      = snapshot.volume_count;
    update_info.total_probe_count = snapshot.total_count;

    StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT> next_history_valid{};
    for (uint volume_index = 0; volume_index < snapshot.volume_count; ++volume_index) {
        const bool reset_history = RequiresHistoryReset(snapshot, volume_index);
        const VolumeSnapshot& volume = snapshot.volumes[volume_index];
        UpdateJob&            job    = update_info.jobs[volume_index];
        job.volume_index = volume_index;
        job.probe_count  = volume.total_count;
        job.param        = BuildUpdateParam(snapshot, volume, scene, frame_index, !reset_history);
        next_history_valid[volume_index] = true;

        if (changed || reset_history) {
            LOG_DEBUG(
                "[ProbeGI] Volume update scheduled: slot={}, config_index={}, count=({}, {}, {}) total={}, offset={}, origin={}, extent={}, spacing={}, intensity={}, blend_distance={}, history_reset={}",
                volume_index,
                volume.config_index,
                volume.count_x,
                volume.count_y,
                volume.count_z,
                volume.total_count,
                volume.probe_offset,
                volume.origin.ToString(2),
                volume.extent.ToString(2),
                volume.spacing.ToString(2),
                volume.intensity,
                volume.blend_distance,
                reset_history ? 1 : 0
            );
        }
    }

    if (changed) {
        LOG_DEBUG(
            "[ProbeGI] Multi-volume layout updated: active_volumes={}, total_probes={}/{}, trace_distance={}, trace_rays={}, hysteresis=({}, {}).",
            snapshot.volume_count,
            snapshot.total_count,
            RASTER_PROBE_MAX_COUNT,
            snapshot.trace_distance,
            snapshot.trace_ray_count,
            snapshot.irradiance_hysteresis,
            snapshot.visibility_hysteresis
        );
        StageVolumeUpload(snapshot);
    }

    m_snapshot       = snapshot;
    m_has_snapshot   = true;
    m_history_valid  = next_history_valid;
    return update_info;
}

void ProbeVolumeResource::FillLightingData(LightingData& lighting_data) const {
    const uint enabled =
        (m_has_snapshot && m_snapshot.enabled && m_probe_buffer.hdl != 0 && m_volume_buffer.hdl != 0 &&
         m_visibility_atlas_buffer.hdl != 0 && m_irradiance_atlas_buffer.hdl != 0 &&
         m_snapshot.volume_count > 0 && m_snapshot.total_count > 0) ?
            1u :
            0u;

    lighting_data.probe_system_config =
        uint4(enabled, m_snapshot.debug_mode, m_probe_buffer.hdl, m_volume_buffer.hdl);
    lighting_data.probe_system_counts =
        uint4(m_snapshot.volume_count, m_snapshot.total_count, 0u, 0u);
    lighting_data.probe_system_atlas = uint4(
        m_visibility_atlas_buffer.hdl,
        m_irradiance_atlas_buffer.hdl,
        m_irradiance_atlas_texture.hdl,
        m_visibility_atlas_texture.hdl
    );
    lighting_data.probe_system_debug = float4(m_snapshot.debug_scale, 0.0f, 0.0f, 0.0f);
}

} // namespace Moer::Render::Raster
