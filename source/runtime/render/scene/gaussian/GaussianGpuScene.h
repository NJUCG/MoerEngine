#pragma once

#include "GaussianAsset.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {
class CommandList;

struct GaussianGpuUpload {
    std::vector<GGaussian>         gaussians;
    std::vector<GGaussianInstance> instances;
    std::vector<GaussianAssetRef>  assets;
    std::vector<Box3D> bounds;
};

// CPU packing is independent of the device and can be checked without a GPU.
RENDER_API GaussianGpuUpload
BuildGaussianGpuUpload(const GaussianSceneSnapshot& snapshot, bool should_include_geometry = true);

class RENDER_API GaussianGpuScene {
public:
    void ApplyUpdate(std::shared_ptr<const GaussianSceneSnapshot> snapshot, CommandList& commands);
    const BufferRef& GetGaussianBuffer() const {
        return m_gaussian_buffer;
    }
    const BufferRef& GetInstanceBuffer() const {
        return m_instance_buffer;
    }
    uint32 GetGaussianCount() const {
        return m_gaussian_count;
    }
    uint32 GetInstanceCount() const {
        return m_instance_count;
    }
    const std::vector<GGaussianInstance>& GetInstances() const { return m_instances; }
    const std::vector<Box3D>& GetBounds() const { return m_bounds; }
    uint64 GetRevision() const {
        return m_revision;
    }

private:
    std::vector<GGaussianInstance> m_instances;
    std::vector<Box3D> m_bounds;
    BufferRef                     m_gaussian_buffer;
    BufferRef                     m_instance_buffer;
    std::vector<GaussianAssetRef> m_assets;
    uint32                        m_gaussian_count = 0;
    uint32                        m_instance_count = 0;
    uint64                        m_revision       = 0;
};
} // namespace Moer::Render
