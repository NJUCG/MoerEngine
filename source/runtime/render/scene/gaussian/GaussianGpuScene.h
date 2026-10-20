#pragma once

#include "GaussianAsset.h"
#include "rhi/RHIResource.h"

namespace Moer::Render {
class CommandList;

struct GaussianGpuUpload {
    std::vector<GGaussian>         gaussians;
    std::vector<GGaussianInstance> instances;
    std::vector<GaussianAssetRef>  assets;
};

// CPU packing is independent of the device and can be checked without a GPU.
RENDER_API GaussianGpuUpload
BuildGaussianGpuUpload(const GaussianSceneSnapshot& snapshot, bool include_geometry = true);

class RENDER_API GaussianGpuScene {
public:
    void ApplyUpdate(std::shared_ptr<const GaussianSceneSnapshot> snapshot, CommandList& commands);
    const BufferRef& GetGaussianBuffer() const {
        return m_gaussians;
    }
    const BufferRef& GetInstanceBuffer() const {
        return m_instances;
    }
    uint32 GetGaussianCount() const {
        return m_gaussian_count;
    }
    uint32 GetInstanceCount() const {
        return m_instance_count;
    }
    uint64 GetRevision() const {
        return m_revision;
    }

private:
    BufferRef                     m_gaussians;
    BufferRef                     m_instances;
    std::vector<GaussianAssetRef> m_assets;
    uint32                        m_gaussian_count = 0;
    uint32                        m_instance_count = 0;
    uint64                        m_revision       = 0;
};
} // namespace Moer::Render
