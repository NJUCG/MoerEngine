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

float SafeSaturate(float value) {
    return Clamp(value, 0.0f, 1.0f);
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
    m_cpu_probes.resize(RASTER_PROBE_MAX_COUNT);

    LOG_DEBUG(
        "[ProbeGI] Created probe buffer: max_count={}, byte_size={}, bindless_handle={}",
        RASTER_PROBE_MAX_COUNT,
        buffer_byte_size,
        m_probe_buffer.hdl
    );
}

void ProbeVolumeResource::Destroy(BindlessArrayRef& bdls) {
    if (m_probe_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_probe_buffer.hdl);
        m_probe_buffer.hdl = 0;
    }
    m_probe_buffer.buf = nullptr;
    m_cpu_probes.clear();
    m_has_snapshot = false;
}

ProbeVolumeResource::Snapshot ProbeVolumeResource::BuildSnapshot(const RasterConfig& config) const {
    Snapshot snapshot{};
    snapshot.enabled     = config.probe_gi_enabled;
    snapshot.debug_mode  = static_cast<uint>(Clamp(config.probe_gi_debug_mode, 0, 2));
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
           !NearlyEqual(m_snapshot.debug_scale, snapshot.debug_scale) ||
           !NearlyEqual(m_snapshot.sky_intensity, snapshot.sky_intensity) ||
           !NearlyEqual(m_snapshot.directional_bounce, snapshot.directional_bounce) ||
           !NearlyEqual(m_snapshot.sky_color, snapshot.sky_color) ||
           !NearlyEqual(m_snapshot.ground_color, snapshot.ground_color);
}

void ProbeVolumeResource::BuildProbeData(const Snapshot& snapshot, const Scene& scene, uint64 frame_index) {
    float3 main_light_direction;
    float  main_light_intensity = 0.0f;
    float3 main_light_color     = GetMainLightColor(scene, main_light_direction, main_light_intensity);

    const float3 up                  = float3(0.0f, 1.0f, 0.0f);
    const float  sun_height          = SafeSaturate(Dotf(-main_light_direction, up));
    const float3 directional_bounce  = main_light_color * main_light_intensity * snapshot.directional_bounce *
                                      (0.35f + 0.65f * sun_height);
    const float  frame_phase         = static_cast<float>(frame_index % 1024) * 0.0009765625f;

    uint probe_index = 0;
    for (uint z = 0; z < snapshot.count_z; ++z) {
        const float z01 = snapshot.count_z > 1 ? float(z) / float(snapshot.count_z - 1) : 0.5f;
        for (uint y = 0; y < snapshot.count_y; ++y) {
            const float y01 = snapshot.count_y > 1 ? float(y) / float(snapshot.count_y - 1) : 0.5f;
            for (uint x = 0; x < snapshot.count_x; ++x) {
                const float x01 = snapshot.count_x > 1 ? float(x) / float(snapshot.count_x - 1) : 0.5f;

                const float3 position = snapshot.origin +
                                        float3(
                                            snapshot.spacing.x * float(x),
                                            snapshot.spacing.y * float(y),
                                            snapshot.spacing.z * float(z)
                                        );

                const float lateral_variation =
                    0.92f + 0.08f * std::sin((x01 * 3.17f + z01 * 2.41f + frame_phase) * PI);
                const float3 sky_term =
                    (snapshot.ground_color * (1.0f - y01) + snapshot.sky_color * y01) * snapshot.sky_intensity;
                const float3 bounced_term = directional_bounce * (0.35f + 0.65f * (1.0f - y01));
                const float3 irradiance   = Max((sky_term + bounced_term) * lateral_variation, float3(0.0f));

                ProbeGridProbeData& probe = m_cpu_probes[probe_index++];
                probe.world_position      = float4(position, 1.0f);
                probe.irradiance          = float4(irradiance, 1.0f);
            }
        }
    }
}

void ProbeVolumeResource::Update(
    CommandList&         cmd_list,
    const RasterConfig&  config,
    const Scene&         scene,
    const uint64         frame_index
) {
    if (m_probe_buffer.buf == nullptr) {
        return;
    }

    const Snapshot snapshot = BuildSnapshot(config);
    const bool     changed  = HasSnapshotChanged(snapshot);

    m_snapshot     = snapshot;
    m_has_snapshot = true;

    if (!snapshot.enabled) {
        if (changed) {
            LOG_DEBUG("[ProbeGI] Probe volume disabled.");
        }
        return;
    }

    BuildProbeData(snapshot, scene, frame_index);

    const uint64 upload_byte_size = sizeof(ProbeGridProbeData) * snapshot.total_count;
    cmd_list.CopyFrom(
        std::span<byte>((byte*)m_cpu_probes.data(), upload_byte_size),
        m_probe_buffer.buf->GetView(0, upload_byte_size),
        "ProbeGI Upload Probe Buffer"
    );

    if (changed) {
        LOG_DEBUG(
            "[ProbeGI] Probe volume updated: count=({}, {}, {}) total={}, origin={}, extent={}, intensity={}",
            snapshot.count_x,
            snapshot.count_y,
            snapshot.count_z,
            snapshot.total_count,
            snapshot.origin.ToString(2),
            snapshot.extent.ToString(2),
            snapshot.intensity
        );
    }
}

void ProbeVolumeResource::FillLightingData(LightingData& lighting_data) const {
    const uint enabled =
        (m_has_snapshot && m_snapshot.enabled && m_probe_buffer.hdl != 0 && m_snapshot.total_count > 0) ? 1u : 0u;

    lighting_data.probe_volume_origin =
        float4(m_snapshot.origin.x, m_snapshot.origin.y, m_snapshot.origin.z, m_snapshot.normal_bias);
    lighting_data.probe_volume_spacing =
        float4(m_snapshot.spacing.x, m_snapshot.spacing.y, m_snapshot.spacing.z, m_snapshot.intensity);
    lighting_data.probe_volume_extent =
        float4(m_snapshot.extent.x, m_snapshot.extent.y, m_snapshot.extent.z, m_snapshot.debug_scale);
    lighting_data.probe_volume_counts =
        uint4(m_snapshot.count_x, m_snapshot.count_y, m_snapshot.count_z, m_snapshot.total_count);
    lighting_data.probe_volume_config = uint4(enabled, m_snapshot.debug_mode, m_probe_buffer.hdl, 0u);
}

} // namespace Moer::Render::Raster
