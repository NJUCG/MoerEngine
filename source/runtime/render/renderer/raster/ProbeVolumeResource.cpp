#include "ProbeVolumeResource.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "math/Transform.h"
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

bool IsFinite(float3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

Box3D TransformBounds(const Box3D& local_bounds, const float4x4& world_transform) {
    Box3D bounds;
    if (!local_bounds.IsValid() || !IsFinite(local_bounds.min) || !IsFinite(local_bounds.max)) {
        return bounds;
    }

    const float3& min = local_bounds.min;
    const float3& max = local_bounds.max;
    const Transform transform(world_transform);
    bounds.Expand(transform * float3(min.x, min.y, min.z));
    bounds.Expand(transform * float3(max.x, min.y, min.z));
    bounds.Expand(transform * float3(min.x, max.y, min.z));
    bounds.Expand(transform * float3(max.x, max.y, min.z));
    bounds.Expand(transform * float3(min.x, min.y, max.z));
    bounds.Expand(transform * float3(max.x, min.y, max.z));
    bounds.Expand(transform * float3(min.x, max.y, max.z));
    bounds.Expand(transform * float3(max.x, max.y, max.z));
    if (!IsFinite(bounds.min) || !IsFinite(bounds.max)) {
        return Box3D();
    }
    return bounds;
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

    constexpr uint cell_buffer_byte_size = sizeof(ProbeCellGpuDesc) * RASTER_PROBE_MAX_CELL_COUNT;
    m_cell_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::CellDescriptors",
        cell_buffer_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_cell_buffer.hdl = bdls->AllocateBuffer(m_cell_buffer.buf->GetView());

    constexpr uint brick_buffer_byte_size = sizeof(ProbeBrickGpuDesc) * RASTER_PROBE_MAX_BRICK_COUNT;
    m_brick_buffer.buf = device.CreateBuffer<byte>(
        "Raster::ProbeVolume::BrickDescriptors",
        brick_buffer_byte_size,
        EBufferUsageFlags::UNORDERED_ACCESS
    );
    m_brick_buffer.hdl = bdls->AllocateBuffer(m_brick_buffer.buf->GetView());

    constexpr uint page_table_byte_size = sizeof(uint) * RASTER_PROBE_MAX_PAGE_COUNT;
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
        "[ProbeGI] Created probe buffers: max_volumes={}, max_count={}, probe_byte_size={}, probe_handle={}, volume_desc_byte_size={}, volume_desc_handle={}, max_cells={}, cell_desc_byte_size={}, cell_desc_handle={}, max_bricks={}, brick_desc_byte_size={}, brick_desc_handle={}, max_pages={}, page_table_byte_size={}, page_table_handle={}, visibility_dim={}x{}, visibility_byte_size={}, visibility_handle={}, irradiance_dim={}x{}, irradiance_byte_size={}, irradiance_handle={}, atlas_texture={}x{}, visibility_texture_handle={}, irradiance_texture_handle={}, scene_data_byte_size={}",
        RASTER_PROBE_VOLUME_MAX_COUNT,
        RASTER_PROBE_MAX_COUNT,
        buffer_byte_size,
        m_probe_buffer.hdl,
        volume_buffer_byte_size,
        m_volume_buffer.hdl,
        RASTER_PROBE_MAX_CELL_COUNT,
        cell_buffer_byte_size,
        m_cell_buffer.hdl,
        RASTER_PROBE_MAX_BRICK_COUNT,
        brick_buffer_byte_size,
        m_brick_buffer.hdl,
        RASTER_PROBE_MAX_PAGE_COUNT,
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
    if (m_cell_buffer.hdl != 0) {
        bdls->UnbindBuffer(m_cell_buffer.hdl);
        m_cell_buffer.hdl = 0;
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
    m_cell_buffer.buf = nullptr;
    m_brick_buffer.buf = nullptr;
    m_page_table_buffer.buf = nullptr;
    m_visibility_atlas_buffer.buf = nullptr;
    m_irradiance_atlas_buffer.buf = nullptr;
    m_visibility_atlas_texture.tex = nullptr;
    m_irradiance_atlas_texture.tex = nullptr;
    m_scene_data_buffer = nullptr;
    m_scene_data_upload.clear();
    m_volume_data_upload.clear();
    m_cell_data_upload.clear();
    m_brick_data_upload.clear();
    m_page_table_upload.clear();
    m_has_snapshot = false;
    m_history_valid.fill(false);
    m_brick_history_valid.fill(false);
    m_brick_last_update_frame.fill(0);
    m_physical_allocations.fill({});
    m_physical_allocator.Reset(0u);
    m_scene_geometry_bounds.clear();
    m_scene_geometry_cache_valid = false;
    m_scene_geometry_generation = 0u;
    m_layout_generation = 0u;
    m_last_scheduled_brick_count = 0;
    m_last_scheduled_probe_count = 0;
}

void ProbeVolumeResource::RefreshSceneGeometry(const Scene& scene) {
    if (!scene.IsReady()) {
        if (m_scene_geometry_cache_valid || !m_scene_geometry_bounds.empty()) {
            m_scene_geometry_bounds.clear();
            m_scene_geometry_cache_valid = false;
            ++m_scene_geometry_generation;
            if (m_scene_geometry_generation == 0u) {
                ++m_scene_geometry_generation;
            }
            LOG_DEBUG(
                "[ProbeGI] Geometry cache invalidated while the scene is not ready: generation={}.",
                m_scene_geometry_generation
            );
        }
        return;
    }

    const Scene::TickState& tick_state = scene.GetLastTickState();
    const bool geometry_changed = tick_state.updated_transform || tick_state.created_transform ||
                                  tick_state.rebuilt_mesh || tick_state.rebuilt_rt_blas;
    if (m_scene_geometry_cache_valid && !geometry_changed) {
        return;
    }

    Array<Box3D> rebuilt_bounds;
    rebuilt_bounds.reserve(m_scene_geometry_bounds.size());
    uint renderable_instance_count = 0u;
    uint leaf_primitive_count      = 0u;
    uint skipped_invalid_count     = 0u;

    const auto& registry = scene.r();
    registry.view<const ecs::CRenderable, const ecs::CNode>().each(
        [&](const auto, const ecs::CRenderable& renderable, const ecs::CNode& node) {
            if (renderable.mesh_entt == entt::null || !registry.valid(renderable.mesh_entt) ||
                !registry.all_of<ecs::CMesh>(renderable.mesh_entt)) {
                ++skipped_invalid_count;
                return;
            }

            ++renderable_instance_count;
            const auto& mesh = registry.get<const ecs::CMesh>(renderable.mesh_entt);
            const uint leaf_count = mesh.num_leaf_clusters > 0u ?
                                        Min(
                                            mesh.num_leaf_clusters,
                                            static_cast<uint>(mesh.primitive_entts.size())
                                        ) :
                                        static_cast<uint>(mesh.primitive_entts.size());
            leaf_primitive_count += leaf_count;
            for (uint primitive_index = 0u; primitive_index < leaf_count; ++primitive_index) {
                const entt::entity primitive_entity = mesh.primitive_entts[primitive_index];
                if (!registry.valid(primitive_entity) || !registry.all_of<ecs::CPrimitive>(primitive_entity)) {
                    ++skipped_invalid_count;
                    continue;
                }

                const auto& primitive = registry.get<const ecs::CPrimitive>(primitive_entity);
                const Box3D world_bounds = TransformBounds(primitive.aabb, node.d_world_transform);
                if (!world_bounds.IsValid()) {
                    ++skipped_invalid_count;
                    continue;
                }
                rebuilt_bounds.push_back(world_bounds);
            }
        }
    );

    m_scene_geometry_bounds = std::move(rebuilt_bounds);
    m_scene_geometry_cache_valid = true;
    ++m_scene_geometry_generation;
    if (m_scene_geometry_generation == 0u) {
        ++m_scene_geometry_generation;
    }
    LOG_DEBUG(
        "[ProbeGI] Geometry cache rebuilt: generation={}, renderable_instances={}, leaf_primitives={}, valid_world_bounds={}, skipped_invalid={}.",
        m_scene_geometry_generation,
        renderable_instance_count,
        leaf_primitive_count,
        m_scene_geometry_bounds.size(),
        skipped_invalid_count
    );
}

void ProbeVolumeResource::UpdateSceneData(CommandList& cmd_list, const Scene& scene) {
    if (m_scene_data_buffer == nullptr || m_volume_buffer.buf == nullptr || m_cell_buffer.buf == nullptr ||
        m_brick_buffer.buf == nullptr || m_page_table_buffer.buf == nullptr) {
        return;
    }

    if (!m_cell_data_upload.empty()) {
        cmd_list.CopyFrom(
            std::move(m_cell_data_upload),
            m_cell_buffer.buf->GetView(),
            "Raster::ProbeVolume::Cell Descriptor Upload"
        );
        m_cell_data_upload.clear();
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
    snapshot.adaptive_placement_enabled =
        snapshot.enabled && config.probe_gi_adaptive_placement_enabled;
    snapshot.hierarchy_enabled = snapshot.enabled && config.probe_gi_adaptive_hierarchy_enabled;
    snapshot.adaptive_geometry_padding = Max(config.probe_gi_adaptive_geometry_padding, 0.0f);
    snapshot.adaptive_fine_occupancy = Clamp(
        config.probe_gi_adaptive_fine_occupancy,
        1.0f / float(RASTER_PROBE_OCCUPANCY_VOXEL_COUNT),
        1.0f
    );
    snapshot.adaptive_fine_primitives =
        static_cast<uint>(Clamp(config.probe_gi_adaptive_fine_primitives, 1, 512));
    snapshot.adaptive_transition_width = Clamp(config.probe_gi_adaptive_transition_width, 0.0f, 4.0f);
    snapshot.geometry_generation =
        snapshot.adaptive_placement_enabled ? m_scene_geometry_generation : 0u;
    snapshot.geometry_primitive_count = snapshot.adaptive_placement_enabled ?
                                            static_cast<uint>(m_scene_geometry_bounds.size()) :
                                            0u;
    snapshot.brick_resident_distance   = Max(config.probe_gi_brick_resident_distance, 0.1f);
    snapshot.brick_resident_hysteresis = Max(config.probe_gi_brick_resident_hysteresis, 0.0f);
    snapshot.update_scheduler_enabled  = config.probe_gi_update_scheduler_enabled;
    snapshot.update_brick_budget =
        static_cast<uint>(Clamp(config.probe_gi_update_brick_budget, 1, int(RASTER_PROBE_MAX_BRICK_COUNT)));
    constexpr uint min_physical_probe_capacity =
        RASTER_PROBE_BRICK_DIM * RASTER_PROBE_BRICK_DIM * RASTER_PROBE_BRICK_DIM;
    snapshot.physical_probe_capacity = static_cast<uint>(Clamp(
        config.probe_gi_physical_probe_capacity,
        int(min_physical_probe_capacity),
        int(RASTER_PROBE_MAX_COUNT)
    ));
    snapshot.debug_mode = static_cast<uint>(Clamp(config.probe_gi_debug_mode, 0, 10));
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
        volume.page_table_offset = config_index * RASTER_PROBE_MAX_PAGES_PER_VOLUME;
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

        const uint3 base_probe_counts(volume.count_x, volume.count_y, volume.count_z);
        const uint3 fine_brick_counts = ProbeAdaptiveLayout::GetLevelBrickCounts(base_probe_counts, 0u);
        const uint3 cell_counts = ProbeAdaptiveLayout::GetCellCounts(fine_brick_counts);
        const Box3D volume_bounds(volume.origin, volume.origin + volume.extent);
        const std::span<const Box3D> geometry_bounds(m_scene_geometry_bounds);
        volume.first_cell_index = snapshot.cell_count;
        volume.max_subdivision_level = snapshot.hierarchy_enabled ? RASTER_PROBE_MAX_SUBDIVISION_LEVEL : 0u;
        snapshot.max_subdivision_level = Max(snapshot.max_subdivision_level, volume.max_subdivision_level);
        uint  nearest_brick_index = RASTER_PROBE_PAGE_INVALID;
        float nearest_distance_sq = std::numeric_limits<float>::max();

        auto append_brick = [&](uint subdivision_level,
                                uint3 brick_coord,
                                uint cell_index,
                                bool level_requested,
                                bool distance_gated) -> uint {
            if (snapshot.brick_count >= snapshot.bricks.size()) {
                LOG_ERROR(
                    "[ProbeGI] Brick descriptor capacity exceeded: volume={}, config_index={}, level={}, coord=({}, {}, {}).",
                    snapshot.volume_count,
                    config_index,
                    subdivision_level,
                    brick_coord.x,
                    brick_coord.y,
                    brick_coord.z
                );
                return RASTER_PROBE_PAGE_INVALID;
            }

            const uint3 level_probe_counts =
                ProbeAdaptiveLayout::GetLevelProbeCounts(base_probe_counts, subdivision_level);
            const uint3 level_brick_counts =
                ProbeAdaptiveLayout::GetLevelBrickCounts(base_probe_counts, subdivision_level);
            const float3 level_spacing = ProbeAdaptiveLayout::GetLevelSpacing(volume.extent, level_probe_counts);
            const uint brick_index = snapshot.brick_count;
            BrickSnapshot& brick = snapshot.bricks[brick_index];
            brick.coord             = brick_coord;
            brick.volume_index      = snapshot.volume_count;
            brick.cell_index        = cell_index;
            brick.subdivision_level = subdivision_level;

            const ProbeBrickVirtualKey virtual_key{config_index, subdivision_level, brick.coord};
            brick.page_index        = ProbeAdaptiveLayout::GetVirtualPageIndex(virtual_key);
            brick.parent_page_index = ProbeAdaptiveLayout::GetParentPageIndex(virtual_key);
            brick.neighbor_pages_0 = uint4(
                ProbeAdaptiveLayout::GetNeighborPageIndex(virtual_key, -1, 0, 0, level_brick_counts),
                ProbeAdaptiveLayout::GetNeighborPageIndex(virtual_key, 1, 0, 0, level_brick_counts),
                ProbeAdaptiveLayout::GetNeighborPageIndex(virtual_key, 0, -1, 0, level_brick_counts),
                ProbeAdaptiveLayout::GetNeighborPageIndex(virtual_key, 0, 1, 0, level_brick_counts)
            );
            brick.neighbor_pages_1 = uint4(
                ProbeAdaptiveLayout::GetNeighborPageIndex(virtual_key, 0, 0, -1, level_brick_counts),
                ProbeAdaptiveLayout::GetNeighborPageIndex(virtual_key, 0, 0, 1, level_brick_counts),
                RASTER_PROBE_PAGE_INVALID,
                0u
            );

            const uint3 brick_start = brick.coord * RASTER_PROBE_BRICK_DIM;
            brick.local_counts = uint3(
                Min(RASTER_PROBE_BRICK_DIM, level_probe_counts.x - brick_start.x),
                Min(RASTER_PROBE_BRICK_DIM, level_probe_counts.y - brick_start.y),
                Min(RASTER_PROBE_BRICK_DIM, level_probe_counts.z - brick_start.z)
            );
            brick.probe_count = brick.local_counts.x * brick.local_counts.y * brick.local_counts.z;
            const float3 brick_center_coord(
                float(brick_start.x) + float(brick.local_counts.x - 1u) * 0.5f,
                float(brick_start.y) + float(brick.local_counts.y - 1u) * 0.5f,
                float(brick_start.z) + float(brick.local_counts.z - 1u) * 0.5f
            );
            const float3 brick_center = volume.origin + level_spacing * brick_center_coord;
            brick.camera_distance_sq = SquaredLengthf(camera_position - brick_center);

            bool same_brick = false;
            if (m_has_snapshot && brick_index < m_snapshot.brick_count) {
                const BrickSnapshot& previous_brick = m_snapshot.bricks[brick_index];
                same_brick = previous_brick.volume_index == brick.volume_index &&
                             previous_brick.coord == brick.coord &&
                             previous_brick.page_index == brick.page_index &&
                             previous_brick.local_counts == brick.local_counts &&
                             previous_brick.subdivision_level == brick.subdivision_level;
            }
            const bool history_valid = same_brick && m_brick_history_valid[brick_index];
            brick.update_age = history_valid ?
                                   static_cast<uint>(std::min<uint64>(
                                       frame_index - m_brick_last_update_frame[brick_index],
                                       255u
                                   )) :
                                   255u;

            float resident_distance = snapshot.brick_resident_distance;
            if (distance_gated && snapshot.sparse_bricks_enabled && same_brick &&
                m_snapshot.sparse_bricks_enabled && m_snapshot.bricks[brick_index].requested_resident) {
                resident_distance += snapshot.brick_resident_hysteresis;
            }
            const bool within_resident_distance =
                !distance_gated || !snapshot.sparse_bricks_enabled ||
                brick.camera_distance_sq <= resident_distance * resident_distance;
            brick.requested_resident = level_requested && within_resident_distance;

            ++snapshot.level_brick_count[subdivision_level];
            ++volume.level_brick_count[subdivision_level];
            snapshot.hierarchy_probe_count += brick.probe_count;
            volume.hierarchy_probe_count += brick.probe_count;
            if (brick.requested_resident) {
                ++snapshot.requested_brick_count;
                snapshot.requested_probe_count += brick.probe_count;
                ++snapshot.level_requested_brick_count[subdivision_level];
                ++volume.requested_brick_count;
                volume.requested_probe_count += brick.probe_count;
                ++volume.level_requested_brick_count[subdivision_level];
                if (cell_index != RASTER_PROBE_PAGE_INVALID) {
                    ++snapshot.cells[cell_index].requested_brick_count;
                }
            }

            ++snapshot.brick_count;
            ++volume.brick_count;
            if (cell_index != RASTER_PROBE_PAGE_INVALID) {
                ++snapshot.cells[cell_index].brick_count;
            }
            return brick_index;
        };

        for (uint cell_z = 0; cell_z < cell_counts.z; ++cell_z) {
            for (uint cell_y = 0; cell_y < cell_counts.y; ++cell_y) {
                for (uint cell_x = 0; cell_x < cell_counts.x; ++cell_x) {
                    if (snapshot.cell_count >= snapshot.cells.size()) {
                        LOG_ERROR(
                            "[ProbeGI] Cell descriptor capacity exceeded: volume={}, config_index={}, coord=({}, {}, {}).",
                            snapshot.volume_count,
                            config_index,
                            cell_x,
                            cell_y,
                            cell_z
                        );
                        continue;
                    }
                    const uint cell_index = snapshot.cell_count;
                    CellSnapshot& cell = snapshot.cells[cell_index];
                    cell.coord             = uint3(cell_x, cell_y, cell_z);
                    cell.volume_index      = snapshot.volume_count;
                    cell.config_index      = config_index;
                    cell.first_brick_index = snapshot.brick_count;
                    cell.min_probe_spacing = Min(volume.spacing.x, Min(volume.spacing.y, volume.spacing.z));

                    const uint3 cell_brick_begin = cell.coord * RASTER_PROBE_CELL_BRICK_DIM;
                    const uint3 cell_brick_end = Min(
                        cell_brick_begin + uint3(RASTER_PROBE_CELL_BRICK_DIM),
                        fine_brick_counts
                    );
                    const uint3 cell_probe_begin = cell_brick_begin * RASTER_PROBE_BRICK_DIM;
                    const uint3 cell_probe_end = Min(
                        cell_brick_end * RASTER_PROBE_BRICK_DIM,
                        uint3(volume.count_x, volume.count_y, volume.count_z)
                    );
                    const uint3 cell_probe_counts = cell_probe_end - cell_probe_begin;
                    cell.origin = volume.origin + volume.spacing * float3(cell_probe_begin);
                    cell.extent = volume.spacing * float3(
                        cell_probe_counts.x > 0u ? cell_probe_counts.x - 1u : 0u,
                        cell_probe_counts.y > 0u ? cell_probe_counts.y - 1u : 0u,
                        cell_probe_counts.z > 0u ? cell_probe_counts.z - 1u : 0u
                    );

                    ProbeGeometryCellStats geometry_stats{};
                    if (snapshot.adaptive_placement_enabled) {
                        const Box3D cell_bounds = ProbeGeometryClassifier::BuildCellInfluenceBounds(
                            cell.origin,
                            cell.extent,
                            volume.spacing,
                            volume_bounds
                        );
                        geometry_stats = ProbeGeometryClassifier::Analyze(
                            cell_bounds,
                            geometry_bounds,
                            snapshot.adaptive_geometry_padding,
                            snapshot.adaptive_fine_occupancy,
                            snapshot.adaptive_fine_primitives
                        );
                    } else {
                        geometry_stats.desired_subdivision_level = 0u;
                    }
                    cell.geometry_primitive_count = geometry_stats.intersecting_primitive_count;
                    cell.occupied_voxel_count = geometry_stats.occupied_voxel_count;
                    cell.desired_subdivision_level = Min(
                        geometry_stats.desired_subdivision_level,
                        RASTER_PROBE_MAX_SUBDIVISION_LEVEL
                    );
                    cell.geometry_generation = snapshot.geometry_generation;
                    cell.geometry_occupancy = geometry_stats.occupancy;
                    cell.min_subdivision_level = snapshot.hierarchy_enabled ?
                                                     cell.desired_subdivision_level :
                                                     0u;
                    cell.max_subdivision_level = volume.max_subdivision_level;
                    if (cell.desired_subdivision_level == 0u) {
                        ++snapshot.fine_cell_count;
                    } else if (cell.desired_subdivision_level == 1u) {
                        ++snapshot.medium_cell_count;
                    } else {
                        ++snapshot.coarse_cell_count;
                    }

                    const bool request_fine_level = ProbeAdaptiveLayout::ShouldRequestLevel(
                        cell.desired_subdivision_level,
                        0u,
                        snapshot.hierarchy_enabled
                    );
                    for (uint brick_z = cell_brick_begin.z; brick_z < cell_brick_end.z; ++brick_z) {
                        for (uint brick_y = cell_brick_begin.y; brick_y < cell_brick_end.y; ++brick_y) {
                            for (uint brick_x = cell_brick_begin.x; brick_x < cell_brick_end.x; ++brick_x) {
                                const uint brick_index = append_brick(
                                    0u,
                                    uint3(brick_x, brick_y, brick_z),
                                    cell_index,
                                    request_fine_level,
                                    true
                                );
                                if (brick_index != RASTER_PROBE_PAGE_INVALID &&
                                    snapshot.bricks[brick_index].camera_distance_sq < nearest_distance_sq) {
                                    nearest_distance_sq = snapshot.bricks[brick_index].camera_distance_sq;
                                    nearest_brick_index = brick_index;
                                }
                            }
                        }
                    }

                    if (snapshot.hierarchy_enabled) {
                        append_brick(
                            1u,
                            cell.coord,
                            cell_index,
                            ProbeAdaptiveLayout::ShouldRequestLevel(
                                cell.desired_subdivision_level,
                                1u,
                                snapshot.hierarchy_enabled
                            ),
                            false
                        );
                    }

                    ++snapshot.cell_count;
                    ++volume.cell_count;
                }
            }
        }

        if (snapshot.hierarchy_enabled) {
            append_brick(
                RASTER_PROBE_MAX_SUBDIVISION_LEVEL,
                uint3(0u),
                RASTER_PROBE_PAGE_INVALID,
                ProbeAdaptiveLayout::ShouldRequestLevel(
                    RASTER_PROBE_MAX_SUBDIVISION_LEVEL,
                    RASTER_PROBE_MAX_SUBDIVISION_LEVEL,
                    snapshot.hierarchy_enabled
                ),
                false
            );
        }

        if (!snapshot.hierarchy_enabled && snapshot.sparse_bricks_enabled &&
            volume.requested_brick_count == 0u &&
            nearest_brick_index != RASTER_PROBE_PAGE_INVALID) {
            BrickSnapshot& nearest_brick = snapshot.bricks[nearest_brick_index];
            nearest_brick.requested_resident = true;
            ++snapshot.requested_brick_count;
            snapshot.requested_probe_count += nearest_brick.probe_count;
            ++snapshot.level_requested_brick_count[nearest_brick.subdivision_level];
            ++volume.requested_brick_count;
            volume.requested_probe_count += nearest_brick.probe_count;
            ++volume.level_requested_brick_count[nearest_brick.subdivision_level];
            ++snapshot.cells[nearest_brick.cell_index].requested_brick_count;
        }

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

bool ProbeVolumeResource::RequiresPhysicalAllocatorReset(const Snapshot& snapshot) const {
    if (!m_has_snapshot || m_physical_allocator.GetCapacity() != snapshot.physical_probe_capacity ||
        m_snapshot.brick_count != snapshot.brick_count) {
        return true;
    }

    for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
        const BrickSnapshot& previous = m_snapshot.bricks[brick_index];
        const BrickSnapshot& current  = snapshot.bricks[brick_index];
        if (previous.coord != current.coord || previous.local_counts != current.local_counts ||
            previous.volume_index != current.volume_index || previous.probe_count != current.probe_count ||
            previous.page_index != current.page_index || previous.cell_index != current.cell_index ||
            previous.subdivision_level != current.subdivision_level ||
            previous.parent_page_index != current.parent_page_index) {
            return true;
        }
    }

    return false;
}

void ProbeVolumeResource::ResetPhysicalAllocator(uint physical_probe_capacity) {
    m_physical_allocations.fill({});
    m_physical_allocator.Reset(physical_probe_capacity);
    m_brick_history_valid.fill(false);
    m_brick_last_update_frame.fill(0);
    LOG_DEBUG("[ProbeGI] Physical Probe allocator reset: capacity={}.", physical_probe_capacity);
}

void ProbeVolumeResource::ReleasePhysicalAllocation(uint brick_index) {
    if (brick_index >= m_physical_allocations.size()) {
        return;
    }

    ProbePhysicalAllocator::Allocation& allocation = m_physical_allocations[brick_index];
    if (allocation.valid && allocation.probe_count > 0u) {
        LOG_DEBUG(
            "[ProbeGI] Physical Probe range released: brick={}, offset={}, count={}.",
            brick_index,
            allocation.probe_offset,
            allocation.probe_count
        );
        if (!m_physical_allocator.Release(allocation)) {
            LOG_ERROR(
                "[ProbeGI] Physical Probe allocator rejected release: brick={}, offset={}, count={}.",
                brick_index,
                allocation.probe_offset,
                allocation.probe_count
            );
        }
    }
    allocation = {};
    m_brick_history_valid[brick_index] = false;
    m_brick_last_update_frame[brick_index] = 0;
}

void ProbeVolumeResource::ApplyPhysicalResidency(Snapshot& snapshot) {
    snapshot.page_table.fill(RASTER_PROBE_PAGE_INVALID);
    snapshot.resident_brick_count = 0;
    snapshot.resident_probe_count = 0;
    snapshot.level_resident_brick_count.fill(0u);
    snapshot.allocated_physical_probe_count = 0;
    snapshot.physical_allocation_count = 0;
    snapshot.capacity_evicted_brick_count = 0;
    for (uint volume_index = 0; volume_index < snapshot.volume_count; ++volume_index) {
        snapshot.volumes[volume_index].resident_brick_count = 0;
        snapshot.volumes[volume_index].resident_probe_count = 0;
        snapshot.volumes[volume_index].level_resident_brick_count.fill(0u);
    }
    for (uint cell_index = 0; cell_index < snapshot.cell_count; ++cell_index) {
        snapshot.cells[cell_index].resident_brick_count = 0;
    }

    Array<uint> requested_candidates;
    requested_candidates.reserve(snapshot.requested_brick_count);
    StaticArray<uint, RASTER_PROBE_VOLUME_MAX_COUNT> fallback_anchor_brick;
    fallback_anchor_brick.fill(RASTER_PROBE_PAGE_INVALID);
    if (snapshot.enabled) {
        for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
            const BrickSnapshot& brick = snapshot.bricks[brick_index];
            if (!brick.requested_resident) {
                continue;
            }
            requested_candidates.push_back(brick_index);
            uint& anchor_index = fallback_anchor_brick[brick.volume_index];
            if (anchor_index == RASTER_PROBE_PAGE_INVALID ||
                brick.subdivision_level > snapshot.bricks[anchor_index].subdivision_level ||
                (brick.subdivision_level == snapshot.bricks[anchor_index].subdivision_level &&
                 brick.camera_distance_sq < snapshot.bricks[anchor_index].camera_distance_sq)) {
                anchor_index = brick_index;
            }
        }
    }

    std::sort(
        requested_candidates.begin(),
        requested_candidates.end(),
        [&](uint lhs_index, uint rhs_index) {
            const BrickSnapshot& lhs = snapshot.bricks[lhs_index];
            const BrickSnapshot& rhs = snapshot.bricks[rhs_index];
            const bool lhs_volume_anchor = fallback_anchor_brick[lhs.volume_index] == lhs_index;
            const bool rhs_volume_anchor = fallback_anchor_brick[rhs.volume_index] == rhs_index;
            if (lhs_volume_anchor != rhs_volume_anchor) {
                return lhs_volume_anchor;
            }
            if (lhs_volume_anchor && lhs.volume_index != rhs.volume_index) {
                return lhs.volume_index < rhs.volume_index;
            }
            if (lhs.subdivision_level != rhs.subdivision_level) {
                return lhs.subdivision_level > rhs.subdivision_level;
            }
            if (!NearlyEqual(lhs.camera_distance_sq, rhs.camera_distance_sq)) {
                return lhs.camera_distance_sq < rhs.camera_distance_sq;
            }
            return lhs_index < rhs_index;
        }
    );

    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT> selected_bricks{};
    uint selected_probe_count = 0;
    for (uint brick_index : requested_candidates) {
        const uint probe_count = snapshot.bricks[brick_index].probe_count;
        if (selected_probe_count + probe_count > snapshot.physical_probe_capacity) {
            continue;
        }
        selected_bricks[brick_index] = true;
        selected_probe_count += probe_count;
    }
    snapshot.capacity_evicted_brick_count =
        static_cast<uint>(requested_candidates.size()) -
        static_cast<uint>(std::count(selected_bricks.begin(), selected_bricks.end(), true));

    const bool allocator_reset = RequiresPhysicalAllocatorReset(snapshot);
    if (allocator_reset) {
        ++m_layout_generation;
        if (m_layout_generation == 0u) {
            ++m_layout_generation;
        }
        ResetPhysicalAllocator(snapshot.physical_probe_capacity);
    }
    snapshot.layout_generation = m_layout_generation;

    for (uint brick_index = 0; brick_index < m_physical_allocations.size(); ++brick_index) {
        if (brick_index >= snapshot.brick_count || !selected_bricks[brick_index]) {
            ReleasePhysicalAllocation(brick_index);
        }
    }

    bool allocation_failed = false;
    for (uint brick_index : requested_candidates) {
        if (!selected_bricks[brick_index]) {
            continue;
        }

        ProbePhysicalAllocator::Allocation& allocation = m_physical_allocations[brick_index];
        const uint probe_count = snapshot.bricks[brick_index].probe_count;
        if (allocation.valid && allocation.probe_count == probe_count) {
            continue;
        }
        if (allocation.valid) {
            ReleasePhysicalAllocation(brick_index);
        }

        ProbePhysicalAllocator::Allocation new_allocation;
        if (!m_physical_allocator.Allocate(probe_count, new_allocation)) {
            allocation_failed = true;
            break;
        }
        allocation = new_allocation;
        m_brick_history_valid[brick_index] = false;
        m_brick_last_update_frame[brick_index] = 0;
        LOG_DEBUG(
            "[ProbeGI] Physical Probe range assigned: brick={}, volume={}, offset={}, count={}.",
            brick_index,
            snapshot.bricks[brick_index].volume_index,
            allocation.probe_offset,
            probe_count
        );
    }

    if (allocation_failed) {
        snapshot.allocator_compacted = true;
        ResetPhysicalAllocator(snapshot.physical_probe_capacity);
        for (uint brick_index : requested_candidates) {
            if (!selected_bricks[brick_index]) {
                continue;
            }
            const uint probe_count = snapshot.bricks[brick_index].probe_count;
            ProbePhysicalAllocator::Allocation allocation;
            if (!m_physical_allocator.Allocate(probe_count, allocation)) {
                LOG_ERROR(
                    "[ProbeGI] Physical Probe allocator failed after compaction: brick={}, probe_count={}, capacity={}.",
                    brick_index,
                    probe_count,
                    snapshot.physical_probe_capacity
                );
                selected_bricks[brick_index] = false;
                continue;
            }
            m_physical_allocations[brick_index] = allocation;
            LOG_DEBUG(
                "[ProbeGI] Physical Probe range reassigned after compaction: brick={}, volume={}, offset={}, count={}.",
                brick_index,
                snapshot.bricks[brick_index].volume_index,
                allocation.probe_offset,
                probe_count
            );
        }
    }

    for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
        BrickSnapshot& brick = snapshot.bricks[brick_index];
        const ProbePhysicalAllocator::Allocation& allocation = m_physical_allocations[brick_index];
        brick.resident = selected_bricks[brick_index] && allocation.valid;
        if (!brick.resident) {
            brick.probe_offset = RASTER_PROBE_PAGE_INVALID;
            brick.update_age = 255u;
            continue;
        }

        brick.probe_offset = allocation.probe_offset;
        if (!m_brick_history_valid[brick_index]) {
            brick.update_age = 255u;
        }
        snapshot.page_table[brick.page_index] = brick_index;
        ++snapshot.resident_brick_count;
        snapshot.resident_probe_count += brick.probe_count;
        ++snapshot.level_resident_brick_count[brick.subdivision_level];
        ++snapshot.physical_allocation_count;
        snapshot.allocated_physical_probe_count += brick.probe_count;

        VolumeSnapshot& volume = snapshot.volumes[brick.volume_index];
        ++volume.resident_brick_count;
        volume.resident_probe_count += brick.probe_count;
        ++volume.level_resident_brick_count[brick.subdivision_level];
        if (brick.cell_index != RASTER_PROBE_PAGE_INVALID) {
            ++snapshot.cells[brick.cell_index].resident_brick_count;
        }
    }

    snapshot.free_physical_probe_count =
        snapshot.physical_probe_capacity - snapshot.allocated_physical_probe_count;
    const uint allocator_free_probe_count = m_physical_allocator.GetFreeProbeCount();
    if (allocator_free_probe_count != snapshot.free_physical_probe_count) {
        LOG_ERROR(
            "[ProbeGI] Physical Probe allocator accounting mismatch: allocated={}, free_ranges={}, expected_free={}, capacity={}.",
            snapshot.allocated_physical_probe_count,
            allocator_free_probe_count,
            snapshot.free_physical_probe_count,
            snapshot.physical_probe_capacity
        );
    }
    if (!m_physical_allocator.Validate()) {
        LOG_ERROR("[ProbeGI] Physical Probe allocator invariant validation failed.");
    }
}

bool ProbeVolumeResource::HasSnapshotChanged(const Snapshot& snapshot) const {
    if (!m_has_snapshot) {
        return true;
    }

    if (m_snapshot.enabled != snapshot.enabled ||
        m_snapshot.sparse_bricks_enabled != snapshot.sparse_bricks_enabled ||
        m_snapshot.adaptive_placement_enabled != snapshot.adaptive_placement_enabled ||
        m_snapshot.hierarchy_enabled != snapshot.hierarchy_enabled ||
        m_snapshot.debug_mode != snapshot.debug_mode ||
        m_snapshot.volume_count != snapshot.volume_count || m_snapshot.cell_count != snapshot.cell_count ||
        m_snapshot.brick_count != snapshot.brick_count ||
        m_snapshot.max_subdivision_level != snapshot.max_subdivision_level ||
        m_snapshot.layout_generation != snapshot.layout_generation ||
        m_snapshot.requested_brick_count != snapshot.requested_brick_count ||
        m_snapshot.requested_probe_count != snapshot.requested_probe_count ||
        m_snapshot.resident_brick_count != snapshot.resident_brick_count ||
        m_snapshot.resident_probe_count != snapshot.resident_probe_count ||
        m_snapshot.total_count != snapshot.total_count ||
        m_snapshot.hierarchy_probe_count != snapshot.hierarchy_probe_count ||
        m_snapshot.physical_probe_capacity != snapshot.physical_probe_capacity ||
        m_snapshot.allocated_physical_probe_count != snapshot.allocated_physical_probe_count ||
        m_snapshot.physical_allocation_count != snapshot.physical_allocation_count ||
        m_snapshot.free_physical_probe_count != snapshot.free_physical_probe_count ||
        m_snapshot.capacity_evicted_brick_count != snapshot.capacity_evicted_brick_count ||
        m_snapshot.geometry_generation != snapshot.geometry_generation ||
        m_snapshot.geometry_primitive_count != snapshot.geometry_primitive_count ||
        m_snapshot.fine_cell_count != snapshot.fine_cell_count ||
        m_snapshot.medium_cell_count != snapshot.medium_cell_count ||
        m_snapshot.coarse_cell_count != snapshot.coarse_cell_count ||
        m_snapshot.level_brick_count != snapshot.level_brick_count ||
        m_snapshot.level_requested_brick_count != snapshot.level_requested_brick_count ||
        m_snapshot.level_resident_brick_count != snapshot.level_resident_brick_count ||
        !NearlyEqual(m_snapshot.adaptive_geometry_padding, snapshot.adaptive_geometry_padding) ||
        !NearlyEqual(m_snapshot.adaptive_fine_occupancy, snapshot.adaptive_fine_occupancy) ||
        m_snapshot.adaptive_fine_primitives != snapshot.adaptive_fine_primitives ||
        !NearlyEqual(m_snapshot.adaptive_transition_width, snapshot.adaptive_transition_width) ||
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
            lhs.hierarchy_probe_count != rhs.hierarchy_probe_count ||
            lhs.probe_offset != rhs.probe_offset || lhs.page_table_offset != rhs.page_table_offset ||
            lhs.brick_count != rhs.brick_count || lhs.first_cell_index != rhs.first_cell_index ||
            lhs.cell_count != rhs.cell_count || lhs.max_subdivision_level != rhs.max_subdivision_level ||
            lhs.requested_brick_count != rhs.requested_brick_count ||
            lhs.requested_probe_count != rhs.requested_probe_count ||
            lhs.resident_brick_count != rhs.resident_brick_count ||
            lhs.resident_probe_count != rhs.resident_probe_count ||
            lhs.level_brick_count != rhs.level_brick_count ||
            lhs.level_requested_brick_count != rhs.level_requested_brick_count ||
            lhs.level_resident_brick_count != rhs.level_resident_brick_count ||
            !NearlyEqual(lhs.origin, rhs.origin) ||
            !NearlyEqual(lhs.extent, rhs.extent) || !NearlyEqual(lhs.spacing, rhs.spacing) ||
            !NearlyEqual(lhs.intensity, rhs.intensity) || !NearlyEqual(lhs.normal_bias, rhs.normal_bias) ||
            !NearlyEqual(lhs.blend_distance, rhs.blend_distance)) {
            return true;
        }
    }

    for (uint cell_index = 0; cell_index < snapshot.cell_count; ++cell_index) {
        const CellSnapshot& lhs = m_snapshot.cells[cell_index];
        const CellSnapshot& rhs = snapshot.cells[cell_index];
        if (lhs.coord != rhs.coord || lhs.volume_index != rhs.volume_index ||
            lhs.config_index != rhs.config_index || lhs.first_brick_index != rhs.first_brick_index ||
            lhs.brick_count != rhs.brick_count ||
            lhs.requested_brick_count != rhs.requested_brick_count ||
            lhs.resident_brick_count != rhs.resident_brick_count ||
            lhs.min_subdivision_level != rhs.min_subdivision_level ||
            lhs.max_subdivision_level != rhs.max_subdivision_level ||
            lhs.geometry_primitive_count != rhs.geometry_primitive_count ||
            lhs.occupied_voxel_count != rhs.occupied_voxel_count ||
            lhs.desired_subdivision_level != rhs.desired_subdivision_level ||
            lhs.geometry_generation != rhs.geometry_generation || !NearlyEqual(lhs.origin, rhs.origin) ||
            !NearlyEqual(lhs.extent, rhs.extent) || !NearlyEqual(lhs.min_probe_spacing, rhs.min_probe_spacing) ||
            !NearlyEqual(lhs.geometry_occupancy, rhs.geometry_occupancy)) {
            return true;
        }
    }

    for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
        const BrickSnapshot& lhs = m_snapshot.bricks[brick_index];
        const BrickSnapshot& rhs = snapshot.bricks[brick_index];
        if (lhs.coord != rhs.coord || lhs.local_counts != rhs.local_counts ||
            lhs.volume_index != rhs.volume_index || lhs.probe_offset != rhs.probe_offset ||
            lhs.probe_count != rhs.probe_count || lhs.page_index != rhs.page_index ||
            lhs.cell_index != rhs.cell_index || lhs.subdivision_level != rhs.subdivision_level ||
            lhs.parent_page_index != rhs.parent_page_index ||
            lhs.neighbor_pages_0 != rhs.neighbor_pages_0 || lhs.neighbor_pages_1 != rhs.neighbor_pages_1 ||
            lhs.requested_resident != rhs.requested_resident || lhs.resident != rhs.resident) {
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
    return m_snapshot.enabled != snapshot.enabled ||
           m_snapshot.hierarchy_enabled != snapshot.hierarchy_enabled ||
           previous.config_index != current.config_index ||
           previous.count_x != current.count_x || previous.count_y != current.count_y ||
           previous.count_z != current.count_z || previous.total_count != current.total_count ||
           previous.hierarchy_probe_count != current.hierarchy_probe_count ||
           previous.probe_offset != current.probe_offset ||
           previous.page_table_offset != current.page_table_offset || previous.brick_count != current.brick_count ||
           previous.first_cell_index != current.first_cell_index ||
           previous.cell_count != current.cell_count ||
           previous.max_subdivision_level != current.max_subdivision_level ||
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
        desc.hierarchy = uint4(
            volume.first_cell_index,
            volume.cell_count,
            volume.max_subdivision_level,
            snapshot.layout_generation
        );
    }

    m_volume_data_upload.resize(sizeof(m_gpu_volume_descs));
    std::memcpy(m_volume_data_upload.data(), m_gpu_volume_descs.data(), sizeof(m_gpu_volume_descs));
    StageCellUpload(snapshot);
    StageBrickUpload(snapshot);
    m_page_table_upload.resize(sizeof(snapshot.page_table));
    std::memcpy(m_page_table_upload.data(), snapshot.page_table.data(), sizeof(snapshot.page_table));
}

