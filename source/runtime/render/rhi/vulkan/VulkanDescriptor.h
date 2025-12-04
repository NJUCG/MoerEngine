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

#include <mutex>

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

struct VulkanDescritporSetLayout {
    VkDescriptorSetLayout layout;
    uint                  size;
    uint                  offset;
};
struct BufferCpuDescHandle {
    uint  data[4];
    void* Data() {
        return data;
    }
    const void* Data() const {
        return data;
    }
};
struct ImageCpuDescHandle {
    uint data[4];
};
struct VulkanDescriptorHeap {
    VulkanBuffer* buffer_desc_buffer = nullptr;
    VulkanBuffer* image_desc_buffer  = nullptr;

public:
    VulkanDescriptorHeap() = default;
    VulkanDescriptorHeap(VulkanDevice& _device);
    ~VulkanDescriptorHeap();

    Array<byte> buffer_desc_data;
    Array<byte> image_desc_data;
    Array<byte> accel_desc_data;

    Array<uint> buffer_free_list;
    Array<uint> image_free_list;
    Array<uint> accel_free_list;

    uint64 buffer_offset;
    uint64 image_offset;
    uint64 accel_offset;

    uint GetBufferDescIdx(
        const BufferView& _in_buffer,
        VkDescriptorType  _type,
        VkFormat          _format = VK_FORMAT_UNDEFINED
    );
    void FreeBufferDescIdx(uint _idx);
    uint GetImageDescIdx(const TextureView* _in_image, VkImageLayout _layout);
    void FreeImageDescIdx(uint _idx);
    uint GetSamplerDescIdx(Sampler _sampler);
    uint GetAccelDescIdx(VulkanAccelerationStructure* _as);
    uint FreeAccelDescIdx(uint _idx);

public:
    uint64 CurrentFrameOffset(uint _frame_idx) const;
    void   BeginPushDescriptors(uint _frame_idx);

    void EndPushDescriptors(uint _frame_idx);

    // void PushBufferDesc(uint64 _src_offset, uint64 _set_offset);
    void PushUniformDesc(uint64 _src_offset, uint64 _set_offset);
    void PushStorageDesc(uint64 _src_offset, uint64 _set_offset);
    void PushImageDesc(uint64 _src_offset, uint64 _set_offset);
    void PushSamplerDesc(uint64 _src_offset, uint64 _set_offset);
    void PushAccelDesc(uint64 _src_offset, uint64 _set_offset);

    void IncrementOffset(uint64 _size);

public:
    VulkanDevice* m_device;

    VulkanBuffer* ring_desc_buffer;

    uint storage_desc_stride;
    uint uniform_desc_stride;
    uint storage_texel_desc_stride;
    uint uniform_texel_desc_stride;
    uint buffer_desc_stride;

    uint image_desc_stride;
    uint sample_desc_stride;
    uint accel_desc_stride;

    std::mutex m_mutex;
    uint64     texture_desc_offset;

    uint          ring_buffer_cnt;
    uint64        ring_buffer_size;
    Array<uint64> ring_buffer_offsets;
    uint64        current_offset;
    uint8*        map_ptr;
};

#pragma endregion
} // namespace Moer::Render

#endif