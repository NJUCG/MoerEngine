#ifndef VULKAN_RHI_DESCRIPTOR_H
#define VULKAN_RHI_DESCRIPTOR_H

#include "PixelFormat.h"
#include "VulkanPlatform.h"
#include "VulkanRHIResource.h"
#include "VulkanTypeDefs.h"
#include "rhi/RHIResource.h"
// #include "spirv_reflect.h"
#include "vulkan/vulkan_core.h"

#define VK_DESCRIPTOR_TYPE_BEGIN_RANGE (VK_DESCRIPTOR_TYPE_SAMPLER)
#define VK_DESCRIPTOR_TYPE_END_RANGE   (VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
#define VK_DESCRIPTOR_TYPE_RANGE_SIZE  3

#include <memory>

namespace Moer::Render {
class VulkanDevice;
struct VulkanShaderResourceState {
    uint         desc_type;
    uint8        resource_type; //SpvReflectResourceType
    uint8        b_sampled;
    EPixelFormat format;

    VulkanShaderResourceState() = default;
    VulkanShaderResourceState(uint _type, uint8 _resource_type, EPixelFormat _fmt) :
        desc_type(_type),
        resource_type(_resource_type),
        b_sampled(0),
        format(_fmt) {}

    VulkanShaderResourceState(uint64 _value) {
        memcpy(this, &_value, sizeof(VulkanShaderResourceState));
    }
    uint64 operator()() const {
        uint64 value;
        memcpy(&value, this, sizeof(VulkanShaderResourceState));
        return value;
    }
};

class VulkanDescriptorSetsLayout final : public VulkanDeviceObject {
public:
    VulkanDescriptorSetsLayout(
        VulkanDevice*                                        _device,
        const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings
    );

    ~VulkanDescriptorSetsLayout();

    inline uint32_t GetDescriptorSetCount() const {
        return m_layouts.size();
    }

    inline const Moer::Array<VkDescriptorSetLayout>& GetLayouts() const {
        return m_layouts;
    }

    inline const TDescriptorCountMap& GetDescriptorTypeCount() const {
        return m_descriptor_type_count;
    }

private:
    Moer::Array<VkDescriptorSetLayout> m_layouts;
    TDescriptorCountMap                m_descriptor_type_count;
};

#pragma region[ descriptor buffer ext ]

struct VulkanBindlessRingCacheEntry {
    uint64 version{0};
    uint64 array_offset{0};
    uint64 buffer_offset{0};
    uint64 texture_offset{0};
};

struct VulkanDescriptorBinderState;

class VulkanDescriptorBinder {
public:
    VulkanDescriptorBinder() = default;
    VulkanDescriptorBinder(const VulkanDescriptorBinder&) = delete;
    VulkanDescriptorBinder& operator=(const VulkanDescriptorBinder&) = delete;
    VulkanDescriptorBinder(VulkanDescriptorBinder&&) noexcept = default;
    VulkanDescriptorBinder& operator=(VulkanDescriptorBinder&&) noexcept = default;
    ~VulkanDescriptorBinder() = default;

    bool IsValid() const {
        return state != nullptr;
    }

    RENDER_API void ActivateOnCurrentThread() const;
    RENDER_API void DeactivateOnCurrentThread() const;

private:
    std::shared_ptr<VulkanDescriptorBinderState> state{};
    uint32                                       resource_heap_index{UINT32_MAX};
    uint64                                       base_offset{0};
    uint64                                       leased_size{0};
    uint64                                       used_size{0};

    friend class VulkanOnlineResourceDescriptorManager;
    friend struct VulkanDescriptorHeap;
};

class VulkanOnlineResourceDescriptorManager;
class VulkanOfflineDescriptorManager;
class VulkanHeapManager;

struct VulkanDescriptorHeap {
public:
    VulkanDescriptorHeap();
    VulkanDescriptorHeap(VulkanDevice& _device);
    ~VulkanDescriptorHeap();

    uint GetBufferDescIdx(
        const BufferView& _in_buffer,
        VkDescriptorType  _type,
        VkFormat          _format = VK_FORMAT_UNDEFINED
    );
    void FreeBufferDescIdx(uint _idx);
    uint GetImageDescIdx(
        const TextureView* _in_image,
        VkImageLayout      _layout,
        VkDescriptorType   _descriptor_type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
    );
    void FreeImageDescIdx(uint _idx);
    uint GetSamplerDescIdx(Sampler _sampler);
    uint GetAccelDescIdx(VulkanAccelerationStructure* _as);
    uint FreeAccelDescIdx(uint _idx);
    void CopyOfflineBufferDesc(uint64 _src_offset, void* _dst, uint64 _size) const;
    void CopyOfflineImageDesc(uint64 _src_offset, void* _dst, uint64 _size) const;
    void CopyOfflineSamplerDesc(uint64 _src_offset, void* _dst, uint64 _size) const;
    void CopyOfflineAccelDesc(uint64 _src_offset, void* _dst, uint64 _size) const;

public:
    RENDER_API VulkanDescriptorBinder BeginPushDescriptors();
    RENDER_API VulkanDescriptorBinder EndPushDescriptors(VulkanDescriptorBinder _binder);
    RENDER_API void RecycleOnlineDescriptorLease(VulkanDescriptorBinder _binder);
    RENDER_API uint64 AllocateOnlineDescriptorRange(uint64 _size);
    RENDER_API VulkanBindlessRingCacheEntry CacheBindlessArray(const VulkanBindlessArray& _array);

    // void PushBufferDesc(uint64 _src_offset, uint64 _set_offset);
    void PushUniformDesc(uint64 _src_offset, uint64 _set_offset);
    void PushStorageDesc(uint64 _src_offset, uint64 _set_offset);
    void PushImageDesc(uint64 _src_offset, uint64 _set_offset);
    void PushSamplerDesc(uint64 _src_offset, uint64 _set_offset);
    void PushAccelDesc(uint64 _src_offset, uint64 _set_offset);

public:
    VkBindHeapInfoEXT GetResourceHeapBindInfo() const;
    VkBindHeapInfoEXT GetSamplerHeapBindInfo() const;
    uint64 GetBindlessArrayDescriptorOffset() const;
    uint64 GetBindlessBufferDescriptorStride() const;
    uint64 GetBindlessImageDescriptorStride() const;
    uint64 GetBindlessSamplerDescriptorOffset(uint32 _sampler_idx) const;
    uint64 GetBindlessSamplerHeapBaseOffset() const;
    uint32 GetBindlessBufferHeapIndex(uint64 _src_offset) const;
    uint32 GetBindlessImageHeapIndex(uint64 _src_offset) const;
    uint64 GetPhysicalDescriptorSize(VkDescriptorType _type) const;
    uint64 GetOfflineSamplerDescriptorStride() const;
    uint64 GetOnlineResourceDescriptorStride(VkDescriptorType _type) const;
    uint64 GetOnlineResourceDescriptorAlignment(VkDescriptorType _type) const;

private:
    VulkanDevice*                                  m_device{nullptr};
    UniquePtr<VulkanHeapManager>                   m_heap_manager{};
    UniquePtr<VulkanOfflineDescriptorManager>      m_offline_manager{};
    UniquePtr<VulkanOnlineResourceDescriptorManager> m_online_manager{};
};

#pragma endregion
} // namespace Moer::Render

#endif
