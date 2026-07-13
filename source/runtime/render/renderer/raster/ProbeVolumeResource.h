#pragma once

#include "ProbeAdaptiveLayout.h"
#include "ProbeClipmap.h"
#include "ProbeDirtyTracker.h"
#include "ProbeGeometryClassifier.h"
#include "ProbePhysicalAllocator.h"
#include "ProbeStreaming.h"
#include "RasterConfig.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

#include <atomic>

namespace Moer {
class Scene;
}

namespace Moer::Render {
class CommandList;
class RenderDevice;
}

namespace Moer::Render::Raster {

class ProbeVolumeResource {
public:
    struct UpdateJob {
        uint             volume_index = 0;
        uint             brick_index  = 0;
        uint             probe_count  = 0;
        ProbeUpdateParam param{};
    };

    struct UpdateInfo {
        bool                                                enabled               = false;
        uint                                                volume_count          = 0;
        uint                                                job_count             = 0;
        uint                                                resident_brick_count  = 0;
        uint                                                resident_probe_count  = 0;
        uint                                                scheduled_probe_count = 0;
        uint                                                deferred_brick_count  = 0;
        uint                                                dirty_brick_count     = 0;
        uint                                                scheduled_dirty_brick_count = 0;
        uint                                                deferred_dirty_brick_count  = 0;
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> scheduled_level_brick_count{};
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> scheduled_dirty_level_brick_count{};
        StaticArray<UpdateJob, RASTER_PROBE_MAX_BRICK_COUNT> jobs{};
    };

    void Create(RenderDevice& device, BindlessArrayRef& bdls);
    void Destroy(BindlessArrayRef& bdls);

    UpdateInfo PrepareUpdate(
        const RasterConfig& config,
        const Scene&        scene,
        float3              camera_position,
        uint64              frame_index
    );
    void UpdateSceneData(CommandList& cmd_list, const Scene& scene);
    void TrackFrameSubmission(CommandList& cmd_list, uint64 frame_index);
    void FillLightingData(LightingData& lighting_data) const;

    BufferView GetProbeBufferView() const {
        return m_probe_buffer.buf->GetView();
    }

    BufferView GetVolumeBufferView() const {
        return m_volume_buffer.buf->GetView();
    }

    BufferView GetBrickBufferView() const {
        return m_brick_buffer.buf->GetView();
    }

    BufferView GetCellBufferView() const {
        return m_cell_buffer.buf->GetView();
    }

    BufferView GetPageTableBufferView() const {
        return m_page_table_buffer.buf->GetView();
    }

    BufferView GetVisibilityAtlasBufferView() const {
        return m_visibility_atlas_buffer.buf->GetView();
    }

    BufferView GetIrradianceAtlasBufferView() const {
        return m_irradiance_atlas_buffer.buf->GetView();
    }

    TextureView GetVisibilityAtlasTextureView() const {
        return m_visibility_atlas_texture.tex->GetView();
    }

    TextureView GetIrradianceAtlasTextureView() const {
        return m_irradiance_atlas_texture.tex->GetView();
    }

    BufferView GetSceneDataBufferView() const {
        return m_scene_data_buffer->GetView();
    }

    uint GetProbeCount() const {
        return m_snapshot.total_count;
    }

    uint GetHierarchyProbeCount() const {
        return m_snapshot.hierarchy_probe_count;
    }

    uint GetVolumeCount() const {
        return m_snapshot.volume_count;
    }

    uint GetBrickCount() const {
        return m_snapshot.brick_count;
    }

    uint GetCellCount() const {
        return m_snapshot.cell_count;
    }

    uint GetResidentBrickCount() const {
        return m_snapshot.resident_brick_count;
    }

    uint GetResidentProbeCount() const {
        return m_snapshot.resident_probe_count;
    }

    uint GetPublishedBrickCount() const {
        return m_snapshot.published_brick_count;
    }

    uint GetPendingLoadBrickCount() const {
        return m_snapshot.pending_load_brick_count;
    }

    uint GetCachedBrickCount() const {
        return m_snapshot.cached_brick_count;
    }

    uint GetRetiringAllocationCount() const {
        return m_snapshot.retiring_allocation_count;
    }

    uint GetRetiringProbeCount() const {
        return m_snapshot.retiring_probe_count;
    }

    uint GetStreamingLoadedBrickCount() const {
        return m_snapshot.streaming_loaded_brick_count;
    }

