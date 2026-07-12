#include "ProbeVolumeResource.h"

#include "log/LogSystem.h"
#include "math/Function.h"
#include "math/Transform.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/LogicalComponents.h"
#include "scene/Scene.h"

#include <algorithm>
#include <bit>
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

uint64 HashTransform(const float4x4& transform) {
    uint64 hash = 14695981039346656037ull;
    for (float component : transform.e) {
        hash ^= static_cast<uint64>(std::bit_cast<uint32>(component));
        hash *= 1099511628211ull;
    }
    return hash;
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

    m_physical_allocation_pages.fill(RASTER_PROBE_PAGE_INVALID);
    m_gpu_completion_state = MakeShared<GpuCompletionState>();

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
    m_physical_allocation_pages.fill(RASTER_PROBE_PAGE_INVALID);
    m_physical_allocation_generations.fill(0u);
    m_brick_load_submission.fill(0u);
    m_brick_load_awaiting_submission.fill(false);
    m_page_generations.fill(0u);
    m_physical_allocator.Reset(0u);
    m_retirement_queue.Clear();
    m_gpu_completion_state = nullptr;
    m_last_submitted_submission = 0u;
    m_pending_physical_capacity = 0u;
    m_scene_geometry_bounds.clear();
    m_scene_geometry_instances.clear();
    m_frame_dirty_regions.clear();
    m_brick_pending_dirty_reasons.fill(0u);
    m_main_light_snapshot = {};
    m_main_light_snapshot_valid = false;
    m_frame_global_dirty_reasons = 0u;
    m_frame_changed_geometry_count = 0u;
    m_frame_dirty_regions_collapsed = false;
    m_scene_geometry_cache_valid = false;
    m_scene_geometry_generation = 0u;
    m_layout_generation = 0u;
    m_last_scheduled_brick_count = 0;
    m_last_scheduled_probe_count = 0;
    m_last_dirty_brick_count = 0;
    m_last_scheduled_dirty_brick_count = 0;
    m_last_deferred_dirty_brick_count = 0;
    m_last_dirty_region_count = 0;
    m_last_global_dirty_reasons = 0u;
}

