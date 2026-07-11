#pragma once

#include "ProbeAdaptiveLayout.h"
#include "ProbeGeometryClassifier.h"
#include "ProbePhysicalAllocator.h"
#include "RasterConfig.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"
#include "shaderheaders/shared/ShaderParameters.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"

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

    uint GetRequestedBrickCount() const {
        return m_snapshot.requested_brick_count;
    }

    uint GetRequestedProbeCount() const {
        return m_snapshot.requested_probe_count;
    }

    uint GetPhysicalProbeCapacity() const {
        return m_snapshot.physical_probe_capacity;
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

    uint GetScheduledBrickCount() const {
        return m_last_scheduled_brick_count;
    }

    uint GetScheduledProbeCount() const {
        return m_last_scheduled_probe_count;
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
        bool  requested_resident = true;
        bool  resident           = false;
        uint  update_age         = 0;
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
        uint   requested_brick_count = 0;
        uint   requested_probe_count = 0;
        uint   resident_brick_count = 0;
        uint   resident_probe_count = 0;
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_brick_count{};
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_requested_brick_count{};
        StaticArray<uint, RASTER_PROBE_MAX_SUBDIVISION_LEVEL + 1u> level_resident_brick_count{};
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
        uint   total_count                = 0;
        uint   hierarchy_probe_count      = 0;
        uint   physical_probe_capacity    = RASTER_PROBE_MAX_COUNT;
        uint   allocated_physical_probe_count = 0;
        uint   physical_allocation_count  = 0;
        uint   free_physical_probe_count  = RASTER_PROBE_MAX_COUNT;
        uint   capacity_evicted_brick_count = 0;
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
        float  brick_resident_distance    = 0.0f;
        float  brick_resident_hysteresis  = 0.0f;
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
        float  directional_bounce         = 0.0f;
        float3 sky_color                  = float3(0.0f);
        float3 ground_color               = float3(0.0f);
        StaticArray<VolumeSnapshot, RASTER_PROBE_VOLUME_MAX_COUNT> volumes{};
        StaticArray<CellSnapshot, RASTER_PROBE_MAX_CELL_COUNT> cells{};
        StaticArray<BrickSnapshot, RASTER_PROBE_MAX_BRICK_COUNT> bricks{};
        StaticArray<uint, RASTER_PROBE_MAX_PAGE_COUNT> page_table{};
    };

    Snapshot BuildSnapshot(const RasterConfig& config, float3 camera_position, uint64 frame_index) const;
    void RefreshSceneGeometry(const Scene& scene);
    ProbeUpdateParam BuildUpdateParam(
        const Snapshot& snapshot,
        const VolumeSnapshot& volume,
        uint            volume_index,
        uint            brick_index,
        const Scene&    scene,
        bool            history_valid
    ) const;
    bool RequiresPhysicalAllocatorReset(const Snapshot& snapshot) const;
    void ApplyPhysicalResidency(Snapshot& snapshot);
    void ResetPhysicalAllocator(uint physical_probe_capacity);
    void ReleasePhysicalAllocation(uint brick_index);
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
    ProbePhysicalAllocator m_physical_allocator;
    Array<Box3D>     m_scene_geometry_bounds;
    bool             m_scene_geometry_cache_valid = false;
    uint             m_scene_geometry_generation  = 0;
    uint             m_layout_generation         = 0;
    uint             m_last_scheduled_brick_count = 0;
    uint             m_last_scheduled_probe_count = 0;
};

} // namespace Moer::Render::Raster
