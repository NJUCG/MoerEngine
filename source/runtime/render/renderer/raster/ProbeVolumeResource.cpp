#include "ProbeVolumeResource.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

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

    constexpr uint brick_buffer_byte_size = sizeof(ProbeBrickGpuDesc) * RASTER_PROBE_MAX_BRICK_COUNT;
    m_brick_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::BrickDescriptors",
        brick_buffer_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_brick_buffer.hdl = bdls->AllocateBuffer(m_brick_buffer.buf->GetView());

    constexpr uint page_table_byte_size = sizeof(uint) * RASTER_PROBE_MAX_BRICK_COUNT;
    m_page_table_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::BrickPageTable",
        page_table_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_page_table_buffer.hdl = bdls->AllocateBuffer(m_page_table_buffer.buf->GetView());

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
        "[ProbeGI] Created probe buffers: max_volumes={}, max_count={}, probe_byte_size={}, probe_handle={}, volume_desc_byte_size={}, volume_desc_handle={}, max_bricks={}, brick_desc_byte_size={}, brick_desc_handle={}, page_table_byte_size={}, page_table_handle={}, visibility_dim={}x{}, visibility_byte_size={}, visibility_handle={}, irradiance_dim={}x{}, irradiance_byte_size={}, irradiance_handle={}, atlas_texture={}x{}, visibility_texture_handle={}, irradiance_texture_handle={}, scene_data_byte_size={}",
        RASTER_PROBE_VOLUME_MAX_COUNT,
        RASTER_PROBE_MAX_COUNT,
        buffer_byte_size,
        m_probe_buffer.hdl,
        volume_buffer_byte_size,
        m_volume_buffer.hdl,
        RASTER_PROBE_MAX_BRICK_COUNT,
        brick_buffer_byte_size,
        m_brick_buffer.hdl,
        page_table_byte_size,
        m_page_table_buffer.hdl,
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
    if (m_brick_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_brick_buffer.hdl);
        m_brick_buffer.hdl = 0;
    }
    if (m_page_table_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_page_table_buffer.hdl);
        m_page_table_buffer.hdl = 0;
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
    m_brick_buffer.buf = nullptr;
    m_page_table_buffer.buf = nullptr;
    m_visibility_atlas_buffer.buf = nullptr;
    m_irradiance_atlas_buffer.buf = nullptr;
    m_visibility_atlas_texture.tex = nullptr;
    m_irradiance_atlas_texture.tex = nullptr;
    m_scene_data_buffer = nullptr;
    m_scene_data_upload.clear();
    m_volume_data_upload.clear();
    m_brick_data_upload.clear();
    m_page_table_upload.clear();
    m_has_snapshot = false;
    m_history_valid.fill(false);
    m_brick_history_valid.fill(false);
    m_brick_last_update_frame.fill(0);
    m_last_scheduled_brick_count = 0;
    m_last_scheduled_probe_count = 0;
}