void ProbeVolumeResource::StageCellUpload(const Snapshot& snapshot) {
    m_gpu_cell_descs.fill({});
    for (uint cell_index = 0; cell_index < snapshot.cell_count; ++cell_index) {
        const CellSnapshot& cell = snapshot.cells[cell_index];
        ProbeCellGpuDesc&   desc = m_gpu_cell_descs[cell_index];
        desc.origin_spacing = float4(cell.origin.x, cell.origin.y, cell.origin.z, cell.min_probe_spacing);
        desc.extent = float4(cell.extent.x, cell.extent.y, cell.extent.z, cell.geometry_occupancy);
        desc.coord_volume = uint4(cell.coord.x, cell.coord.y, cell.coord.z, cell.volume_index);
        desc.brick_range = uint4(
            cell.first_brick_index,
            cell.brick_count,
            cell.requested_brick_count,
            cell.resident_brick_count
        );
        desc.hierarchy = uint4(
            cell.min_subdivision_level,
            cell.max_subdivision_level,
            cell.config_index,
            snapshot.layout_generation
        );
        desc.geometry = uint4(
            cell.geometry_primitive_count,
            cell.occupied_voxel_count,
            cell.desired_subdivision_level,
            cell.geometry_generation
        );
    }

    m_cell_data_upload.resize(sizeof(m_gpu_cell_descs));
    std::memcpy(m_cell_data_upload.data(), m_gpu_cell_descs.data(), sizeof(m_gpu_cell_descs));
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
        desc.hierarchy = uint4(
            brick.cell_index,
            brick.subdivision_level,
            brick.parent_page_index,
            brick.page_index
        );
        desc.neighbor_pages_0 = brick.neighbor_pages_0;
        desc.neighbor_pages_1 = uint4(
            brick.neighbor_pages_1.x,
            brick.neighbor_pages_1.y,
            snapshot.layout_generation,
            brick.neighbor_pages_1.w
        );
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
    const BrickSnapshot& brick = snapshot.bricks[brick_index];
    const uint3 base_probe_counts(volume.count_x, volume.count_y, volume.count_z);
    const uint3 level_probe_counts =
        ProbeAdaptiveLayout::GetLevelProbeCounts(base_probe_counts, brick.subdivision_level);
    const float3 level_spacing = ProbeAdaptiveLayout::GetLevelSpacing(volume.extent, level_probe_counts);

    ProbeUpdateParam param{};
    param.probe_volume_counts = uint4(
        level_probe_counts.x,
        level_probe_counts.y,
        level_probe_counts.z,
        volume_index
    );
    param.probe_volume_origin  = float4(
        volume.origin.x,
        volume.origin.y,
        volume.origin.z,
        history_valid ? snapshot.irradiance_hysteresis : 0.0f
    );
    param.probe_volume_spacing = float4(
        level_spacing.x,
        level_spacing.y,
        level_spacing.z,
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

    if (m_probe_buffer.buf == nullptr || m_volume_buffer.buf == nullptr || m_cell_buffer.buf == nullptr ||
        m_brick_buffer.buf == nullptr || m_page_table_buffer.buf == nullptr ||
        m_visibility_atlas_buffer.buf == nullptr || m_irradiance_atlas_buffer.buf == nullptr) {
        return update_info;
    }

    if (config.probe_gi_enabled && config.probe_gi_adaptive_placement_enabled) {
        RefreshSceneGeometry(scene);
    } else {
        m_scene_geometry_cache_valid = false;
    }

    Snapshot snapshot = BuildSnapshot(config, camera_position, frame_index);
    ApplyPhysicalResidency(snapshot);
    const bool changed  = HasSnapshotChanged(snapshot);
    bool adaptive_layout_changed =
        !m_has_snapshot || m_snapshot.adaptive_placement_enabled != snapshot.adaptive_placement_enabled ||
        m_snapshot.hierarchy_enabled != snapshot.hierarchy_enabled ||
        m_snapshot.geometry_generation != snapshot.geometry_generation ||
        m_snapshot.geometry_primitive_count != snapshot.geometry_primitive_count ||
        m_snapshot.cell_count != snapshot.cell_count ||
        !NearlyEqual(m_snapshot.adaptive_geometry_padding, snapshot.adaptive_geometry_padding) ||
        !NearlyEqual(m_snapshot.adaptive_fine_occupancy, snapshot.adaptive_fine_occupancy) ||
        m_snapshot.adaptive_fine_primitives != snapshot.adaptive_fine_primitives ||
        !NearlyEqual(m_snapshot.adaptive_transition_width, snapshot.adaptive_transition_width);
    if (!adaptive_layout_changed) {
        for (uint cell_index = 0u; cell_index < snapshot.cell_count; ++cell_index) {
            const CellSnapshot& previous = m_snapshot.cells[cell_index];
            const CellSnapshot& current  = snapshot.cells[cell_index];
            if (previous.coord != current.coord || previous.volume_index != current.volume_index ||
                previous.geometry_primitive_count != current.geometry_primitive_count ||
                previous.occupied_voxel_count != current.occupied_voxel_count ||
                previous.desired_subdivision_level != current.desired_subdivision_level ||
                !NearlyEqual(previous.geometry_occupancy, current.geometry_occupancy)) {
                adaptive_layout_changed = true;
                break;
            }
        }
    }

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
                "[ProbeGI] Volume residency updated: slot={}, config_index={}, count=({}, {}, {}) base_probes={}, hierarchy_probes={}, logical_offset={}, cells={} range=[{}, {}), max_level={}, level_bricks=({}, {}, {}), requested_bricks={}/{}, requested_levels=({}, {}, {}), requested_probes={}, resident_bricks={}/{}, resident_levels=({}, {}, {}), resident_probes={}, page_table_offset={}, origin={}, extent={}, spacing={}, intensity={}, blend_distance={}, history_reset={}",
                volume_index,
                volume.config_index,
                volume.count_x,
                volume.count_y,
                volume.count_z,
                volume.total_count,
                volume.hierarchy_probe_count,
                volume.probe_offset,
                volume.cell_count,
                volume.first_cell_index,
                volume.first_cell_index + volume.cell_count,
                volume.max_subdivision_level,
                volume.level_brick_count[0],
                volume.level_brick_count[1],
                volume.level_brick_count[2],
                volume.requested_brick_count,
                volume.brick_count,
                volume.level_requested_brick_count[0],
                volume.level_requested_brick_count[1],
                volume.level_requested_brick_count[2],
                volume.requested_probe_count,
                volume.resident_brick_count,
                volume.brick_count,
                volume.level_resident_brick_count[0],
                volume.level_resident_brick_count[1],
                volume.level_resident_brick_count[2],
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

    if (adaptive_layout_changed && snapshot.adaptive_placement_enabled) {
        LOG_DEBUG(
            "[ProbeGI] Adaptive Cell analysis updated: hierarchy={}, geometry_generation={}, world_primitive_bounds={}, cells={}, fine={}, medium={}, coarse={}, padding={}, fine_occupancy_threshold={}, fine_primitive_threshold={}, transition_width={}, requested_levels=({}, {}, {}).",
            snapshot.hierarchy_enabled ? 1 : 0,
            snapshot.geometry_generation,
            snapshot.geometry_primitive_count,
            snapshot.cell_count,
            snapshot.fine_cell_count,
            snapshot.medium_cell_count,
            snapshot.coarse_cell_count,
            snapshot.adaptive_geometry_padding,
            snapshot.adaptive_fine_occupancy,
            snapshot.adaptive_fine_primitives,
            snapshot.adaptive_transition_width,
            snapshot.level_requested_brick_count[0],
            snapshot.level_requested_brick_count[1],
            snapshot.level_requested_brick_count[2]
        );
        for (uint cell_index = 0u; cell_index < snapshot.cell_count; ++cell_index) {
            const CellSnapshot& cell = snapshot.cells[cell_index];
            LOG_DEBUG(
                "[ProbeGI] Adaptive Cell: index={}, volume={}, coord=({}, {}, {}), primitives={}, occupied_voxels={}/{}, occupancy={}, desired_level={}.",
                cell_index,
                cell.volume_index,
                cell.coord.x,
                cell.coord.y,
                cell.coord.z,
                cell.geometry_primitive_count,
                cell.occupied_voxel_count,
                RASTER_PROBE_OCCUPANCY_VOXEL_COUNT,
                cell.geometry_occupancy,
                cell.desired_subdivision_level
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
            if (lhs.subdivision_level != rhs.subdivision_level) {
                return lhs.subdivision_level > rhs.subdivision_level;
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
            "[ProbeGI] Multi-volume brick residency updated: active_volumes={}, cells={}, hierarchy={}, max_level={}, layout_generation={}, base_probes={}, hierarchy_probes={}, level_bricks=({}, {}, {}), requested_bricks={}/{}, requested_levels=({}, {}, {}), requested_probes={}/{}, resident_bricks={}/{}, resident_levels=({}, {}, {}), resident_probes={}/{}, physical_allocations={}, physical_probes={}/{}, free_physical_probes={}, capacity_evicted_bricks={}, allocator_compacted={}, sparse={}, resident_distance={}, resident_hysteresis={}, scheduler={}, update_budget={}, scheduled_bricks={}, scheduled_probes={}, deferred_bricks={}, trace_distance={}, trace_rays={}, history_hysteresis=({}, {}).",
            snapshot.volume_count,
            snapshot.cell_count,
            snapshot.hierarchy_enabled ? 1 : 0,
            snapshot.max_subdivision_level,
            snapshot.layout_generation,
            snapshot.total_count,
            snapshot.hierarchy_probe_count,
            snapshot.level_brick_count[0],
            snapshot.level_brick_count[1],
            snapshot.level_brick_count[2],
            snapshot.requested_brick_count,
            snapshot.brick_count,
            snapshot.level_requested_brick_count[0],
            snapshot.level_requested_brick_count[1],
            snapshot.level_requested_brick_count[2],
            snapshot.requested_probe_count,
            snapshot.hierarchy_probe_count,
            snapshot.resident_brick_count,
            snapshot.brick_count,
            snapshot.level_resident_brick_count[0],
            snapshot.level_resident_brick_count[1],
            snapshot.level_resident_brick_count[2],
            snapshot.resident_probe_count,
            snapshot.hierarchy_probe_count,
            snapshot.physical_allocation_count,
            snapshot.allocated_physical_probe_count,
            snapshot.physical_probe_capacity,
            snapshot.free_physical_probe_count,
            snapshot.capacity_evicted_brick_count,
            snapshot.allocator_compacted ? 1 : 0,
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
         m_cell_buffer.hdl != 0 && m_brick_buffer.hdl != 0 && m_page_table_buffer.hdl != 0 &&
         m_visibility_atlas_buffer.hdl != 0 &&
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
    lighting_data.probe_system_hierarchy = uint4(
        m_cell_buffer.hdl,
        m_snapshot.cell_count,
        m_snapshot.max_subdivision_level,
        m_snapshot.layout_generation
    );
    lighting_data.probe_system_debug =
        float4(m_snapshot.debug_scale, m_snapshot.adaptive_transition_width, 0.0f, 0.0f);
}

} // namespace Moer::Render::Raster
