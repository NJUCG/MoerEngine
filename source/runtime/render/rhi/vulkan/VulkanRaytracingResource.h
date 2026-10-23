#pragma once

#include "VulkanDeviceObject.h"
#include "VulkanRHIResource.h"
#include "misc/Alignment.h"
#include "rhi/RHICommand.h"

namespace Moer::Render {

class VulkanRaytracingScene;
struct VulkanAccelerationStructure : public RaytracingTlas {
    VulkanAccelerationStructure(VulkanDevice& _device, VulkanRaytracingScene& _src_scene);
    virtual ~VulkanAccelerationStructure() override;
    void    Destroy() override;
    Buffer* GetUnderlyingBuffer() const override {
        return underlying_buffer;
    }

    VkAccelerationStructureKHR handle            = VK_NULL_HANDLE;
    VulkanBufferRef            underlying_buffer = nullptr;
    VulkanDevice&              device;
    VulkanRaytracingScene&     src_scene;

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
    uint64 blas_address;

private:
    VkAccelerationStructureKHR acc;
    VulkanBufferRef            underlying_buffer;
};

class VulkanRaytracingScene final : public RaytracingScene, public VulkanDeviceObject {
public:
    VulkanRaytracingScene(VulkanDevice* _device);
    virtual ~VulkanRaytracingScene();
    RaytracingInstance& AddInstance() override;

    void MarkModified(uint _array_idx) override;
    void FreeInstance(uint _array_idx) override;

    void RegisterGeometry(RaytracingGeometryRef _geometry) override;
    void UnregisterGeometry(RaytracingGeometryRef _geometry) override;

    RaytracingTlasRef GetTlas() const override;
    RaytracingTlasRef GetPrevTlas() const override;

    UniquePtr<Command> UpdateScene() override;
    void               AdvanceFrame() override;

public:
    void                RefitInstanceBuffer();
    bool                RefitTLASAndScratchBuffer();
    RaytracingSizeInfos CalculateSizeInfos();
    // temperory
    Array<uint> temp_update_instance_ids;
    Set<uint>   temp_modified_instance_ids;

    Array<byte> temp_update_instances;

    Set<uint> prev_modified_instance_ids;

public:
    RaytracingSizeInfos size_infos{};
    RaytracingSizeInfos prev_size_infos{};

private:
    VulkanAccelRef  tlas           = nullptr;
    VulkanAccelRef  prev_tlas      = nullptr;
    VulkanBufferRef scratch_buffer = nullptr;

    VulkanBufferRef instance_buffer = nullptr;

    // TLAS instance buffer device address must be 16-byte aligned (Vulkan spec)
    // Store the aligned address to avoid recomputing
    uint64 aligned_instance_buffer_address = 0;

public:
    // Get 16-byte aligned device address for TLAS instance data
    uint64 GetAlignedInstanceBufferAddress() const {
        constexpr uint64 kAlignment = 256; // Use 256-byte alignment for AMD GPU compatibility
        if (aligned_instance_buffer_address != 0) {
            return aligned_instance_buffer_address;
        }
        if (!instance_buffer)
            return 0;
        uint64 addr = instance_buffer->DeviceAddress();
        return Moer::AlignUp(addr, kAlignment);
    }

    void UpdateAlignedInstanceBufferAddress() {
        constexpr uint64 kAlignment = 256;
        if (instance_buffer) {
            aligned_instance_buffer_address = Moer::AlignUp(instance_buffer->DeviceAddress(), kAlignment);
        }
    }

    Array<VkAccelerationStructureInstanceKHR> vk_instances;

    Array<uint> free_instance_slots;

    std::mutex                 geom_mutex;
    UnorderedMap<uint64, uint> related_geometries;
    bool                       b_current_full_refit = false;

private:
    uint instance_capacity      = 1000;
    uint prev_instance_capacity = 1000;
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