void ProbeVolumeResource::UpdateSceneData(CommandList& cmd_list, const Scene& scene) {
    if (m_scene_data_buffer == nullptr || m_volume_buffer.buf == nullptr || m_brick_buffer.buf == nullptr ||
        m_page_table_buffer.buf == nullptr) {
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

    if (!m_brick_data_upload.empty()) {
        cmd_list.CopyFrom(
            std::move(m_brick_data_upload),
            m_brick_buffer.buf->GetView(),
            "Raster::ProbeVolume::Brick Descriptor Upload"
        );
        m_brick_data_upload.clear();
    }

    if (!m_page_table_upload.empty()) {
        cmd_list.CopyFrom(
            std::move(m_page_table_upload),
            m_page_table_buffer.buf->GetView(),
            "Raster::ProbeVolume::Brick Page Table Upload"
        );
        m_page_table_upload.clear();
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

ProbeVolumeResource::Snapshot
ProbeVolumeResource::BuildSnapshot(const RasterConfig& config, float3 camera_position, uint64 frame_index) const {
    Snapshot snapshot{};
    snapshot.enabled                    = config.probe_gi_enabled;
    snapshot.sparse_bricks_enabled     = config.probe_gi_sparse_bricks_enabled;
    snapshot.brick_resident_distance   = Max(config.probe_gi_brick_resident_distance, 0.1f);
    snapshot.brick_resident_hysteresis = Max(config.probe_gi_brick_resident_hysteresis, 0.0f);
    snapshot.update_scheduler_enabled  = config.probe_gi_update_scheduler_enabled;
    snapshot.update_brick_budget =
        static_cast<uint>(Clamp(config.probe_gi_update_brick_budget, 1, int(RASTER_PROBE_MAX_BRICK_COUNT)));
    snapshot.debug_mode = static_cast<uint>(Clamp(config.probe_gi_debug_mode, 0, 6));
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
    snapshot.page_table.fill(RASTER_PROBE_PAGE_INVALID);

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
        volume.page_table_offset = snapshot.volume_count * RASTER_PROBE_MAX_BRICKS_PER_VOLUME;
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

        const uint3 brick_counts(
            (volume.count_x + RASTER_PROBE_BRICK_DIM - 1u) / RASTER_PROBE_BRICK_DIM,
            (volume.count_y + RASTER_PROBE_BRICK_DIM - 1u) / RASTER_PROBE_BRICK_DIM,
            (volume.count_z + RASTER_PROBE_BRICK_DIM - 1u) / RASTER_PROBE_BRICK_DIM
        );
        uint  nearest_brick_index = RASTER_PROBE_PAGE_INVALID;
        float nearest_distance_sq = std::numeric_limits<float>::max();
        for (uint brick_z = 0; brick_z < brick_counts.z; ++brick_z) {
            for (uint brick_y = 0; brick_y < brick_counts.y; ++brick_y) {
                for (uint brick_x = 0; brick_x < brick_counts.x; ++brick_x) {
                    const uint logical_brick_index =
                        brick_x + brick_y * brick_counts.x + brick_z * brick_counts.x * brick_counts.y;
                    const uint page_index = volume.page_table_offset + logical_brick_index;
                    const uint brick_index = snapshot.brick_count;

                    BrickSnapshot& brick = snapshot.bricks[brick_index];
                    brick.coord           = uint3(brick_x, brick_y, brick_z);
                    brick.volume_index    = snapshot.volume_count;
                    brick.probe_offset    = snapshot.total_count;
                    brick.page_index      = page_index;

                    const uint3 brick_start = brick.coord * RASTER_PROBE_BRICK_DIM;
                    brick.local_counts = uint3(
                        Min(RASTER_PROBE_BRICK_DIM, volume.count_x - brick_start.x),
                        Min(RASTER_PROBE_BRICK_DIM, volume.count_y - brick_start.y),
                        Min(RASTER_PROBE_BRICK_DIM, volume.count_z - brick_start.z)
                    );
                    brick.probe_count = brick.local_counts.x * brick.local_counts.y * brick.local_counts.z;
                    const float3 brick_center_coord(
                        float(brick_start.x) + float(brick.local_counts.x - 1u) * 0.5f,
                        float(brick_start.y) + float(brick.local_counts.y - 1u) * 0.5f,
                        float(brick_start.z) + float(brick.local_counts.z - 1u) * 0.5f
                    );
                    const float3 brick_center = volume.origin + volume.spacing * brick_center_coord;
                    const float distance_sq = SquaredLengthf(camera_position - brick_center);
                    brick.camera_distance_sq = distance_sq;
                    brick.update_age = m_brick_history_valid[brick_index] ?
                                           static_cast<uint>(std::min<uint64>(
                                               frame_index - m_brick_last_update_frame[brick_index],
                                               255u
                                           )) :
                                           255u;
                    float resident_distance = snapshot.brick_resident_distance;
                    if (snapshot.sparse_bricks_enabled && m_has_snapshot &&
                        m_snapshot.sparse_bricks_enabled && brick_index < m_snapshot.brick_count) {
                        const BrickSnapshot& previous_brick = m_snapshot.bricks[brick_index];
                        const bool same_brick = previous_brick.volume_index == brick.volume_index &&
                                                previous_brick.coord == brick.coord &&
                                                previous_brick.local_counts == brick.local_counts;
                        if (same_brick && previous_brick.resident) {
                            resident_distance += snapshot.brick_resident_hysteresis;
                        }
                    }
                    brick.resident = !snapshot.sparse_bricks_enabled ||
                                     distance_sq <= resident_distance * resident_distance;

                    if (distance_sq < nearest_distance_sq) {
                        nearest_distance_sq = distance_sq;
                        nearest_brick_index = brick_index;
                    }

                    if (brick.resident) {
                        snapshot.page_table[page_index] = brick_index;
                        ++snapshot.resident_brick_count;
                        snapshot.resident_probe_count += brick.probe_count;
                        ++volume.resident_brick_count;
                        volume.resident_probe_count += brick.probe_count;
                    }
                    snapshot.total_count += brick.probe_count;
                    ++snapshot.brick_count;
                    ++volume.brick_count;
                }
            }
        }

        if (snapshot.sparse_bricks_enabled && volume.resident_brick_count == 0u &&
            nearest_brick_index != RASTER_PROBE_PAGE_INVALID) {
            BrickSnapshot& nearest_brick = snapshot.bricks[nearest_brick_index];
            nearest_brick.resident = true;
            snapshot.page_table[nearest_brick.page_index] = nearest_brick_index;
            ++snapshot.resident_brick_count;
            snapshot.resident_probe_count += nearest_brick.probe_count;
            ++volume.resident_brick_count;
            volume.resident_probe_count += nearest_brick.probe_count;
        }

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

    if (m_snapshot.enabled != snapshot.enabled ||
        m_snapshot.sparse_bricks_enabled != snapshot.sparse_bricks_enabled ||
        m_snapshot.debug_mode != snapshot.debug_mode ||
        m_snapshot.volume_count != snapshot.volume_count || m_snapshot.brick_count != snapshot.brick_count ||
        m_snapshot.resident_brick_count != snapshot.resident_brick_count ||
        m_snapshot.resident_probe_count != snapshot.resident_probe_count ||
        m_snapshot.total_count != snapshot.total_count ||
        !NearlyEqual(m_snapshot.brick_resident_distance, snapshot.brick_resident_distance) ||
        !NearlyEqual(m_snapshot.brick_resident_hysteresis, snapshot.brick_resident_hysteresis) ||
        m_snapshot.update_scheduler_enabled != snapshot.update_scheduler_enabled ||
        m_snapshot.update_brick_budget != snapshot.update_brick_budget ||
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
            lhs.probe_offset != rhs.probe_offset || lhs.page_table_offset != rhs.page_table_offset ||
            lhs.brick_count != rhs.brick_count || lhs.resident_brick_count != rhs.resident_brick_count ||
            lhs.resident_probe_count != rhs.resident_probe_count || !NearlyEqual(lhs.origin, rhs.origin) ||
            !NearlyEqual(lhs.extent, rhs.extent) || !NearlyEqual(lhs.spacing, rhs.spacing) ||
            !NearlyEqual(lhs.intensity, rhs.intensity) || !NearlyEqual(lhs.normal_bias, rhs.normal_bias) ||
            !NearlyEqual(lhs.blend_distance, rhs.blend_distance)) {
            return true;
        }
    }

    for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
        const BrickSnapshot& lhs = m_snapshot.bricks[brick_index];
        const BrickSnapshot& rhs = snapshot.bricks[brick_index];
        if (lhs.coord != rhs.coord || lhs.local_counts != rhs.local_counts ||
            lhs.volume_index != rhs.volume_index || lhs.probe_offset != rhs.probe_offset ||
            lhs.probe_count != rhs.probe_count || lhs.page_index != rhs.page_index ||
            lhs.resident != rhs.resident) {
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
           previous.probe_offset != current.probe_offset ||
           previous.page_table_offset != current.page_table_offset || previous.brick_count != current.brick_count ||
           !NearlyEqual(previous.origin, current.origin) ||
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
        desc.allocation =
            uint4(volume.probe_offset, volume.config_index, volume.page_table_offset, volume.brick_count);
        desc.visibility = float4(
            snapshot.visibility_bias,
            snapshot.visibility_power,
            snapshot.visibility_min_weight,
            snapshot.visibility_strength
        );
    }

    m_volume_data_upload.resize(sizeof(m_gpu_volume_descs));
    std::memcpy(m_volume_data_upload.data(), m_gpu_volume_descs.data(), sizeof(m_gpu_volume_descs));
    StageBrickUpload(snapshot);
    m_page_table_upload.resize(sizeof(snapshot.page_table));
    std::memcpy(m_page_table_upload.data(), snapshot.page_table.data(), sizeof(snapshot.page_table));
}

void ProbeVolumeResource::StageBrickUpload(const Snapshot& snapshot) {
    m_gpu_brick_descs.fill({});
    for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
        const BrickSnapshot& brick = snapshot.bricks[brick_index];
        ProbeBrickGpuDesc&   desc  = m_gpu_brick_descs[brick_index];
        desc.coord_volume = uint4(brick.coord.x, brick.coord.y, brick.coord.z, brick.volume_index);
        desc.probe_range =
            uint4(brick.probe_offset, brick.probe_count, brick.resident ? 1u : 0u, brick.page_index);
        desc.local_counts =
            uint4(brick.local_counts.x, brick.local_counts.y, brick.local_counts.z, brick.update_age);
    }

    m_brick_data_upload.resize(sizeof(m_gpu_brick_descs));
    std::memcpy(m_brick_data_upload.data(), m_gpu_brick_descs.data(), sizeof(m_gpu_brick_descs));
}

ProbeUpdateParam ProbeVolumeResource::BuildUpdateParam(
    const Snapshot& snapshot,
    const VolumeSnapshot& volume,
    uint            volume_index,
    uint            brick_index,
    const Scene&    scene,
    bool            history_valid
) const {
    float3 main_light_direction;
    float  main_light_intensity = 0.0f;
    float3 main_light_color     = GetMainLightColor(scene, main_light_direction, main_light_intensity);

    ProbeUpdateParam param{};
    param.probe_volume_counts  = uint4(volume.count_x, volume.count_y, volume.count_z, volume_index);
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
        float4(main_light_direction.x, main_light_direction.y, main_light_direction.z, float(brick_index));
    param.main_light_color = float4(main_light_color.x, main_light_color.y, main_light_color.z, main_light_intensity);
    param.probe_trace_config =
        float4(snapshot.trace_distance, snapshot.visibility_bias, float(snapshot.trace_ray_count), 0.0f);
    return param;
}

ProbeVolumeResource::UpdateInfo
ProbeVolumeResource::PrepareUpdate(
    const RasterConfig& config,
    const Scene&        scene,
    float3              camera_position,
    uint64              frame_index
) {
    UpdateInfo update_info{};
    m_last_scheduled_brick_count = 0;
    m_last_scheduled_probe_count = 0;

    if (m_probe_buffer.buf == nullptr || m_volume_buffer.buf == nullptr || m_brick_buffer.buf == nullptr ||
        m_page_table_buffer.buf == nullptr ||
        m_visibility_atlas_buffer.buf == nullptr || m_irradiance_atlas_buffer.buf == nullptr) {
        return update_info;
    }

    Snapshot   snapshot = BuildSnapshot(config, camera_position, frame_index);
    const bool changed  = HasSnapshotChanged(snapshot);

    if (!snapshot.enabled || snapshot.volume_count == 0 || snapshot.total_count == 0) {
        m_history_valid.fill(false);
        m_brick_history_valid.fill(false);
        m_brick_last_update_frame.fill(0);
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

    update_info.enabled              = true;
    update_info.volume_count         = snapshot.volume_count;
    update_info.resident_brick_count = snapshot.resident_brick_count;
    update_info.resident_probe_count = snapshot.resident_probe_count;

    StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT> volume_history_reset{};
    StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT> next_history_valid{};
    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT>  next_brick_history_valid{};
    for (uint volume_index = 0; volume_index < snapshot.volume_count; ++volume_index) {
        const bool volume_reset = RequiresHistoryReset(snapshot, volume_index);
        const VolumeSnapshot& volume = snapshot.volumes[volume_index];
        volume_history_reset[volume_index] = volume_reset;
        next_history_valid[volume_index] = true;

        if (changed || volume_reset) {
            LOG_DEBUG(
                "[ProbeGI] Volume residency updated: slot={}, config_index={}, count=({}, {}, {}) total={}, offset={}, resident_bricks={}/{}, resident_probes={}, page_table_offset={}, origin={}, extent={}, spacing={}, intensity={}, blend_distance={}, history_reset={}",
                volume_index,
                volume.config_index,
                volume.count_x,
                volume.count_y,
                volume.count_z,
                volume.total_count,
                volume.probe_offset,
                volume.resident_brick_count,
                volume.brick_count,
                volume.resident_probe_count,
                volume.page_table_offset,
                volume.origin.ToString(2),
                volume.extent.ToString(2),
                volume.spacing.ToString(2),
                volume.intensity,
                volume.blend_distance,
                volume_reset ? 1 : 0
            );
        }
    }

    Array<uint> resident_candidates;
    resident_candidates.reserve(snapshot.resident_brick_count);
    for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
        const BrickSnapshot& brick = snapshot.bricks[brick_index];
        if (!brick.resident) {
            continue;
        }

        resident_candidates.push_back(brick_index);
        next_brick_history_valid[brick_index] =
            !volume_history_reset[brick.volume_index] && m_brick_history_valid[brick_index];
    }

    std::sort(
        resident_candidates.begin(),
        resident_candidates.end(),
        [&](uint lhs_index, uint rhs_index) {
            const BrickSnapshot& lhs = snapshot.bricks[lhs_index];
            const BrickSnapshot& rhs = snapshot.bricks[rhs_index];
            const bool lhs_mandatory =
                volume_history_reset[lhs.volume_index] || !m_brick_history_valid[lhs_index];
            const bool rhs_mandatory =
                volume_history_reset[rhs.volume_index] || !m_brick_history_valid[rhs_index];
            if (lhs_mandatory != rhs_mandatory) {
                return lhs_mandatory;
            }
            if (lhs.update_age != rhs.update_age) {
                return lhs.update_age > rhs.update_age;
            }
            if (!NearlyEqual(lhs.camera_distance_sq, rhs.camera_distance_sq)) {
                return lhs.camera_distance_sq < rhs.camera_distance_sq;
            }
            return lhs_index < rhs_index;
        }
    );

    for (uint brick_index : resident_candidates) {
        BrickSnapshot& brick = snapshot.bricks[brick_index];
        const bool mandatory =
            volume_history_reset[brick.volume_index] || !m_brick_history_valid[brick_index];
        if (snapshot.update_scheduler_enabled && !mandatory &&
            update_info.job_count >= snapshot.update_brick_budget) {
            ++update_info.deferred_brick_count;
            continue;
        }
        if (update_info.job_count >= update_info.jobs.size()) {
            break;
        }

        const VolumeSnapshot& volume = snapshot.volumes[brick.volume_index];
        UpdateJob& job = update_info.jobs[update_info.job_count++];
        job.volume_index = brick.volume_index;
        job.brick_index  = brick_index;
        job.probe_count  = brick.probe_count;
        job.param = BuildUpdateParam(snapshot, volume, brick.volume_index, brick_index, scene, !mandatory);
        update_info.scheduled_probe_count += brick.probe_count;
        next_brick_history_valid[brick_index] = true;
        m_brick_last_update_frame[brick_index] = frame_index;
        brick.update_age = 0;
    }

    m_last_scheduled_brick_count = update_info.job_count;
    m_last_scheduled_probe_count = update_info.scheduled_probe_count;

    if (changed) {
        LOG_DEBUG(
            "[ProbeGI] Multi-volume brick residency updated: active_volumes={}, resident_bricks={}/{}, resident_probes={}/{}, sparse={}, resident_distance={}, resident_hysteresis={}, scheduler={}, update_budget={}, scheduled_bricks={}, scheduled_probes={}, deferred_bricks={}, trace_distance={}, trace_rays={}, history_hysteresis=({}, {}).",
            snapshot.volume_count,
            snapshot.resident_brick_count,
            snapshot.brick_count,
            snapshot.resident_probe_count,
            snapshot.total_count,
            snapshot.sparse_bricks_enabled ? 1 : 0,
            snapshot.brick_resident_distance,
            snapshot.brick_resident_hysteresis,
            snapshot.update_scheduler_enabled ? 1 : 0,
            snapshot.update_brick_budget,
            update_info.job_count,
            update_info.scheduled_probe_count,
            update_info.deferred_brick_count,
            snapshot.trace_distance,
            snapshot.trace_ray_count,
            snapshot.irradiance_hysteresis,
            snapshot.visibility_hysteresis
        );
        StageVolumeUpload(snapshot);
    } else if (snapshot.debug_mode == 6u) {
        StageBrickUpload(snapshot);
    }

    m_snapshot            = snapshot;
    m_has_snapshot        = true;
    m_history_valid       = next_history_valid;
    m_brick_history_valid = next_brick_history_valid;
    return update_info;
}

void ProbeVolumeResource::FillLightingData(LightingData& lighting_data) const {
    const uint enabled =
        (m_has_snapshot && m_snapshot.enabled && m_probe_buffer.hdl != 0 && m_volume_buffer.hdl != 0 &&
         m_brick_buffer.hdl != 0 && m_page_table_buffer.hdl != 0 && m_visibility_atlas_buffer.hdl != 0 &&
         m_irradiance_atlas_buffer.hdl != 0 &&
         m_snapshot.volume_count > 0 && m_snapshot.total_count > 0) ?
            1u :
            0u;

    lighting_data.probe_system_config =
        uint4(enabled, m_snapshot.debug_mode, m_probe_buffer.hdl, m_volume_buffer.hdl);
    lighting_data.probe_system_counts = uint4(
        m_snapshot.volume_count,
        m_snapshot.total_count,
        m_brick_buffer.hdl,
        m_page_table_buffer.hdl
    );
    lighting_data.probe_system_atlas = uint4(
        m_visibility_atlas_buffer.hdl,
        m_irradiance_atlas_buffer.hdl,
        m_irradiance_atlas_texture.hdl,
        m_visibility_atlas_texture.hdl
    );
    lighting_data.probe_system_debug = float4(m_snapshot.debug_scale, 0.0f, 0.0f, 0.0f);
}

} // namespace Moer::Render::Raster