    uint GetStreamingEvictedBrickCount() const {
        return m_snapshot.streaming_evicted_brick_count;
    }

    uint GetStreamingReclaimedAllocationCount() const {
        return m_snapshot.streaming_reclaimed_allocation_count;
    }

    uint GetStreamingAllocationStallCount() const {
        return m_snapshot.streaming_allocation_stall_count;
    }

    uint GetRequestedBrickCount() const {
        return m_snapshot.requested_brick_count;
    }

    uint GetRequestedProbeCount() const {
        return m_snapshot.requested_probe_count;
    }

    uint GetPhysicalProbeCapacity() const {
        return m_snapshot.physical_probe_capacity;
    }

    uint GetPhysicalAllocatorCapacity() const {
        return m_snapshot.physical_allocator_capacity;
    }

    uint GetAllocatedPhysicalProbeCount() const {
        return m_snapshot.allocated_physical_probe_count;
    }

    uint GetPhysicalAllocationCount() const {
        return m_snapshot.physical_allocation_count;
    }

    uint GetFreePhysicalProbeCount() const {
        return m_snapshot.free_physical_probe_count;
    }

    uint GetCapacityEvictedBrickCount() const {
        return m_snapshot.capacity_evicted_brick_count;
    }

    uint GetClipmapVolumeCount() const {
        return m_snapshot.clipmap_volume_count;
    }

    uint GetClipmapScrolledVolumeCount() const {
        return m_snapshot.clipmap_scrolled_volume_count;
    }

    uint GetPrefetchedBrickCount() const {
        return m_snapshot.prefetched_brick_count;
    }

    uint GetClipmapReusedBrickCount() const {
        return m_snapshot.clipmap_reused_brick_count;
    }

    uint GetScheduledBrickCount() const {
        return m_last_scheduled_brick_count;
    }

    uint GetScheduledProbeCount() const {
        return m_last_scheduled_probe_count;
    }

    uint GetDirtyBrickCount() const {
        return m_last_dirty_brick_count;
    }

    uint GetScheduledDirtyBrickCount() const {
        return m_last_scheduled_dirty_brick_count;
    }

    uint GetDeferredDirtyBrickCount() const {
        return m_last_deferred_dirty_brick_count;
    }

    uint GetDirtyRegionCount() const {
        return m_last_dirty_region_count;
    }

    uint GetGlobalDirtyReasons() const {
        return m_last_global_dirty_reasons;
    }

    const ProbeBrickGpuDesc& GetBrickDesc(uint brick_index) const {
        return m_gpu_brick_descs[brick_index];
    }

    const ProbeCellGpuDesc& GetCellDesc(uint cell_index) const {
        return m_gpu_cell_descs[cell_index];
    }

    const ProbeVolumeGpuDesc& GetVolumeDesc(uint volume_index) const {
        return m_gpu_volume_descs[volume_index];
    }

    uint GetBufferHandle() const {
        return m_probe_buffer.hdl;
    }

    uint GetVisibilityAtlasHandle() const {
        return m_visibility_atlas_buffer.hdl;
    }

    uint GetIrradianceAtlasHandle() const {
        return m_irradiance_atlas_buffer.hdl;
    }

    uint GetVisibilityAtlasTextureHandle() const {
        return m_visibility_atlas_texture.hdl;
    }

    uint GetIrradianceAtlasTextureHandle() const {
        return m_irradiance_atlas_texture.hdl;
    }

    uint GetVolumeBufferHandle() const {
        return m_volume_buffer.hdl;
    }

    uint GetBrickBufferHandle() const {
        return m_brick_buffer.hdl;
    }

    uint GetCellBufferHandle() const {
        return m_cell_buffer.hdl;
    }

