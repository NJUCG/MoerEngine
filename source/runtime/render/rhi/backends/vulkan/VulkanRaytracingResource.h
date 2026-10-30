#pragma once

#include "VulkanDeviceObject.h"
#include "VulkanRHIResource.h"
#include "misc/Types.h"
#include "rhi/RHICommand.h"

namespace Moer::Render {

struct VulkanAccelerationStructure : public RaytracingTlas {
    explicit VulkanAccelerationStructure(VulkanDevice& device);
    virtual ~VulkanAccelerationStructure() override;
    void    Destroy() override;
    Buffer* GetUnderlyingBuffer() const override {
        return underlying_buffer;
    }

    VkAccelerationStructureKHR handle            = VK_NULL_HANDLE;
    VulkanBufferRef            underlying_buffer = nullptr;
    VulkanDevice&              device;

private:
    friend class VulkanDescriptorHeap;
    int m_descriptor_cache_slot = -1;
};

using VulkanAccelRef = CountableRef<VulkanAccelerationStructure>;
class VulkanRaytracingGeometry final : public RaytracingGeometry, public VulkanDeviceObject {
public:
    VulkanRaytracingGeometry(const RaytracingGeometryInfo& _info, VulkanDevice* _device);
    virtual ~VulkanRaytracingGeometry();

    inline VkAccelerationStructureKHR GetHandle() const {
        return acc;
    }

    inline VulkanBuffer* GetUnderlyingBuffer() const override {
        return underlying_buffer;
    }
    Array<VkAccelerationStructureGeometryKHR>       build_geometries;
    Array<VkAccelerationStructureBuildRangeInfoKHR> build_ranges;
    VkAccelerationStructureBuildSizesInfoKHR        build_sizes_info{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR
    };
    uint64 blas_address = 0;

private:
    VkAccelerationStructureKHR acc = VK_NULL_HANDLE;
    VulkanBufferRef            underlying_buffer;
};

class VulkanRaytracingScene final : public RaytracingScene, public VulkanDeviceObject {
public:
    VulkanRaytracingScene(VulkanDevice* _device);
    ~VulkanRaytracingScene() override = default;
    RaytracingInstance& AddInstance() override;

    void MarkModified(uint32 _array_idx) override;
    void FreeInstance(uint32 _array_idx) override;

    RaytracingTlasRef GetTlas() const override;
    RaytracingTlasRef GetPrevTlas() const override;

    UniquePtr<Command> UpdateScene() override;
    void               AdvanceFrame() override;

private:
    VulkanBufferRef     CreateInstanceBuffer(uint32 capacity);
    void GrowInstanceBuffer();
    bool EnsureAccelerationStructureStorage();
    RaytracingSizeInfos CalculateSizeInfos();

    // Persistent scene slots and pending edits, consumed by UpdateScene.
    Array<uint32> free_instance_slots;
    Array<uint32> pending_instance_indices;
    Set<uint32>   modified_instance_indices;
    Set<uint32>   previous_modified_instance_indices;

    // Current/previous TLAS storage follows the existing AdvanceFrame boundary.
    VulkanAccelRef tlas;
    VulkanAccelRef prev_tlas;
    VulkanBufferRef scratch_buffer;
    VulkanBufferRef instance_buffer;
    RaytracingSizeInfos size_infos{};
    RaytracingSizeInfos prev_size_infos{};
    uint32              instance_capacity      = 1000;
    uint32              prev_instance_capacity = 1000;
};

inline VulkanRaytracingGeometry* ResourceCast(RaytracingGeometry* resource) {
    return static_cast<VulkanRaytracingGeometry*>(resource);
}

inline VulkanRaytracingScene* ResourceCast(RaytracingScene* resource) {
    return static_cast<VulkanRaytracingScene*>(resource);
}

inline VulkanAccelerationStructure* ResourceCast(RaytracingTlas* resource) {
    return static_cast<VulkanAccelerationStructure*>(resource);
}

} // namespace Moer::Render
