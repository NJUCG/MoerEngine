#pragma once

#include "RasterConfig.h"
#include "misc/STL.h"
#include "misc/Traits.h"
#include "rhi/RHIResource.h"
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
    struct UpdateInfo {
        bool             enabled = false;
        uint             probe_count = 0;
        ProbeUpdateParam param{};
    };

    void Create(RenderDevice& device, BindlessArrayRef& bdls);
    void Destroy(BindlessArrayRef& bdls);

    UpdateInfo PrepareUpdate(const RasterConfig& config, const Scene& scene, uint64 frame_index);
    void FillLightingData(LightingData& lighting_data) const;

    BufferView GetProbeBufferView() const {
        return m_probe_buffer.buf->GetView();
    }

    BufferView GetVisibilityAtlasBufferView() const {
        return m_visibility_atlas_buffer.buf->GetView();
    }

    uint GetProbeCount() const {
        return m_snapshot.total_count;
    }

    uint GetBufferHandle() const {
        return m_probe_buffer.hdl;
    }

    uint GetVisibilityAtlasHandle() const {
        return m_visibility_atlas_buffer.hdl;
    }

private:
    struct Snapshot {
        bool   enabled      = false;
        uint   debug_mode   = 0;
        uint   count_x      = 1;
        uint   count_y      = 1;
        uint   count_z      = 1;
        uint   total_count  = 1;
        float3 origin       = float3(0.0f);
        float3 extent       = float3(1.0f);
        float3 spacing      = float3(1.0f);
        float  intensity    = 0.0f;
        float  normal_bias  = 0.0f;
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
    };

    Snapshot BuildSnapshot(const RasterConfig& config) const;
    ProbeUpdateParam BuildUpdateParam(
        const Snapshot& snapshot,
        const Scene&    scene,
        uint64          frame_index,
        bool            history_valid
    ) const;
    bool HasSnapshotChanged(const Snapshot& snapshot) const;
    bool RequiresHistoryReset(const Snapshot& snapshot) const;

    BufferWithHandle m_probe_buffer;
    BufferWithHandle m_visibility_atlas_buffer;
    Snapshot         m_snapshot;
    bool             m_has_snapshot = false;
    bool             m_history_valid = false;
};

} // namespace Moer::Render::Raster