    uint GetPageTableBufferHandle() const {
        return m_page_table_buffer.hdl;
    }

private:
    struct BrickSnapshot {
        uint3 coord              = uint3(0u);
        int3  world_fine_coord   = int3(0);
        uint3 local_counts       = uint3(1u);
        uint  volume_index       = 0;
        uint  probe_offset       = RASTER_PROBE_PAGE_INVALID;
        uint  probe_count        = 0;
        uint  page_index         = 0;
        uint  cell_index         = RASTER_PROBE_PAGE_INVALID;
        uint  subdivision_level  = 0;
        uint  parent_page_index  = RASTER_PROBE_PAGE_INVALID;
        uint4 neighbor_pages_0   = uint4(RASTER_PROBE_PAGE_INVALID);
        uint4 neighbor_pages_1   = uint4(RASTER_PROBE_PAGE_INVALID);
        Box3D sample_bounds;
        bool  placement_requested = true;
        bool  requested_resident = true;
        bool  resident           = false;
        uint  update_age         = 0;
        uint  dirty_flags        = 0u;
        uint  page_generation    = 1u;
        uint  streaming_state    = RASTER_PROBE_STREAMING_UNMAPPED;
        bool  cached             = false;
        bool  prefetched         = false;
        bool  clipmap_reused     = false;
        uint  previous_brick_index = RASTER_PROBE_PAGE_INVALID;
        uint64 last_requested_frame = 0u;
        uint64 last_reused_frame = 0u;
        float camera_distance_sq = 0.0f;
    };

    struct VolumeSnapshot {
        uint   config_index         = 0;
        uint   count_x              = 1;
        uint   count_y              = 1;
        uint   count_z              = 1;
        uint   total_count          = 1;
        uint   hierarchy_probe_count = 0;
        uint   probe_offset         = 0;
        uint   page_table_offset    = 0;
        uint   brick_count          = 0;
        uint   first_cell_index     = 0;
        uint   cell_count           = 0;
        uint   max_subdivision_level = 0;
        uint   previous_volume_index = RASTER_PROBE_PAGE_INVALID;
        uint   requested_brick_count = 0;
        uint   requested_probe_count = 0;
        uint   resident_brick_count = 0;
        uint   resident_probe_count = 0;
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_brick_count{};
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_requested_brick_count{};
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_resident_brick_count{};
        bool   camera_clipmap        = false;
        bool   clipmap_follow_y      = false;
        bool   clipmap_scrolled      = false;
        uint   prefetched_brick_count = 0;
        uint   clipmap_reused_brick_count = 0;
        int3   clipmap_anchor_cell   = int3(0);
        float3 configured_origin     = float3(0.0f);
        float3 clipmap_cell_step     = float3(1.0f);
        float3 origin               = float3(0.0f);
        float3 extent               = float3(1.0f);
        float3 spacing              = float3(1.0f);
        float  intensity            = 0.0f;
        float  normal_bias          = 0.0f;
        float  blend_distance       = 0.0f;
    };

    struct CellSnapshot {
        uint3  coord                  = uint3(0u);
        uint   volume_index           = 0;
        uint   config_index           = 0;
        uint   first_brick_index      = 0;
        uint   brick_count            = 0;
        uint   requested_brick_count  = 0;
        uint   resident_brick_count   = 0;
        uint   min_subdivision_level  = 0;
        uint   max_subdivision_level  = 0;
        uint   geometry_primitive_count = 0;
        uint   occupied_voxel_count     = 0;
        uint   desired_subdivision_level = RASTER_PROBE_MAX_SUBDIVISION_LEVEL;
        uint   geometry_generation       = 0;
        float3 origin                 = float3(0.0f);
        float3 extent                 = float3(0.0f);
        float  min_probe_spacing      = 0.0f;
        float  geometry_occupancy     = 0.0f;
    };