void ProbeVolumeResource::RefreshSceneGeometry(const Scene& scene, bool dirty_tracking_enabled) {
    if (!scene.IsReady()) {
        if (m_scene_geometry_cache_valid || !m_scene_geometry_bounds.empty() ||
            !m_scene_geometry_instances.empty()) {
            m_scene_geometry_bounds.clear();
            m_scene_geometry_instances.clear();
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
    Array<ProbeTrackedBounds> rebuilt_instances;
    rebuilt_instances.reserve(m_scene_geometry_instances.size());
    uint renderable_instance_count = 0u;
    uint leaf_primitive_count      = 0u;
    uint skipped_invalid_count     = 0u;

    const auto& registry = scene.r();
    registry.view<const ecs::CRenderable, const ecs::CNode>().each(
        [&](const auto renderable_entity, const ecs::CRenderable& renderable, const ecs::CNode& node) {
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
            Box3D instance_bounds;
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
                instance_bounds.Expand(world_bounds);
            }

            if (instance_bounds.IsValid()) {
                rebuilt_instances.push_back(
                    {
                        static_cast<uint64>(entt::to_integral(renderable_entity)),
                        instance_bounds,
                        HashTransform(node.d_world_transform)
                    }
                );
            }
        }
    );

    ProbeDirtyTracker::SortByKey(rebuilt_instances);
    const bool transform_changed = tick_state.updated_transform || tick_state.created_transform;
    const bool geometry_rebuilt  = tick_state.rebuilt_mesh || tick_state.rebuilt_rt_blas;
    ProbeDirtyDiff geometry_diff;
    if (m_scene_geometry_cache_valid) {
        uint dirty_reasons = 0u;
        if (transform_changed) {
            dirty_reasons |= RASTER_PROBE_DIRTY_DYNAMIC;
        }
        if (geometry_rebuilt) {
            dirty_reasons |= RASTER_PROBE_DIRTY_GEOMETRY;
        }

        geometry_diff = ProbeDirtyTracker::Diff(
            m_scene_geometry_instances,
            rebuilt_instances,
            dirty_reasons,
            64u,
            geometry_rebuilt
        );
        if (!geometry_rebuilt && geometry_diff.changed_bounds == 0u) {
            return;
        }
        if (dirty_tracking_enabled) {
            m_frame_changed_geometry_count += geometry_diff.changed_bounds;
            m_frame_dirty_regions_collapsed |= geometry_diff.collapsed;
            m_frame_dirty_regions = std::move(geometry_diff.regions);
        }
    }

    m_scene_geometry_bounds = std::move(rebuilt_bounds);
    m_scene_geometry_instances = std::move(rebuilt_instances);
    m_scene_geometry_cache_valid = true;
    ++m_scene_geometry_generation;
    if (m_scene_geometry_generation == 0u) {
        ++m_scene_geometry_generation;
    }
    LOG_DEBUG(
        "[ProbeGI] Geometry cache rebuilt: generation={}, renderable_instances={}, leaf_primitives={}, valid_world_bounds={}, skipped_invalid={}, dirty_regions={}, dirty_instances={}, dirty_collapsed={}.",
        m_scene_geometry_generation,
        renderable_instance_count,
        leaf_primitive_count,
        m_scene_geometry_bounds.size(),
        skipped_invalid_count,
        m_frame_dirty_regions.size(),
        m_frame_changed_geometry_count,
        m_frame_dirty_regions_collapsed ? 1 : 0
    );
}

void ProbeVolumeResource::RefreshGlobalDirtyEvents(const Scene& scene, bool dirty_tracking_enabled) {
    if (!dirty_tracking_enabled || !scene.IsReady()) {
        m_main_light_snapshot_valid = false;
        return;
    }

    MainLightSnapshot current_light{};
    const entt::entity light_entity = scene.GetMainDirectionalLightEntity();
    current_light.exists = light_entity != entt::null;
    if (current_light.exists) {
        const auto& light = scene.GetMainDirectionalLight();
        current_light.direction = SafeNormalize(light.d_direction, current_light.direction);
        current_light.color     = light.color;
        current_light.intensity = light.intensity;
    }

    if (m_main_light_snapshot_valid &&
        (m_main_light_snapshot.exists != current_light.exists ||
         !NearlyEqual(m_main_light_snapshot.direction, current_light.direction) ||
         !NearlyEqual(m_main_light_snapshot.color, current_light.color) ||
         !NearlyEqual(m_main_light_snapshot.intensity, current_light.intensity))) {
        m_frame_global_dirty_reasons |= RASTER_PROBE_DIRTY_LIGHT;
    }
    if (m_has_snapshot && scene.GetLastTickState().updated_material) {
        m_frame_global_dirty_reasons |= RASTER_PROBE_DIRTY_MATERIAL;
    }

    m_main_light_snapshot = current_light;
    m_main_light_snapshot_valid = true;
    if (m_frame_global_dirty_reasons != 0u) {
        LOG_DEBUG(
            "[ProbeGI] Global dirty event: reasons=0x{:x}, main_light_exists={}, direction={}, color={}, intensity={}.",
            m_frame_global_dirty_reasons,
            current_light.exists ? 1 : 0,
            current_light.direction.ToString(3),
            current_light.color.ToString(3),
            current_light.intensity
        );
    }
}

void ProbeVolumeResource::ApplyDirtyEvents(Snapshot& snapshot) {
    m_last_dirty_region_count = static_cast<uint>(m_frame_dirty_regions.size());
    m_last_global_dirty_reasons = m_frame_global_dirty_reasons;
    m_last_dirty_brick_count = 0u;
    if (!snapshot.dirty_tracking_enabled) {
        m_brick_pending_dirty_reasons.fill(0u);
        return;
    }

    const float influence_distance = snapshot.trace_distance * snapshot.dirty_influence_scale;
    for (uint brick_index = 0u; brick_index < snapshot.brick_count; ++brick_index) {
        BrickSnapshot& brick = snapshot.bricks[brick_index];
        if (!brick.resident) {
            m_brick_pending_dirty_reasons[brick_index] = 0u;
            brick.dirty_flags = 0u;
            continue;
        }

        const uint event_reasons = ProbeDirtyTracker::ResolveReasons(
            brick.sample_bounds,
            m_frame_dirty_regions,
            m_frame_global_dirty_reasons,
            influence_distance
        );
        m_brick_pending_dirty_reasons[brick_index] |= event_reasons;
        brick.dirty_flags = m_brick_pending_dirty_reasons[brick_index];
        if (brick.dirty_flags != 0u) {
            ++m_last_dirty_brick_count;
        }
    }
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

void ProbeVolumeResource::TrackFrameSubmission(CommandList& cmd_list, uint64 frame_index) {
    if (m_gpu_completion_state == nullptr) {
        m_gpu_completion_state = MakeShared<GpuCompletionState>();
    }

    const uint64 submission = frame_index + 1u;
    m_last_submitted_submission = Max(m_last_submitted_submission, submission);
    for (uint brick_index = 0u; brick_index < m_brick_load_awaiting_submission.size(); ++brick_index) {
        if (!m_brick_load_awaiting_submission[brick_index]) {
            continue;
        }
        m_brick_load_submission[brick_index] = submission;
        m_brick_load_awaiting_submission[brick_index] = false;
    }

    SharedPtr<GpuCompletionState> completion_state = m_gpu_completion_state;
    cmd_list.AddCallback([completion_state, submission]() {
        uint64 completed = completion_state->completed_submission.load(std::memory_order_relaxed);
        while (completed < submission &&
               !completion_state->completed_submission.compare_exchange_weak(
                   completed,
                   submission,
                   std::memory_order_release,
                   std::memory_order_relaxed
               )) {
        }
    });
}

ProbeVolumeResource::Snapshot
ProbeVolumeResource::BuildSnapshot(const RasterConfig& config, float3 camera_position, uint64 frame_index) const {
    Snapshot snapshot{};
    snapshot.frame_index                = frame_index;
    snapshot.camera_position            = camera_position;
    snapshot.camera_motion              = m_has_snapshot ? camera_position - m_snapshot.camera_position : float3(0.0f);
    snapshot.enabled                    = config.probe_gi_enabled;
    snapshot.sparse_bricks_enabled     = config.probe_gi_sparse_bricks_enabled;
    snapshot.adaptive_placement_enabled =
        snapshot.enabled && config.probe_gi_adaptive_placement_enabled;
    snapshot.hierarchy_enabled = snapshot.enabled && config.probe_gi_adaptive_hierarchy_enabled;
    snapshot.dirty_tracking_enabled = snapshot.enabled && config.probe_gi_dirty_tracking_enabled;
    snapshot.adaptive_geometry_padding = Max(config.probe_gi_adaptive_geometry_padding, 0.0f);
    snapshot.adaptive_fine_occupancy = Clamp(
        config.probe_gi_adaptive_fine_occupancy,
        1.0f / float(RASTER_PROBE_OCCUPANCY_VOXEL_COUNT),
        1.0f
    );
    snapshot.adaptive_fine_primitives =
        static_cast<uint>(Clamp(config.probe_gi_adaptive_fine_primitives, 1, 512));
    snapshot.adaptive_transition_width = Clamp(config.probe_gi_adaptive_transition_width, 0.0f, 4.0f);
    snapshot.dirty_influence_scale = Clamp(config.probe_gi_dirty_influence_scale, 0.0f, 1.0f);
    snapshot.geometry_generation =
        snapshot.adaptive_placement_enabled ? m_scene_geometry_generation : 0u;
    snapshot.geometry_primitive_count = snapshot.adaptive_placement_enabled ?
                                            static_cast<uint>(m_scene_geometry_bounds.size()) :
                                            0u;
    snapshot.brick_resident_distance   = Max(config.probe_gi_brick_resident_distance, 0.1f);
    snapshot.brick_resident_hysteresis = Max(config.probe_gi_brick_resident_hysteresis, 0.0f);
    snapshot.clipmap_anchor_hysteresis = Clamp(config.probe_gi_clipmap_anchor_hysteresis, 0.0f, 0.49f);
    snapshot.motion_prefetch_enabled = config.probe_gi_motion_prefetch_enabled;
    snapshot.motion_prefetch_threshold = Max(config.probe_gi_motion_prefetch_threshold, 0.0f);
    snapshot.motion_prefetch_keep_frames = static_cast<uint>(Clamp(
        config.probe_gi_motion_prefetch_keep_frames,
        1,
        120
    ));
    snapshot.update_scheduler_enabled  = config.probe_gi_update_scheduler_enabled;
    snapshot.update_brick_budget =
        static_cast<uint>(Clamp(config.probe_gi_update_brick_budget, 1, int(RASTER_PROBE_MAX_BRICK_COUNT)));
    const uint clamped_physical_probe_capacity = static_cast<uint>(Clamp(
        config.probe_gi_physical_probe_capacity,
        int(RASTER_PROBE_BRICK_PROBE_CAPACITY),
        int(RASTER_PROBE_MAX_COUNT)
    ));
    snapshot.physical_probe_capacity = Max(
        (clamped_physical_probe_capacity / RASTER_PROBE_BRICK_PROBE_CAPACITY) *
            RASTER_PROBE_BRICK_PROBE_CAPACITY,
        RASTER_PROBE_BRICK_PROBE_CAPACITY
    );
    snapshot.streaming_enabled = config.probe_gi_streaming_enabled;
    snapshot.streaming_load_budget = static_cast<uint>(Clamp(
        config.probe_gi_streaming_load_budget,
        1,
        int(RASTER_PROBE_MAX_BRICK_COUNT)
    ));
    snapshot.streaming_eviction_budget = static_cast<uint>(Clamp(
        config.probe_gi_streaming_eviction_budget,
        1,
        int(RASTER_PROBE_MAX_BRICK_COUNT)
    ));
    snapshot.debug_mode = static_cast<uint>(Clamp(config.probe_gi_debug_mode, 0, 13));
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
        volume.camera_clipmap   = volume_config.camera_clipmap;
        volume.clipmap_follow_y = volume_config.clipmap_follow_y;
        volume.configured_origin = volume_config.origin;
        volume.extent       = SanitizeExtent(volume_config.extent);
        volume.spacing      = float3(
            volume.count_x > 1 ? volume.extent.x / float(volume.count_x - 1) : volume.extent.x,
            volume.count_y > 1 ? volume.extent.y / float(volume.count_y - 1) : volume.extent.y,
            volume.count_z > 1 ? volume.extent.z / float(volume.count_z - 1) : volume.extent.z
        );
        volume.clipmap_cell_step = ProbeClipmap::GetCellStep(
            volume.extent,
            uint3(volume.count_x, volume.count_y, volume.count_z)
        );
        if (m_has_snapshot) {
            for (uint previous_volume_index = 0u;
                 previous_volume_index < m_snapshot.volume_count;
                 ++previous_volume_index) {
                if (m_snapshot.volumes[previous_volume_index].config_index == config_index) {
                    volume.previous_volume_index = previous_volume_index;
                    break;
                }
            }
        }

        bool previous_anchor_valid = false;
        int3 previous_anchor_cell(0);
        if (volume.previous_volume_index != RASTER_PROBE_PAGE_INVALID) {
            const VolumeSnapshot& previous = m_snapshot.volumes[volume.previous_volume_index];
            previous_anchor_valid = previous.camera_clipmap && volume.camera_clipmap &&
                                    previous.count_x == volume.count_x &&
                                    previous.count_y == volume.count_y &&
                                    previous.count_z == volume.count_z &&
                                    NearlyEqual(previous.configured_origin, volume.configured_origin) &&
                                    NearlyEqual(previous.extent, volume.extent) &&
                                    NearlyEqual(previous.clipmap_cell_step, volume.clipmap_cell_step) &&
                                    previous.clipmap_follow_y == volume.clipmap_follow_y;
            previous_anchor_cell = previous.clipmap_anchor_cell;
        }

        if (volume.camera_clipmap) {
            const ProbeClipmapAnchor anchor = ProbeClipmap::ResolveAnchor(
                camera_position,
                volume.configured_origin,
                volume.extent,
                volume.clipmap_cell_step,
                previous_anchor_cell,
                previous_anchor_valid,
                volume.clipmap_follow_y,
                snapshot.clipmap_anchor_hysteresis
            );
            volume.clipmap_anchor_cell = anchor.cell;
            volume.origin = anchor.origin;
            volume.clipmap_scrolled = previous_anchor_valid && anchor.cell != previous_anchor_cell;
            ++snapshot.clipmap_volume_count;
            if (volume.clipmap_scrolled) {
                ++snapshot.clipmap_scrolled_volume_count;
                LOG_DEBUG(
                    "[ProbeGI] Camera Clipmap anchor scrolled: config_index={}, previous_cell=({}, {}, {}), current_cell=({}, {}, {}), runtime_origin={}, cell_step={}, camera={}, camera_motion={}, follow_y={}, hysteresis={}.",
                    config_index,
                    previous_anchor_cell.x,
                    previous_anchor_cell.y,
                    previous_anchor_cell.z,
                    anchor.cell.x,
                    anchor.cell.y,
                    anchor.cell.z,
                    volume.origin.ToString(2),
                    volume.clipmap_cell_step.ToString(2),
                    camera_position.ToString(2),
                    snapshot.camera_motion.ToString(2),
                    volume.clipmap_follow_y ? 1 : 0,
                    snapshot.clipmap_anchor_hysteresis
                );
            }
        } else {
            volume.origin = volume.configured_origin;
        }
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
        const uint volume_first_brick_index = snapshot.brick_count;
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
            brick.world_fine_coord = subdivision_level == 0u ?
                                         ProbeClipmap::GetWorldFineBrickCoord(
                                             volume.clipmap_anchor_cell,
                                             brick_coord
                                         ) :
                                         int3(
                                             static_cast<int>(brick_coord.x),
                                             static_cast<int>(brick_coord.y),
                                             static_cast<int>(brick_coord.z)
                                         );

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
            const float3 brick_min = volume.origin + level_spacing * float3(brick_start);
            const uint3  brick_last = brick_start + brick.local_counts - uint3(1u);
            const float3 brick_max = volume.origin + level_spacing * float3(brick_last);
            brick.sample_bounds = Box3D(Min(brick_min, brick_max), Max(brick_min, brick_max));

            if (volume.previous_volume_index != RASTER_PROBE_PAGE_INVALID) {
                for (uint previous_brick_index = 0u;
                     previous_brick_index < m_snapshot.brick_count;
                     ++previous_brick_index) {
                    const BrickSnapshot& previous_brick = m_snapshot.bricks[previous_brick_index];
                    if (previous_brick.volume_index != volume.previous_volume_index ||
                        previous_brick.subdivision_level != brick.subdivision_level ||
                        previous_brick.local_counts != brick.local_counts ||
                        previous_brick.probe_count != brick.probe_count) {
                        continue;
                    }

                    const bool same_identity = volume.clipmap_scrolled ?
                                                   brick.subdivision_level == 0u &&
                                                       previous_brick.world_fine_coord ==
                                                           brick.world_fine_coord :
                                                   previous_brick.coord == brick.coord &&
                                                       previous_brick.page_index == brick.page_index &&
                                                       previous_brick.cell_index == brick.cell_index;
                    if (same_identity) {
                        brick.previous_brick_index = previous_brick_index;
                        brick.last_requested_frame = previous_brick.last_requested_frame;
                        brick.last_reused_frame = previous_brick.last_reused_frame;
                        break;
                    }
                }
            }
            const bool same_brick = brick.previous_brick_index != RASTER_PROBE_PAGE_INVALID;
            const bool history_valid = same_brick && m_brick_history_valid[brick.previous_brick_index];
            brick.update_age = history_valid ?
                                   static_cast<uint>(std::min<uint64>(
                                       frame_index - m_brick_last_update_frame[brick.previous_brick_index],
                                       255u
                                   )) :
                                   255u;
            brick.clipmap_reused = same_brick && brick.last_reused_frame != 0u &&
                                   frame_index >= brick.last_reused_frame &&
                                   frame_index - brick.last_reused_frame <=
                                       snapshot.motion_prefetch_keep_frames;

            float resident_distance = snapshot.brick_resident_distance;
            if (distance_gated && snapshot.sparse_bricks_enabled && same_brick &&
                m_snapshot.sparse_bricks_enabled &&
                m_snapshot.bricks[brick.previous_brick_index].requested_resident) {
                resident_distance += snapshot.brick_resident_hysteresis;
            }
            const bool within_resident_distance =
                !distance_gated || !snapshot.sparse_bricks_enabled ||
                brick.camera_distance_sq <= resident_distance * resident_distance;
            brick.placement_requested = level_requested;
            brick.requested_resident = level_requested && within_resident_distance;
            if (brick.requested_resident) {
                brick.last_requested_frame = frame_index;
            }

            ++snapshot.level_brick_count[subdivision_level];
            ++volume.level_brick_count[subdivision_level];
            snapshot.hierarchy_probe_count += brick.probe_count;
            volume.hierarchy_probe_count += brick.probe_count;

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

        bool has_requested_brick = false;
        for (uint brick_index = volume_first_brick_index;
             brick_index < snapshot.brick_count;
             ++brick_index) {
            has_requested_brick |= snapshot.bricks[brick_index].requested_resident;
        }
        if (!snapshot.hierarchy_enabled && snapshot.sparse_bricks_enabled &&
            !has_requested_brick && nearest_brick_index != RASTER_PROBE_PAGE_INVALID) {
            BrickSnapshot& nearest_brick = snapshot.bricks[nearest_brick_index];
            nearest_brick.requested_resident = true;
            nearest_brick.last_requested_frame = frame_index;
        }

        if (snapshot.sparse_bricks_enabled && snapshot.motion_prefetch_enabled) {
            for (uint brick_index = volume_first_brick_index;
                 brick_index < snapshot.brick_count;
                 ++brick_index) {
                BrickSnapshot& brick = snapshot.bricks[brick_index];
                if (brick.subdivision_level != 0u || !brick.placement_requested ||
                    brick.requested_resident ||
                    brick.previous_brick_index == RASTER_PROBE_PAGE_INVALID) {
                    continue;
                }
                const BrickSnapshot& previous =
                    m_snapshot.bricks[brick.previous_brick_index];
                if (previous.prefetched && previous.last_requested_frame != 0u &&
                    frame_index >= previous.last_requested_frame &&
                    frame_index - previous.last_requested_frame <=
                        snapshot.motion_prefetch_keep_frames) {
                    brick.requested_resident = true;
                    brick.prefetched = true;
                }
            }

            float3 prefetch_motion = snapshot.camera_motion;
            if (volume.camera_clipmap && !volume.clipmap_follow_y) {
                prefetch_motion.y = 0.0f;
            }
            const int3 prefetch_offset = ProbeClipmap::GetDominantPrefetchOffset(
                prefetch_motion,
                snapshot.motion_prefetch_threshold
            );
            if (prefetch_offset.x != 0 || prefetch_offset.y != 0 || prefetch_offset.z != 0) {
                for (uint brick_index = volume_first_brick_index;
                     brick_index < snapshot.brick_count;
                     ++brick_index) {
                    const BrickSnapshot& source_brick = snapshot.bricks[brick_index];
                    if (source_brick.subdivision_level != 0u ||
                        !source_brick.requested_resident || source_brick.prefetched) {
                        continue;
                    }

                    uint3 neighbor_coord;
                    if (!ProbeClipmap::ResolveNeighborCoord(
                            source_brick.coord,
                            prefetch_offset,
                            fine_brick_counts,
                            neighbor_coord
                        )) {
                        continue;
                    }

                    for (uint neighbor_index = volume_first_brick_index;
                         neighbor_index < snapshot.brick_count;
                         ++neighbor_index) {
                        BrickSnapshot& neighbor = snapshot.bricks[neighbor_index];
                        if (neighbor.subdivision_level != 0u || !neighbor.placement_requested ||
                            neighbor.coord != neighbor_coord) {
                            continue;
                        }
                        if (!neighbor.requested_resident || neighbor.prefetched) {
                            neighbor.requested_resident = true;
                            neighbor.prefetched = true;
                            neighbor.last_requested_frame = frame_index;
                        }
                        break;
                    }
                }
            }
        }

        for (uint brick_index = volume_first_brick_index;
             brick_index < snapshot.brick_count;
             ++brick_index) {
            const BrickSnapshot& brick = snapshot.bricks[brick_index];
            if (!brick.requested_resident) {
                continue;
            }
            ++snapshot.requested_brick_count;
            snapshot.requested_probe_count += brick.probe_count;
            ++snapshot.level_requested_brick_count[brick.subdivision_level];
            ++volume.requested_brick_count;
            volume.requested_probe_count += brick.probe_count;
            ++volume.level_requested_brick_count[brick.subdivision_level];
            if (brick.cell_index != RASTER_PROBE_PAGE_INVALID) {
                ++snapshot.cells[brick.cell_index].requested_brick_count;
            }
            if (brick.prefetched) {
                ++snapshot.prefetched_brick_count;
                ++volume.prefetched_brick_count;
            }
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
    if (!m_has_snapshot || m_snapshot.physical_probe_capacity != snapshot.physical_probe_capacity ||
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
    m_physical_allocation_pages.fill(RASTER_PROBE_PAGE_INVALID);
    m_physical_allocation_generations.fill(0u);
    m_brick_load_submission.fill(0u);
    m_brick_load_awaiting_submission.fill(false);
    m_physical_allocator.Reset(physical_probe_capacity);
    m_brick_history_valid.fill(false);
    m_brick_last_update_frame.fill(0);
    m_brick_pending_dirty_reasons.fill(0u);
    LOG_DEBUG("[ProbeGI] Physical Probe allocator reset: capacity={}.", physical_probe_capacity);
}

uint ProbeVolumeResource::AdvancePageGeneration(uint virtual_page) {
    if (virtual_page >= m_page_generations.size()) {
        return 1u;
    }
    m_page_generations[virtual_page] = NextProbePageGeneration(m_page_generations[virtual_page]);
    return m_page_generations[virtual_page];
}

void ProbeVolumeResource::RetirePhysicalAllocation(uint brick_index, uint virtual_page) {
    if (brick_index >= m_physical_allocations.size()) {
        return;
    }

    ProbePhysicalAllocator::Allocation& allocation = m_physical_allocations[brick_index];
    if (allocation.valid && allocation.probe_count > 0u) {
        if (virtual_page == RASTER_PROBE_PAGE_INVALID) {
            virtual_page = m_physical_allocation_pages[brick_index];
        }
        LOG_DEBUG(
            "[ProbeGI] Physical Probe range retired: brick={}, page={}, generation={}, offset={}, count={}, safe_after_submission={}.",
            brick_index,
            virtual_page,
            m_physical_allocation_generations[brick_index],
            allocation.probe_offset,
            allocation.probe_count,
            m_last_submitted_submission
        );
        if (!m_retirement_queue.Enqueue(ProbeRetirementQueue::Entry{
                allocation,
                brick_index,
                virtual_page,
                m_physical_allocation_generations[brick_index],
                m_last_submitted_submission,
            })) {
            LOG_ERROR(
                "[ProbeGI] Physical Probe retirement rejected: brick={}, offset={}, count={}.",
                brick_index,
                allocation.probe_offset,
                allocation.probe_count
            );
        }
    }
    allocation = {};
    m_physical_allocation_pages[brick_index] = RASTER_PROBE_PAGE_INVALID;
    m_physical_allocation_generations[brick_index] = 0u;
    m_brick_load_submission[brick_index] = 0u;
    m_brick_load_awaiting_submission[brick_index] = false;
    m_brick_history_valid[brick_index] = false;
    m_brick_last_update_frame[brick_index] = 0;
    m_brick_pending_dirty_reasons[brick_index] = 0u;
}

void ProbeVolumeResource::CollectRetiredAllocations(Snapshot& snapshot) {
    const uint64 completed_submission = m_gpu_completion_state != nullptr ?
                                            m_gpu_completion_state->completed_submission.load(
                                                std::memory_order_acquire
                                            ) :
                                            0u;
    const ProbeRetirementQueue::CollectResult collected =
        m_retirement_queue.Collect(completed_submission, m_physical_allocator);
    snapshot.streaming_reclaimed_allocation_count += collected.allocation_count;
    if (collected.allocation_count != 0u || collected.failed_count != 0u) {
        LOG_DEBUG(
            "[ProbeGI] Physical Probe retire collection: completed_submission={}, reclaimed_allocations={}, reclaimed_probes={}, failed={}, remaining_allocations={}, remaining_probes={}.",
            completed_submission,
            collected.allocation_count,
            collected.probe_count,
            collected.failed_count,
            m_retirement_queue.GetAllocationCount(),
            m_retirement_queue.GetProbeCount()
        );
    }
    if (collected.failed_count != 0u) {
        LOG_ERROR("[ProbeGI] Physical Probe allocator rejected {} retired ranges.", collected.failed_count);
    }
}

void ProbeVolumeResource::RebindPhysicalAllocations(Snapshot& snapshot, bool allow_reuse) {
    StaticArray<ProbePhysicalAllocator::Allocation, RASTER_PROBE_MAX_BRICK_COUNT> next_allocations{};
    StaticArray<uint, RASTER_PROBE_MAX_BRICK_COUNT> next_pages{};
    StaticArray<uint, RASTER_PROBE_MAX_BRICK_COUNT> next_generations{};
    StaticArray<uint64, RASTER_PROBE_MAX_BRICK_COUNT> next_load_submissions{};
    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT> next_load_awaiting_submission{};
    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT> next_history_valid{};
    StaticArray<uint64, RASTER_PROBE_MAX_BRICK_COUNT> next_last_update_frame{};
    StaticArray<uint, RASTER_PROBE_MAX_BRICK_COUNT> next_pending_dirty_reasons{};
    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT> previous_allocation_used{};
    next_pages.fill(RASTER_PROBE_PAGE_INVALID);

    if (allow_reuse && m_has_snapshot) {
        for (uint brick_index = 0u; brick_index < snapshot.brick_count; ++brick_index) {
            BrickSnapshot& brick = snapshot.bricks[brick_index];
            const uint previous_index = brick.previous_brick_index;
            if (previous_index == RASTER_PROBE_PAGE_INVALID ||
                previous_index >= m_snapshot.brick_count ||
                previous_allocation_used[previous_index] ||
                !m_physical_allocations[previous_index].valid) {
                continue;
            }

            const BrickSnapshot& previous_brick = m_snapshot.bricks[previous_index];
            if (m_physical_allocation_pages[previous_index] != previous_brick.page_index ||
                previous_brick.local_counts != brick.local_counts ||
                previous_brick.probe_count != brick.probe_count) {
                continue;
            }

            const VolumeSnapshot& volume = snapshot.volumes[brick.volume_index];
            const bool clipmap_rebind = volume.clipmap_scrolled &&
                                        brick.subdivision_level == 0u &&
                                        previous_brick.world_fine_coord == brick.world_fine_coord;
            if (volume.clipmap_scrolled && !clipmap_rebind) {
                continue;
            }

            next_allocations[brick_index] = m_physical_allocations[previous_index];
            next_pages[brick_index] = brick.page_index;
            next_generations[brick_index] = clipmap_rebind ?
                                                AdvancePageGeneration(brick.page_index) :
                                                m_physical_allocation_generations[previous_index];
            next_load_submissions[brick_index] = m_brick_load_submission[previous_index];
            next_load_awaiting_submission[brick_index] =
                m_brick_load_awaiting_submission[previous_index];
            next_history_valid[brick_index] = m_brick_history_valid[previous_index];
            next_last_update_frame[brick_index] = m_brick_last_update_frame[previous_index];
            next_pending_dirty_reasons[brick_index] =
                m_brick_pending_dirty_reasons[previous_index];
            previous_allocation_used[previous_index] = true;

            if (clipmap_rebind) {
                brick.clipmap_reused = true;
                brick.last_reused_frame = snapshot.frame_index;
                ++snapshot.clipmap_reused_brick_count;
                ++snapshot.volumes[brick.volume_index].clipmap_reused_brick_count;
                LOG_DEBUG(
                    "[ProbeGI] Clipmap L0 Brick rebound: volume={}, world_brick=({}, {}, {}), old_brick={}, new_brick={}, old_page={}, new_page={}, generation={}, physical_offset={}, history_valid={}.",
                    brick.volume_index,
                    brick.world_fine_coord.x,
                    brick.world_fine_coord.y,
                    brick.world_fine_coord.z,
                    previous_index,
                    brick_index,
                    previous_brick.page_index,
                    brick.page_index,
                    next_generations[brick_index],
                    next_allocations[brick_index].probe_offset,
                    next_history_valid[brick_index] ? 1 : 0
                );
            }
        }
    }

    for (uint previous_index = 0u; previous_index < m_physical_allocations.size(); ++previous_index) {
        if (!m_physical_allocations[previous_index].valid ||
            previous_allocation_used[previous_index]) {
            continue;
        }
        RetirePhysicalAllocation(previous_index, m_physical_allocation_pages[previous_index]);
    }

    m_physical_allocations = next_allocations;
    m_physical_allocation_pages = next_pages;
    m_physical_allocation_generations = next_generations;
    m_brick_load_submission = next_load_submissions;
    m_brick_load_awaiting_submission = next_load_awaiting_submission;
    m_brick_history_valid = next_history_valid;
    m_brick_last_update_frame = next_last_update_frame;
    m_brick_pending_dirty_reasons = next_pending_dirty_reasons;
}

void ProbeVolumeResource::ApplyPhysicalResidency(
    Snapshot& snapshot,
    const StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT>& volume_history_reset
) {
    snapshot.page_table.fill(RASTER_PROBE_PAGE_INVALID);
    snapshot.resident_brick_count = 0;
    snapshot.resident_probe_count = 0;
    snapshot.published_brick_count = 0;
    snapshot.pending_load_brick_count = 0;
    snapshot.cached_brick_count = 0;
    snapshot.retiring_brick_count = 0;
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

    CollectRetiredAllocations(snapshot);

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
    uint selected_physical_probe_count = 0;
    for (uint brick_index : requested_candidates) {
        if (selected_physical_probe_count + RASTER_PROBE_BRICK_PROBE_CAPACITY >
            snapshot.physical_probe_capacity) {
            continue;
        }
        selected_bricks[brick_index] = true;
        selected_physical_probe_count += RASTER_PROBE_BRICK_PROBE_CAPACITY;
    }
    snapshot.capacity_evicted_brick_count =
        static_cast<uint>(requested_candidates.size()) -
        static_cast<uint>(std::count(selected_bricks.begin(), selected_bricks.end(), true));

    const bool layout_changed = RequiresPhysicalAllocatorReset(snapshot) ||
                                snapshot.clipmap_scrolled_volume_count != 0u;
    if (layout_changed) {
        ++m_layout_generation;
        if (m_layout_generation == 0u) {
            ++m_layout_generation;
        }
    }
    snapshot.layout_generation = m_layout_generation;

    const uint current_allocator_capacity = m_physical_allocator.GetCapacity();
    if (current_allocator_capacity == 0u && m_retirement_queue.Empty()) {
        ResetPhysicalAllocator(snapshot.physical_probe_capacity);
    } else if (current_allocator_capacity != snapshot.physical_probe_capacity) {
        m_pending_physical_capacity = snapshot.physical_probe_capacity;
    }
    RebindPhysicalAllocations(snapshot, m_pending_physical_capacity == 0u);

    Array<uint> eviction_candidates;
    for (uint brick_index = 0u; brick_index < snapshot.brick_count; ++brick_index) {
        if (m_physical_allocations[brick_index].valid && !selected_bricks[brick_index]) {
            eviction_candidates.push_back(brick_index);
        }
    }
    std::sort(
        eviction_candidates.begin(),
        eviction_candidates.end(),
        [&](uint lhs_index, uint rhs_index) {
            const BrickSnapshot& lhs = snapshot.bricks[lhs_index];
            const BrickSnapshot& rhs = snapshot.bricks[rhs_index];
            if (lhs.requested_resident != rhs.requested_resident) {
                return !lhs.requested_resident;
            }
            if (lhs.subdivision_level != rhs.subdivision_level) {
                return lhs.subdivision_level < rhs.subdivision_level;
            }
            if (!NearlyEqual(lhs.camera_distance_sq, rhs.camera_distance_sq)) {
                return lhs.camera_distance_sq > rhs.camera_distance_sq;
            }
            return lhs_index > rhs_index;
        }
    );

    const uint eviction_budget = snapshot.streaming_enabled ?
                                     snapshot.streaming_eviction_budget :
                                     RASTER_PROBE_MAX_BRICK_COUNT;
    for (uint brick_index : eviction_candidates) {
        if (snapshot.streaming_evicted_brick_count >= eviction_budget) {
            break;
        }
        RetirePhysicalAllocation(brick_index, snapshot.bricks[brick_index].page_index);
        ++snapshot.streaming_evicted_brick_count;
    }

    CollectRetiredAllocations(snapshot);

    if (m_pending_physical_capacity != 0u && m_retirement_queue.Empty()) {
        const bool has_active_allocations = std::any_of(
            m_physical_allocations.begin(),
            m_physical_allocations.end(),
            [](const ProbePhysicalAllocator::Allocation& allocation) { return allocation.valid; }
        );
        if (!has_active_allocations) {
            const uint new_capacity = m_pending_physical_capacity;
            m_pending_physical_capacity = 0u;
            ResetPhysicalAllocator(new_capacity);
        }
    }

    for (uint brick_index = 0u; brick_index < snapshot.brick_count; ++brick_index) {
        if (!m_physical_allocations[brick_index].valid ||
            !volume_history_reset[snapshot.bricks[brick_index].volume_index]) {
            continue;
        }
        const uint virtual_page = snapshot.bricks[brick_index].page_index;
        const uint generation = AdvancePageGeneration(virtual_page);
        m_physical_allocation_generations[brick_index] = generation;
        m_brick_load_submission[brick_index] = 0u;
        m_brick_load_awaiting_submission[brick_index] = false;
        m_brick_history_valid[brick_index] = false;
        m_brick_last_update_frame[brick_index] = 0u;
    }

    const uint load_budget = snapshot.streaming_enabled ?
                                 snapshot.streaming_load_budget :
                                 RASTER_PROBE_MAX_BRICK_COUNT;
    if (m_pending_physical_capacity == 0u) {
        for (uint brick_index : requested_candidates) {
            if (!selected_bricks[brick_index] || m_physical_allocations[brick_index].valid) {
                continue;
            }
            if (snapshot.streaming_loaded_brick_count >= load_budget) {
                break;
            }

            ProbePhysicalAllocator::Allocation allocation;
            if (!m_physical_allocator.Allocate(RASTER_PROBE_BRICK_PROBE_CAPACITY, allocation)) {
                ++snapshot.streaming_allocation_stall_count;
                break;
            }
            m_physical_allocations[brick_index] = allocation;
            m_physical_allocation_pages[brick_index] = snapshot.bricks[brick_index].page_index;
            m_physical_allocation_generations[brick_index] =
                AdvancePageGeneration(snapshot.bricks[brick_index].page_index);
            m_brick_load_submission[brick_index] = 0u;
            m_brick_load_awaiting_submission[brick_index] = false;
            m_brick_history_valid[brick_index] = false;
            m_brick_last_update_frame[brick_index] = 0u;
            ++snapshot.streaming_loaded_brick_count;
            LOG_DEBUG(
                "[ProbeGI] Physical Probe tile loading: brick={}, volume={}, page={}, generation={}, offset={}, tile_probes={}, local_probes={}.",
                brick_index,
                snapshot.bricks[brick_index].volume_index,
                snapshot.bricks[brick_index].page_index,
                m_physical_allocation_generations[brick_index],
                allocation.probe_offset,
                allocation.probe_count,
                snapshot.bricks[brick_index].probe_count
            );
        }
    }

    const uint64 completed_submission = m_gpu_completion_state != nullptr ?
                                            m_gpu_completion_state->completed_submission.load(
                                                std::memory_order_acquire
                                            ) :
                                            0u;

    for (uint brick_index = 0; brick_index < snapshot.brick_count; ++brick_index) {
        BrickSnapshot& brick = snapshot.bricks[brick_index];
        const ProbePhysicalAllocator::Allocation& allocation = m_physical_allocations[brick_index];
        brick.resident = allocation.valid &&
                         m_physical_allocation_pages[brick_index] == brick.page_index;
        brick.cached = brick.resident && !selected_bricks[brick_index];
        brick.page_generation = brick.resident ?
                                    m_physical_allocation_generations[brick_index] :
                                    NormalizeProbePageGeneration(m_page_generations[brick.page_index]);
        if (!brick.resident) {
            brick.probe_offset = RASTER_PROBE_PAGE_INVALID;
            brick.update_age = 255u;
            if (selected_bricks[brick_index]) {
                brick.streaming_state = RASTER_PROBE_STREAMING_PENDING_LOAD;
                ++snapshot.pending_load_brick_count;
            } else if (m_retirement_queue.ContainsVirtualPage(brick.page_index)) {
                brick.streaming_state = RASTER_PROBE_STREAMING_RETIRING;
                ++snapshot.retiring_brick_count;
            } else {
                brick.streaming_state = RASTER_PROBE_STREAMING_UNMAPPED;
            }
            snapshot.page_table[brick.page_index] = PackProbePageEntry(
                brick_index,
                brick.page_generation,
                brick.streaming_state
            );
            continue;
        }

        brick.probe_offset = allocation.probe_offset;
        if (!m_brick_history_valid[brick_index]) {
            brick.update_age = 255u;
        }
        const uint64 load_submission = m_brick_load_submission[brick_index];
        const bool published = load_submission != 0u && load_submission <= completed_submission;
        brick.streaming_state = published ?
                                    RASTER_PROBE_STREAMING_RESIDENT :
                                    RASTER_PROBE_STREAMING_PENDING_LOAD;
        if (published) {
            ++snapshot.published_brick_count;
        } else {
            ++snapshot.pending_load_brick_count;
        }
        if (brick.cached) {
            ++snapshot.cached_brick_count;
        }
        snapshot.page_table[brick.page_index] = PackProbePageEntry(
            brick_index,
            brick.page_generation,
            brick.streaming_state
        );
        ++snapshot.resident_brick_count;
        snapshot.resident_probe_count += brick.probe_count;
        ++snapshot.level_resident_brick_count[brick.subdivision_level];
        ++snapshot.physical_allocation_count;
        snapshot.allocated_physical_probe_count += allocation.probe_count;

        VolumeSnapshot& volume = snapshot.volumes[brick.volume_index];
        ++volume.resident_brick_count;
        volume.resident_probe_count += brick.probe_count;
        ++volume.level_resident_brick_count[brick.subdivision_level];
        if (brick.cell_index != RASTER_PROBE_PAGE_INVALID) {
            ++snapshot.cells[brick.cell_index].resident_brick_count;
        }
    }

    snapshot.physical_allocator_capacity = m_physical_allocator.GetCapacity();
    snapshot.free_physical_probe_count = m_physical_allocator.GetFreeProbeCount();
    snapshot.retiring_allocation_count = m_retirement_queue.GetAllocationCount();
    snapshot.retiring_probe_count = m_retirement_queue.GetProbeCount();
    const uint accounted_probe_count = snapshot.allocated_physical_probe_count +
                                       snapshot.retiring_probe_count +
                                       snapshot.free_physical_probe_count;
    if (accounted_probe_count != snapshot.physical_allocator_capacity) {
        LOG_ERROR(
            "[ProbeGI] Physical Probe allocator accounting mismatch: resident={}, retiring={}, free={}, accounted={}, capacity={}.",
            snapshot.allocated_physical_probe_count,
            snapshot.retiring_probe_count,
            snapshot.free_physical_probe_count,
            accounted_probe_count,
            snapshot.physical_allocator_capacity
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
        m_snapshot.dirty_tracking_enabled != snapshot.dirty_tracking_enabled ||
        m_snapshot.debug_mode != snapshot.debug_mode ||
        m_snapshot.volume_count != snapshot.volume_count || m_snapshot.cell_count != snapshot.cell_count ||
        m_snapshot.brick_count != snapshot.brick_count ||
        m_snapshot.max_subdivision_level != snapshot.max_subdivision_level ||
        m_snapshot.layout_generation != snapshot.layout_generation ||
        m_snapshot.requested_brick_count != snapshot.requested_brick_count ||
        m_snapshot.requested_probe_count != snapshot.requested_probe_count ||
        m_snapshot.resident_brick_count != snapshot.resident_brick_count ||
        m_snapshot.resident_probe_count != snapshot.resident_probe_count ||
        m_snapshot.published_brick_count != snapshot.published_brick_count ||
        m_snapshot.pending_load_brick_count != snapshot.pending_load_brick_count ||
        m_snapshot.cached_brick_count != snapshot.cached_brick_count ||
        m_snapshot.retiring_brick_count != snapshot.retiring_brick_count ||
        m_snapshot.total_count != snapshot.total_count ||
        m_snapshot.hierarchy_probe_count != snapshot.hierarchy_probe_count ||
        m_snapshot.physical_probe_capacity != snapshot.physical_probe_capacity ||
        m_snapshot.physical_allocator_capacity != snapshot.physical_allocator_capacity ||
        m_snapshot.allocated_physical_probe_count != snapshot.allocated_physical_probe_count ||
        m_snapshot.physical_allocation_count != snapshot.physical_allocation_count ||
        m_snapshot.free_physical_probe_count != snapshot.free_physical_probe_count ||
        m_snapshot.retiring_allocation_count != snapshot.retiring_allocation_count ||
        m_snapshot.retiring_probe_count != snapshot.retiring_probe_count ||
        m_snapshot.streaming_loaded_brick_count != snapshot.streaming_loaded_brick_count ||
        m_snapshot.streaming_evicted_brick_count != snapshot.streaming_evicted_brick_count ||
        m_snapshot.streaming_reclaimed_allocation_count !=
            snapshot.streaming_reclaimed_allocation_count ||
        m_snapshot.streaming_allocation_stall_count != snapshot.streaming_allocation_stall_count ||
        m_snapshot.capacity_evicted_brick_count != snapshot.capacity_evicted_brick_count ||
        m_snapshot.clipmap_volume_count != snapshot.clipmap_volume_count ||
        m_snapshot.clipmap_scrolled_volume_count != snapshot.clipmap_scrolled_volume_count ||
        m_snapshot.prefetched_brick_count != snapshot.prefetched_brick_count ||
        m_snapshot.clipmap_reused_brick_count != snapshot.clipmap_reused_brick_count ||
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
        !NearlyEqual(m_snapshot.dirty_influence_scale, snapshot.dirty_influence_scale) ||
        !NearlyEqual(m_snapshot.brick_resident_distance, snapshot.brick_resident_distance) ||
        !NearlyEqual(m_snapshot.brick_resident_hysteresis, snapshot.brick_resident_hysteresis) ||
        !NearlyEqual(m_snapshot.clipmap_anchor_hysteresis, snapshot.clipmap_anchor_hysteresis) ||
        m_snapshot.motion_prefetch_enabled != snapshot.motion_prefetch_enabled ||
        !NearlyEqual(m_snapshot.motion_prefetch_threshold, snapshot.motion_prefetch_threshold) ||
        m_snapshot.motion_prefetch_keep_frames != snapshot.motion_prefetch_keep_frames ||
        m_snapshot.streaming_enabled != snapshot.streaming_enabled ||
        m_snapshot.streaming_load_budget != snapshot.streaming_load_budget ||
        m_snapshot.streaming_eviction_budget != snapshot.streaming_eviction_budget ||
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
            lhs.camera_clipmap != rhs.camera_clipmap ||
            lhs.clipmap_follow_y != rhs.clipmap_follow_y ||
            lhs.clipmap_scrolled != rhs.clipmap_scrolled ||
            lhs.prefetched_brick_count != rhs.prefetched_brick_count ||
            lhs.clipmap_reused_brick_count != rhs.clipmap_reused_brick_count ||
            lhs.clipmap_anchor_cell != rhs.clipmap_anchor_cell ||
            !NearlyEqual(lhs.configured_origin, rhs.configured_origin) ||
            !NearlyEqual(lhs.clipmap_cell_step, rhs.clipmap_cell_step) ||
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
            lhs.placement_requested != rhs.placement_requested ||
            lhs.requested_resident != rhs.requested_resident || lhs.resident != rhs.resident ||
            lhs.page_generation != rhs.page_generation ||
            lhs.streaming_state != rhs.streaming_state || lhs.cached != rhs.cached ||
            lhs.prefetched != rhs.prefetched || lhs.clipmap_reused != rhs.clipmap_reused ||
            lhs.world_fine_coord != rhs.world_fine_coord) {
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
    const bool fixed_origin_changed =
        !current.camera_clipmap && !NearlyEqual(previous.origin, current.origin);
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
           previous.camera_clipmap != current.camera_clipmap ||
           previous.clipmap_follow_y != current.clipmap_follow_y ||
           !NearlyEqual(previous.configured_origin, current.configured_origin) ||
           !NearlyEqual(previous.clipmap_cell_step, current.clipmap_cell_step) ||
           fixed_origin_changed ||
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
            uint4(brick.probe_offset, brick.probe_count, brick.resident ? 1u : 0u, brick.page_generation);
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
            PackProbeStreamingFlags(
                brick.dirty_flags,
                brick.streaming_state,
                brick.cached,
                brick.prefetched,
                brick.clipmap_reused
            )
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
    m_last_dirty_brick_count = 0;
    m_last_scheduled_dirty_brick_count = 0;
    m_last_deferred_dirty_brick_count = 0;
    m_last_dirty_region_count = 0;
    m_last_global_dirty_reasons = 0u;
    m_frame_dirty_regions.clear();
    m_frame_global_dirty_reasons = 0u;
    m_frame_changed_geometry_count = 0u;
    m_frame_dirty_regions_collapsed = false;

    if (m_probe_buffer.buf == nullptr || m_volume_buffer.buf == nullptr || m_cell_buffer.buf == nullptr ||
        m_brick_buffer.buf == nullptr || m_page_table_buffer.buf == nullptr ||
        m_visibility_atlas_buffer.buf == nullptr || m_irradiance_atlas_buffer.buf == nullptr) {
        return update_info;
    }

    const bool dirty_tracking_enabled = config.probe_gi_enabled && config.probe_gi_dirty_tracking_enabled;
    if (config.probe_gi_enabled &&
        (config.probe_gi_adaptive_placement_enabled || dirty_tracking_enabled)) {
        RefreshSceneGeometry(scene, dirty_tracking_enabled);
    } else {
        m_scene_geometry_cache_valid = false;
    }
    RefreshGlobalDirtyEvents(scene, dirty_tracking_enabled);

    Snapshot snapshot = BuildSnapshot(config, camera_position, frame_index);
    StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT> volume_history_reset{};
    for (uint volume_index = 0u; volume_index < snapshot.volume_count; ++volume_index) {
        volume_history_reset[volume_index] = RequiresHistoryReset(snapshot, volume_index);
    }
    ApplyPhysicalResidency(snapshot, volume_history_reset);
    const uint previous_prefetched_brick_count =
        m_has_snapshot ? m_snapshot.prefetched_brick_count : 0u;
    if (snapshot.clipmap_scrolled_volume_count != 0u ||
        snapshot.clipmap_reused_brick_count != 0u ||
        ((snapshot.prefetched_brick_count != previous_prefetched_brick_count) &&
         (snapshot.prefetched_brick_count != 0u || previous_prefetched_brick_count != 0u))) {
        LOG_DEBUG(
            "[ProbeGI] Clipmap residency decision: camera={}, motion={}, clipmap_volumes={}, scrolled_volumes={}, prefetched_bricks={}, reused_l0_bricks={}, requested_bricks={}/{}, loaded_this_frame={}, evicted_this_frame={}, retiring_allocations={}, load_budget={}, prefetch_threshold={}, keep_frames={}.",
            snapshot.camera_position.ToString(2),
            snapshot.camera_motion.ToString(2),
            snapshot.clipmap_volume_count,
            snapshot.clipmap_scrolled_volume_count,
            snapshot.prefetched_brick_count,
            snapshot.clipmap_reused_brick_count,
            snapshot.requested_brick_count,
            snapshot.brick_count,
            snapshot.streaming_loaded_brick_count,
            snapshot.streaming_evicted_brick_count,
            snapshot.retiring_allocation_count,
            snapshot.streaming_load_budget,
            snapshot.motion_prefetch_threshold,
            snapshot.motion_prefetch_keep_frames
        );
    }
    ApplyDirtyEvents(snapshot);
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
        m_brick_pending_dirty_reasons.fill(0u);
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
    update_info.dirty_brick_count    = m_last_dirty_brick_count;

    StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT> next_history_valid{};
    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT>  next_brick_history_valid{};
    for (uint volume_index = 0; volume_index < snapshot.volume_count; ++volume_index) {
        const bool volume_reset = volume_history_reset[volume_index];
        const VolumeSnapshot& volume = snapshot.volumes[volume_index];
        volume_history_reset[volume_index] = volume_reset;
        next_history_valid[volume_index] = true;

        if (changed || volume_reset) {
            LOG_DEBUG(
                "[ProbeGI] Volume residency updated: slot={}, config_index={}, count=({}, {}, {}) base_probes={}, hierarchy_probes={}, logical_offset={}, cells={} range=[{}, {}), max_level={}, level_bricks=({}, {}, {}), requested_bricks={}/{}, requested_levels=({}, {}, {}), requested_probes={}, prefetched_bricks={}, resident_bricks={}/{}, resident_levels=({}, {}, {}), resident_probes={}, page_table_offset={}, configured_origin={}, runtime_origin={}, extent={}, spacing={}, intensity={}, blend_distance={}, clipmap={}, follow_y={}, anchor=({}, {}, {}), cell_step={}, scrolled={}, reused_l0={}, history_reset={}",
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
                volume.prefetched_brick_count,
                volume.resident_brick_count,
                volume.brick_count,
                volume.level_resident_brick_count[0],
                volume.level_resident_brick_count[1],
                volume.level_resident_brick_count[2],
                volume.resident_probe_count,
                volume.page_table_offset,
                volume.configured_origin.ToString(2),
                volume.origin.ToString(2),
                volume.extent.ToString(2),
                volume.spacing.ToString(2),
                volume.intensity,
                volume.blend_distance,
                volume.camera_clipmap ? 1 : 0,
                volume.clipmap_follow_y ? 1 : 0,
                volume.clipmap_anchor_cell.x,
                volume.clipmap_anchor_cell.y,
                volume.clipmap_anchor_cell.z,
                volume.clipmap_cell_step.ToString(2),
                volume.clipmap_scrolled ? 1 : 0,
                volume.clipmap_reused_brick_count,
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
            const bool lhs_dirty = (m_brick_pending_dirty_reasons[lhs_index] &
                                    RASTER_PROBE_DIRTY_REASON_MASK) != 0u;
            const bool rhs_dirty = (m_brick_pending_dirty_reasons[rhs_index] &
                                    RASTER_PROBE_DIRTY_REASON_MASK) != 0u;
            if (lhs_dirty != rhs_dirty) {
                return lhs_dirty;
            }
            if (lhs_dirty && lhs.subdivision_level != rhs.subdivision_level) {
                return lhs.subdivision_level > rhs.subdivision_level;
            }
            if (lhs.update_age != rhs.update_age) {
                return lhs.update_age > rhs.update_age;
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

    for (uint brick_index : resident_candidates) {
        BrickSnapshot& brick = snapshot.bricks[brick_index];
        const bool mandatory =
            volume_history_reset[brick.volume_index] || !m_brick_history_valid[brick_index];
        const uint dirty_reasons =
            m_brick_pending_dirty_reasons[brick_index] & RASTER_PROBE_DIRTY_REASON_MASK;
        if (snapshot.update_scheduler_enabled && !mandatory &&
            update_info.job_count >= snapshot.update_brick_budget) {
            ++update_info.deferred_brick_count;
            if (dirty_reasons != 0u) {
                ++update_info.deferred_dirty_brick_count;
            }
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
        ++update_info.scheduled_level_brick_count[brick.subdivision_level];
        next_brick_history_valid[brick_index] = true;
        m_brick_last_update_frame[brick_index] = frame_index;
        brick.update_age = 0;
        if (mandatory && brick.streaming_state == RASTER_PROBE_STREAMING_PENDING_LOAD) {
            m_brick_load_submission[brick_index] = 0u;
            m_brick_load_awaiting_submission[brick_index] = true;
        }
        if (dirty_reasons != 0u) {
            ++update_info.scheduled_dirty_brick_count;
            ++update_info.scheduled_dirty_level_brick_count[brick.subdivision_level];
            brick.dirty_flags = dirty_reasons | RASTER_PROBE_DIRTY_SCHEDULED;
            m_brick_pending_dirty_reasons[brick_index] = 0u;
        }
    }

    m_last_scheduled_brick_count = update_info.job_count;
    m_last_scheduled_probe_count = update_info.scheduled_probe_count;
    m_last_scheduled_dirty_brick_count = update_info.scheduled_dirty_brick_count;
    m_last_deferred_dirty_brick_count = update_info.deferred_dirty_brick_count;

    if (m_frame_changed_geometry_count != 0u || m_frame_global_dirty_reasons != 0u ||
        update_info.scheduled_dirty_brick_count != 0u || update_info.deferred_dirty_brick_count != 0u) {
        LOG_DEBUG(
            "[ProbeGI] Dirty update scheduler: regions={}, changed_instances={}, collapsed={}, global_reasons=0x{:x}, influence_distance={}, dirty_bricks={}, scheduled_dirty={}, scheduled_dirty_levels=({}, {}, {}), deferred_dirty={}, scheduled_total={}/{}, scheduled_levels=({}, {}, {}), budget={}.",
            m_frame_dirty_regions.size(),
            m_frame_changed_geometry_count,
            m_frame_dirty_regions_collapsed ? 1 : 0,
            m_frame_global_dirty_reasons,
            snapshot.trace_distance * snapshot.dirty_influence_scale,
            update_info.dirty_brick_count,
            update_info.scheduled_dirty_brick_count,
            update_info.scheduled_dirty_level_brick_count[0],
            update_info.scheduled_dirty_level_brick_count[1],
            update_info.scheduled_dirty_level_brick_count[2],
            update_info.deferred_dirty_brick_count,
            update_info.job_count,
            snapshot.resident_brick_count,
            update_info.scheduled_level_brick_count[0],
            update_info.scheduled_level_brick_count[1],
            update_info.scheduled_level_brick_count[2],
            snapshot.update_brick_budget
        );
    }

    if (changed) {
        LOG_DEBUG(
            "[ProbeGI] Multi-volume brick residency updated: active_volumes={}, cells={}, hierarchy={}, max_level={}, layout_generation={}, base_probes={}, hierarchy_probes={}, level_bricks=({}, {}, {}), requested_bricks={}/{}, requested_levels=({}, {}, {}), requested_probes={}/{}, physical_resident_bricks={}/{}, resident_levels=({}, {}, {}), resident_local_probes={}/{}, published_bricks={}, pending_load_bricks={}, cached_bricks={}, retiring_pages={}, active_allocations={}, active_physical_probes={}/{}, target_physical_capacity={}, free_physical_probes={}, retiring_allocations={}, retiring_probes={}, capacity_evicted_bricks={}, streaming={}, load_budget={}, eviction_budget={}, loaded_this_frame={}, evicted_this_frame={}, reclaimed_this_frame={}, allocation_stalls={}, sparse={}, resident_distance={}, resident_hysteresis={}, scheduler={}, update_budget={}, scheduled_bricks={}, scheduled_probes={}, deferred_bricks={}, trace_distance={}, trace_rays={}, history_hysteresis=({}, {}).",
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
            snapshot.published_brick_count,
            snapshot.pending_load_brick_count,
            snapshot.cached_brick_count,
            snapshot.retiring_brick_count,
            snapshot.physical_allocation_count,
            snapshot.allocated_physical_probe_count,
            snapshot.physical_allocator_capacity,
            snapshot.physical_probe_capacity,
            snapshot.free_physical_probe_count,
            snapshot.retiring_allocation_count,
            snapshot.retiring_probe_count,
            snapshot.capacity_evicted_brick_count,
            snapshot.streaming_enabled ? 1 : 0,
            snapshot.streaming_load_budget,
            snapshot.streaming_eviction_budget,
            snapshot.streaming_loaded_brick_count,
            snapshot.streaming_evicted_brick_count,
            snapshot.streaming_reclaimed_allocation_count,
            snapshot.streaming_allocation_stall_count,
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
    } else if (snapshot.debug_mode == 6u || snapshot.debug_mode == 11u ||
               snapshot.debug_mode == 12u || snapshot.debug_mode == 13u) {
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
