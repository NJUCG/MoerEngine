#pragma once

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
        uint             probe_count  = 0;
        ProbeUpdateParam param{};
    };

    struct UpdateInfo {
        bool                                                   enabled           = false;
        uint                                                   volume_count      = 0;
        uint                                                   total_probe_count = 0;
        StaticArray<UpdateJob, RASTER_PROBE_VOLUME_MAX_COUNT> jobs{};
    };

    void Create(RenderDevice& device, BindlessArrayRef& bdls);
    void Destroy(BindlessArrayRef& bdls);

    UpdateInfo PrepareUpdate(const RasterConfig& config, const Scene& scene, uint64 frame_index);
    void UpdateSceneData(CommandList& cmd_list, const Scene& scene);
    void FillLightingData(LightingData& lighting_data) const;

    BufferView GetProbeBufferView() const {
        return m_probe_buffer.buf->GetView();
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

    uint GetVolumeCount() const {
        return m_snapshot.volume_count;
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

private:
    struct VolumeSnapshot {
        uint   config_index = 0;
        uint   count_x      = 1;
        uint   count_y      = 1;
        uint   count_z      = 1;
        uint   total_count  = 1;
        uint   probe_offset = 0;
        float3 origin       = float3(0.0f);
        float3 extent       = float3(1.0f);
        float3 spacing      = float3(1.0f);
        float  intensity      = 0.0f;
        float  normal_bias    = 0.0f;
        float  blend_distance = 0.0f;
    };

    struct Snapshot {
        bool   enabled      = false;
        uint   debug_mode   = 0;
        uint   volume_count = 0;
        uint   total_count  = 0;
        float  trace_distance = 0.0f;
        uint   trace_ray_count = 1;
        float  visibility_bias = 0.0f;
        float  visibility_power = 1.0f;
        float  visibility_min_weight = 0.0f;
        float  visibility_strength = 0.0f;
        float  irradiance_hysteresis = 0.0f;
        float  visibility_hysteresis = 0.0f;
        float  debug_scale  = 1.0f;
        float  sky_intensity = 0.0f;
        float  directional_bounce = 0.0f;
        float3 sky_color    = float3(0.0f);
        float3 ground_color = float3(0.0f);
        StaticArray<VolumeSnapshot, RASTER_PROBE_VOLUME_MAX_COUNT> volumes{};
    };

    Snapshot BuildSnapshot(const RasterConfig& config) const;
    ProbeUpdateParam BuildUpdateParam(
        const Snapshot& snapshot,
        const VolumeSnapshot& volume,
        const Scene&    scene,
        uint64          frame_index,
        bool            history_valid
    ) const;
    bool HasSnapshotChanged(const Snapshot& snapshot) const;
    bool RequiresHistoryReset(const Snapshot& snapshot, uint volume_index) const;
    void StageVolumeUpload(const Snapshot& snapshot);

    BufferWithHandle m_probe_buffer;
    BufferWithHandle m_volume_buffer;
    BufferWithHandle m_visibility_atlas_buffer;
    BufferWithHandle m_irradiance_atlas_buffer;
    TextureWithHandle m_visibility_atlas_texture;
    TextureWithHandle m_irradiance_atlas_texture;
    BufferRef        m_scene_data_buffer;
    Array<byte>      m_scene_data_upload;
    Array<byte>      m_volume_data_upload;
    StaticArray<ProbeVolumeGpuDesc, RASTER_PROBE_VOLUME_MAX_COUNT> m_gpu_volume_descs{};
    Snapshot         m_snapshot;
    bool             m_has_snapshot = false;
    StaticArray<bool, RASTER_PROBE_VOLUME_MAX_COUNT> m_history_valid{};
};

} // namespace Moer::Render::Raster