    struct Snapshot {
        bool   enabled                    = false;
        bool   sparse_bricks_enabled      = false;
        bool   adaptive_placement_enabled = false;
        bool   hierarchy_enabled          = false;
        bool   dirty_tracking_enabled     = false;
        uint   debug_mode                 = 0;
        uint   volume_count               = 0;
        uint   cell_count                 = 0;
        uint   brick_count                = 0;
        uint   max_subdivision_level      = 0;
        uint   layout_generation          = 0;
        uint   requested_brick_count      = 0;
        uint   requested_probe_count      = 0;
        uint   resident_brick_count       = 0;
        uint   resident_probe_count       = 0;
        uint   published_brick_count      = 0;
        uint   pending_load_brick_count   = 0;
        uint   cached_brick_count         = 0;
        uint   retiring_brick_count       = 0;
        uint   total_count                = 0;
        uint   hierarchy_probe_count      = 0;
        uint   physical_probe_capacity    = RASTER_PROBE_MAX_COUNT;
        uint   physical_allocator_capacity = RASTER_PROBE_MAX_COUNT;
        uint   allocated_physical_probe_count = 0;
        uint   physical_allocation_count  = 0;
        uint   free_physical_probe_count  = RASTER_PROBE_MAX_COUNT;
        uint   retiring_allocation_count  = 0;
        uint   retiring_probe_count       = 0;
        uint   streaming_loaded_brick_count = 0;
        uint   streaming_evicted_brick_count = 0;
        uint   streaming_reclaimed_allocation_count = 0;
        uint   streaming_allocation_stall_count = 0;
        uint   capacity_evicted_brick_count = 0;
        uint   clipmap_volume_count       = 0;
        uint   clipmap_scrolled_volume_count = 0;
        uint   prefetched_brick_count     = 0;
        uint   clipmap_reused_brick_count = 0;
        uint   geometry_generation        = 0;
        uint   geometry_primitive_count   = 0;
        uint   fine_cell_count            = 0;
        uint   medium_cell_count          = 0;
        uint   coarse_cell_count          = 0;
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_brick_count{};
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_requested_brick_count{};
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_resident_brick_count{};
        bool   allocator_compacted        = false;
        float  adaptive_geometry_padding  = 0.0f;
        float  adaptive_fine_occupancy    = 0.25f;
        uint   adaptive_fine_primitives   = 64u;
        float  adaptive_transition_width  = 1.5f;
        float  dirty_influence_scale      = 1.0f;
        float  brick_resident_distance    = 0.0f;
        float  brick_resident_hysteresis  = 0.0f;
        float  clipmap_anchor_hysteresis  = 0.0f;
        bool   motion_prefetch_enabled    = false;
        float  motion_prefetch_threshold  = 0.0f;
        uint   motion_prefetch_keep_frames = 1u;
        bool   streaming_enabled          = true;
        uint   streaming_load_budget      = 1u;
        uint   streaming_eviction_budget  = 1u;
        bool   update_scheduler_enabled   = false;
        uint   update_brick_budget        = 1;
        float  trace_distance             = 0.0f;
        uint   trace_ray_count            = 1;
        float  visibility_bias            = 0.0f;
        float  visibility_power           = 1.0f;
        float  visibility_min_weight      = 0.0f;
        float  visibility_strength        = 0.0f;
        float  irradiance_hysteresis      = 0.0f;
        float  visibility_hysteresis      = 0.0f;
        float  debug_scale                = 1.0f;
        float  sky_intensity              = 0.0f;
        float3 sky_color                  = float3(0.0f);
        float3 ground_color               = float3(0.0f);
        float3 camera_position            = float3(0.0f);
        float3 camera_motion              = float3(0.0f);
        uint64 frame_index                = 0u;
        StaticArray<VolumeSnapshot, RASTER_PROBE_VOLUME_MAX_COUNT> volumes{};
        StaticArray<CellSnapshot, RASTER_PROBE_MAX_CELL_COUNT> cells{};
        StaticArray<BrickSnapshot, RASTER_PROBE_MAX_BRICK_COUNT> bricks{};
        StaticArray<uint, RASTER_PROBE_MAX_PAGE_COUNT> page_table{};
    };

    Snapshot BuildSnapshot(const RasterConfig& config, float3 camera_position, uint64 frame_index) const;
    void RefreshSceneGeometry(const Scene& scene, bool dirty_tracking_enabled);
    void RefreshGlobalDirtyEvents(const Scene& scene, bool dirty_tracking_enabled);
    void ApplyDirtyEvents(Snapshot& snapshot);
    ProbeUpdateParam BuildUpdateParam(
        const Snapshot& snapshot,
        const VolumeSnapshot& volume,
        uint            volume_index,
        uint            brick_index,
        const Scene&    scene,
        bool            history_valid
    ) const;
    bool RequiresPhysicalAllocatorReset(const Snapshot& snapshot) const;
    void ApplyPhysicalResidency(
        Snapshot& snapshot,
        const StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT>& volume_history_reset
    );
    void RebindPhysicalAllocations(Snapshot& snapshot, bool allow_reuse);
    void ResetPhysicalAllocator(uint physical_probe_capacity);
    void RetirePhysicalAllocation(uint brick_index, uint virtual_page);
    void CollectRetiredAllocations(Snapshot& snapshot);
    uint AdvancePageGeneration(uint virtual_page);
    bool HasSnapshotChanged(const Snapshot& snapshot) const;
    bool RequiresHistoryReset(const Snapshot& snapshot, uint volume_index) const;
    void StageVolumeUpload(const Snapshot& snapshot);
    void StageCellUpload(const Snapshot& snapshot);
    void StageBrickUpload(const Snapshot& snapshot);

