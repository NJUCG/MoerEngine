#include "ProbeVolumeResource.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"

#include <cmath>

namespace Moer::Render::Raster {
namespace {

uint ClampProbeCount(int value) {
    return static_cast<uint>(Clamp(value, 1, 16));
}

void ClampProbeBudget(uint& count_x, uint& count_y, uint& count_z) {
    while (count_x * count_y * count_z > RASTER_PROBE_MAX_COUNT) {
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

    constexpr uint visibility_atlas_byte_size =
        sizeof(ProbeGridVisibilityTexel) * RASTER_PROBE_MAX_COUNT * RASTER_PROBE_VISIBILITY_ATLAS_TEXEL_COUNT;
    m_visibility_atlas_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::VisibilityAtlas",
        visibility_atlas_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_visibility_atlas_buffer.hdl = bdls->AllocateBuffer(m_visibility_atlas_buffer.buf->GetView());

    LOG_DEBUG(
        "[ProbeGI] Created probe buffers: max_count={}, probe_byte_size={}, probe_handle={}, visibility_dim={}x{}, visibility_byte_size={}, visibility_handle={}",
        RASTER_PROBE_MAX_COUNT,
        buffer_byte_size,
        m_probe_buffer.hdl,
        RASTER_PROBE_VISIBILITY_ATLAS_DIM,
        RASTER_PROBE_VISIBILITY_ATLAS_DIM,
        visibility_atlas_byte_size,
        m_visibility_atlas_buffer.hdl
    );
}

void ProbeVolumeResource::Destroy(BindlessArrayRef& bdls) {
    if (m_probe_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_probe_buffer.hdl);
        m_probe_buffer.hdl = 0;
    }
    if (m_visibility_atlas_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_visibility_atlas_buffer.hdl);
        m_visibility_atlas_buffer.hdl = 0;
    }
    m_probe_buffer.buf = nullptr;
    m_visibility_atlas_buffer.buf = nullptr;
    m_has_snapshot = false;
    m_history_valid = false;
}

ProbeVolumeResource::Snapshot ProbeVolumeResource::BuildSnapshot(const RasterConfig& config) const {
    Snapshot snapshot{};
    snapshot.enabled     = config.probe_gi_enabled;
    snapshot.debug_mode  = static_cast<uint>(Clamp(config.probe_gi_debug_mode, 0, 4));
    snapshot.count_x     = ClampProbeCount(config.probe_gi_count_x);
    snapshot.count_y     = ClampProbeCount(config.probe_gi_count_y);
    snapshot.count_z     = ClampProbeCount(config.probe_gi_count_z);

    const uint requested_count = snapshot.count_x * snapshot.count_y * snapshot.count_z;
    ClampProbeBudget(snapshot.count_x, snapshot.count_y, snapshot.count_z);

    snapshot.total_count = snapshot.count_x * snapshot.count_y * snapshot.count_z;
    snapshot.origin      = config.probe_gi_volume_origin;
    snapshot.extent      = SanitizeExtent(config.probe_gi_volume_extent);
    snapshot.spacing     = float3(
        snapshot.count_x > 1 ? snapshot.extent.x / float(snapshot.count_x - 1) : snapshot.extent.x,
        snapshot.count_y > 1 ? snapshot.extent.y / float(snapshot.count_y - 1) : snapshot.extent.y,
        snapshot.count_z > 1 ? snapshot.extent.z / float(snapshot.count_z - 1) : snapshot.extent.z
    );

    snapshot.intensity          = Max(config.probe_gi_intensity, 0.0f);
    snapshot.normal_bias        = Max(config.probe_gi_normal_bias, 0.0f);
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

    if (requested_count != snapshot.total_count) {
        LOG_DEBUG(
            "[ProbeGI] Probe count clamped from {} to {} to fit max_count={}.",
            requested_count,
            snapshot.total_count,
            RASTER_PROBE_MAX_COUNT
        );
    }

    return snapshot;
}

bool ProbeVolumeResource::HasSnapshotChanged(const Snapshot& snapshot) const {
    if (!m_has_snapshot) {
        return true;
    }

    return m_snapshot.enabled != snapshot.enabled || m_snapshot.debug_mode != snapshot.debug_mode ||
           m_snapshot.count_x != snapshot.count_x || m_snapshot.count_y != snapshot.count_y ||
           m_snapshot.count_z != snapshot.count_z || !NearlyEqual(m_snapshot.origin, snapshot.origin) ||
           !NearlyEqual(m_snapshot.extent, snapshot.extent) || !NearlyEqual(m_snapshot.spacing, snapshot.spacing) ||
           !NearlyEqual(m_snapshot.intensity, snapshot.intensity) ||
           !NearlyEqual(m_snapshot.normal_bias, snapshot.normal_bias) ||
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
           !NearlyEqual(m_snapshot.ground_color, snapshot.ground_color);
}

bool ProbeVolumeResource::RequiresHistoryReset(const Snapshot& snapshot) const {
    if (!m_has_snapshot || !m_history_valid) {
        return true;
    }

    return m_snapshot.enabled != snapshot.enabled || m_snapshot.count_x != snapshot.count_x ||
           m_snapshot.count_y != snapshot.count_y || m_snapshot.count_z != snapshot.count_z ||
           !NearlyEqual(m_snapshot.origin, snapshot.origin) || !NearlyEqual(m_snapshot.extent, snapshot.extent) ||
           !NearlyEqual(m_snapshot.spacing, snapshot.spacing) ||
           !NearlyEqual(m_snapshot.trace_distance, snapshot.trace_distance) ||
           m_snapshot.trace_ray_count != snapshot.trace_ray_count ||
           !NearlyEqual(m_snapshot.visibility_bias, snapshot.visibility_bias) ||
           !NearlyEqual(m_snapshot.sky_intensity, snapshot.sky_intensity) ||
           !NearlyEqual(m_snapshot.directional_bounce, snapshot.directional_bounce) ||
           !NearlyEqual(m_snapshot.sky_color, snapshot.sky_color) ||
           !NearlyEqual(m_snapshot.ground_color, snapshot.ground_color);
}

ProbeUpdateParam ProbeVolumeResource::BuildUpdateParam(
    const Snapshot& snapshot,
    const Scene&    scene,
    uint64          frame_index,
    bool            history_valid
) const {
    float3 main_light_direction;
    float  main_light_intensity = 0.0f;
    float3 main_light_color     = GetMainLightColor(scene, main_light_direction, main_light_intensity);

    ProbeUpdateParam param{};
    param.probe_volume_counts  = uint4(snapshot.count_x, snapshot.count_y, snapshot.count_z, snapshot.total_count);
    param.probe_volume_origin  = float4(
        snapshot.origin.x,
        snapshot.origin.y,
        snapshot.origin.z,
        history_valid ? snapshot.irradiance_hysteresis : 0.0f
    );
    param.probe_volume_spacing = float4(
        snapshot.spacing.x,
        snapshot.spacing.y,
        snapshot.spacing.z,
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

    if (m_probe_buffer.buf == nullptr || m_visibility_atlas_buffer.buf == nullptr) {
        return update_info;
    }

    const Snapshot snapshot      = BuildSnapshot(config);
    const bool     changed       = HasSnapshotChanged(snapshot);
    const bool     reset_history = RequiresHistoryReset(snapshot);

    m_snapshot     = snapshot;
    m_has_snapshot = true;

    if (!snapshot.enabled) {
        m_history_valid = false;
        if (changed) {
            LOG_DEBUG("[ProbeGI] Probe volume disabled.");
        }
        return update_info;
    }

    if (changed) {
        LOG_DEBUG(
            "[ProbeGI] Probe volume update scheduled: count=({}, {}, {}) total={}, origin={}, extent={}, intensity={}, trace_distance={}, trace_rays={}, history_reset={}, hysteresis=({}, {})",
            snapshot.count_x,
            snapshot.count_y,
            snapshot.count_z,
            snapshot.total_count,
            snapshot.origin.ToString(2),
            snapshot.extent.ToString(2),
            snapshot.intensity,
            snapshot.trace_distance,
            snapshot.trace_ray_count,
            reset_history ? 1 : 0,
            snapshot.irradiance_hysteresis,
            snapshot.visibility_hysteresis
        );
    }

    update_info.enabled     = true;
    update_info.probe_count = snapshot.total_count;
    update_info.param       = BuildUpdateParam(snapshot, scene, frame_index, !reset_history);
    m_history_valid         = true;
    return update_info;
}

void ProbeVolumeResource::FillLightingData(LightingData& lighting_data) const {
    const uint enabled =
        (m_has_snapshot && m_snapshot.enabled && m_probe_buffer.hdl != 0 && m_visibility_atlas_buffer.hdl != 0 &&
         m_snapshot.total_count > 0) ?
            1u :
            0u;

    lighting_data.probe_volume_origin =
        float4(m_snapshot.origin.x, m_snapshot.origin.y, m_snapshot.origin.z, m_snapshot.normal_bias);
    lighting_data.probe_volume_spacing =
        float4(m_snapshot.spacing.x, m_snapshot.spacing.y, m_snapshot.spacing.z, m_snapshot.intensity);
    lighting_data.probe_volume_extent =
        float4(m_snapshot.extent.x, m_snapshot.extent.y, m_snapshot.extent.z, m_snapshot.debug_scale);
    lighting_data.probe_volume_counts =
        uint4(m_snapshot.count_x, m_snapshot.count_y, m_snapshot.count_z, m_snapshot.total_count);
    lighting_data.probe_volume_config =
        uint4(enabled, m_snapshot.debug_mode, m_probe_buffer.hdl, m_visibility_atlas_buffer.hdl);
    lighting_data.probe_volume_visibility = float4(
        m_snapshot.visibility_bias,
        m_snapshot.visibility_power,
        m_snapshot.visibility_min_weight,
        m_snapshot.visibility_strength
    );
}

} // namespace Moer::Render::Raster