    BufferWithHandle m_probe_buffer;
    BufferWithHandle m_volume_buffer;
    BufferWithHandle m_cell_buffer;
    BufferWithHandle m_brick_buffer;
    BufferWithHandle m_page_table_buffer;
    BufferWithHandle m_visibility_atlas_buffer;
    BufferWithHandle m_irradiance_atlas_buffer;
    TextureWithHandle m_visibility_atlas_texture;
    TextureWithHandle m_irradiance_atlas_texture;
    BufferRef        m_scene_data_buffer;
    Array<byte>      m_scene_data_upload;
    Array<byte>      m_volume_data_upload;
    Array<byte>      m_cell_data_upload;
    Array<byte>      m_brick_data_upload;
    Array<byte>      m_page_table_upload;
    StaticArray<ProbeVolumeGpuDesc, RASTER_PROBE_VOLUME_MAX_COUNT> m_gpu_volume_descs{};
    StaticArray<ProbeCellGpuDesc, RASTER_PROBE_MAX_CELL_COUNT> m_gpu_cell_descs{};
    StaticArray<ProbeBrickGpuDesc, RASTER_PROBE_MAX_BRICK_COUNT> m_gpu_brick_descs{};
    Snapshot         m_snapshot;
    bool             m_has_snapshot = false;
    StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT> m_history_valid{};
    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT> m_brick_history_valid{};
    StaticArray<uint64, RASTER_PROBE_MAX_BRICK_COUNT> m_brick_last_update_frame{};
    StaticArray<ProbePhysicalAllocator::Allocation, RASTER_PROBE_MAX_BRICK_COUNT> m_physical_allocations{};
    StaticArray<uint, RASTER_PROBE_MAX_BRICK_COUNT> m_physical_allocation_pages{};
    StaticArray<uint, RASTER_PROBE_MAX_BRICK_COUNT> m_physical_allocation_generations{};
    StaticArray<uint64, RASTER_PROBE_MAX_BRICK_COUNT> m_brick_load_submission{};
    StaticArray<bool, RASTER_PROBE_MAX_BRICK_COUNT> m_brick_load_awaiting_submission{};
    StaticArray<uint, RASTER_PROBE_MAX_PAGE_COUNT> m_page_generations{};
    ProbePhysicalAllocator m_physical_allocator;
    ProbeRetirementQueue   m_retirement_queue;
    struct GpuCompletionState {
        std::atomic<uint64> completed_submission{0u};
    };
    SharedPtr<GpuCompletionState> m_gpu_completion_state;
    uint64           m_last_submitted_submission = 0u;
    uint             m_pending_physical_capacity = 0u;
    Array<Box3D>     m_scene_geometry_bounds;
    Array<ProbeTrackedBounds> m_scene_geometry_instances;
    Array<ProbeDirtyRegion>   m_frame_dirty_regions;
    StaticArray<uint, RASTER_PROBE_MAX_BRICK_COUNT> m_brick_pending_dirty_reasons{};
    struct MainLightSnapshot {
        bool   exists    = false;
        float3 direction = float3(0.0f, -1.0f, 0.0f);
        float3 color     = float3(1.0f);
        float  intensity = 0.0f;
    };
    MainLightSnapshot m_main_light_snapshot;
    bool             m_main_light_snapshot_valid = false;
    uint             m_frame_global_dirty_reasons = 0u;
    uint             m_frame_changed_geometry_count = 0u;
    bool             m_frame_dirty_regions_collapsed = false;
    bool             m_scene_geometry_cache_valid = false;
    uint             m_scene_geometry_generation  = 0;
    uint             m_layout_generation         = 0;
    uint             m_last_scheduled_brick_count = 0;
    uint             m_last_scheduled_probe_count = 0;
    uint             m_last_dirty_brick_count = 0;
    uint             m_last_scheduled_dirty_brick_count = 0;
    uint             m_last_deferred_dirty_brick_count = 0;
    uint             m_last_dirty_region_count = 0;
    uint             m_last_global_dirty_reasons = 0u;
};

} // namespace Moer::Render::Raster
