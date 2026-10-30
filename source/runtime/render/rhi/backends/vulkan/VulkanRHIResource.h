#pragma once

#include "VulkanCommon.h"
#include "VulkanDeviceObject.h"
#include "VulkanMemoryAllocator.h"
#include "misc/Types.h"
#include "rhi/RHIResource.h"

#include <atomic>
#include <mutex>
#include <span>

namespace Moer::Render {

struct VulkanBufferDescriptorKey {
    uint64   byte_offset{0};
    uint64   byte_size{0};
    VkFormat format{VK_FORMAT_UNDEFINED};

    bool operator==(const VulkanBufferDescriptorKey& _other) const {
        return byte_offset == _other.byte_offset && byte_size == _other.byte_size && format == _other.format;
    }

    struct Hasher {
        size_t operator()(const VulkanBufferDescriptorKey& _key) const {
            size_t hash = std::hash<uint64>()(_key.byte_offset);
            hash ^= std::hash<uint64>()(_key.byte_size) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<uint32>()(uint32(_key.format)) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
};

using VulkanBufferDescriptorCacheSlots =
    UnorderedMap<VulkanBufferDescriptorKey, uint32, VulkanBufferDescriptorKey::Hasher>;

// Native allocation policy used during creation; resource lifetime stays with its owner.
struct VulkanBufferAllocationInfo {
    VkBufferUsageFlags       vk_usage{0};
    VmaAllocationCreateFlags allocation_flags{0};
    VmaMemoryUsage           memory_usage{VMA_MEMORY_USAGE_AUTO};
    bool                     needs_device_address{false};
    // Borrowed only during creation. Two or more distinct families use concurrent sharing.
    std::span<const uint32>   queue_family_indices{};
    VkDeviceSize              min_alignment = 1;
};

class VulkanBuffer : public Buffer, VulkanDeviceObject {
public:
    struct BufferAlloc {
        VkBuffer      buffer{VK_NULL_HANDLE};
        VmaAllocation alloc{VK_NULL_HANDLE};
    };
    VulkanBuffer() = delete;
    virtual ~VulkanBuffer();
    virtual void       Destroy() override;
    void               SetName(const std::string_view _name) override;
    [[nodiscard]] bool SupportsOwningReadback() const noexcept override {
        return true;
    }

    VulkanBuffer(std::string_view _name, const BufferInfo& _info, VulkanDevice& _device);
    VulkanBuffer(
        std::string_view                  _name,
        const BufferInfo&                 _info,
        VulkanDevice&                     _device,
        const VulkanBufferAllocationInfo& _allocation_info,
        bool                              _defer_destroy
    );
    uint64                     DeviceAddress() const;
    inline const VmaAllocation GetAllocation() const {
        return m_alloc.alloc;
    }

    inline VkBuffer GetHandle() const {
        return m_alloc.buffer;
    }

private:
    friend class VulkanDescriptorHeap;

    VulkanBufferDescriptorCacheSlots& GetDescriptorCacheSlots(VkDescriptorType _type);
    VulkanBufferDescriptorCacheSlots  m_descriptor_cache_slots[4];

    BufferAlloc m_alloc;
    uint64      m_device_address = 0;
};

struct VulkanTextureDescriptorKey {
    VkDescriptorType descriptor_type;
    VkImageLayout    layout;
    uint8            mip_level;
    uint8            mip_cnt;
    uint8            array_layer;
    uint8            array_count;

    bool operator==(const VulkanTextureDescriptorKey& _other) const {
        return descriptor_type == _other.descriptor_type && layout == _other.layout &&
               mip_level == _other.mip_level && mip_cnt == _other.mip_cnt &&
               array_layer == _other.array_layer && array_count == _other.array_count;
    }

    struct Hasher {
        size_t operator()(const VulkanTextureDescriptorKey& _key) const {
            size_t hash = std::hash<uint32>()(uint32(_key.descriptor_type));
            hash ^= std::hash<uint32>()(uint32(_key.layout)) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            const uint32 subresources = (uint32(_key.mip_level) << 24) | (uint32(_key.mip_cnt) << 16) |
                                        (uint32(_key.array_layer) << 8) | uint32(_key.array_count);
            hash ^= std::hash<uint32>()(subresources) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            return hash;
        }
    };
};
class VulkanTexture final : public Texture, public VulkanDeviceObject {
public:
    VulkanTexture() = delete;
    ~VulkanTexture();
    //for inner usage only
    explicit VulkanTexture(const TextureInfo& _info, VulkanDevice* _device, VkImage _image = VK_NULL_HANDLE);

    inline const VmaAllocation GetAllocation() const {
        return m_alloc.alloc;
    }

    inline VkImage GetHandle() const {
        return m_alloc.image;
    }

    uint32             GetMipByteSize(uint32 _mip_idx) const override;
    [[nodiscard]] bool SupportsOwningReadback() const noexcept override {
        return true;
    }
    VkImageView GetView(
        uint32 _mip_level  = 0,
        uint32 _mip_cnt    = 1,
        uint32 _base_layer = 0,
        uint32 _layer_cnt  = VK_REMAINING_ARRAY_LAYERS
    );
    VkImageView
    GetAttachmentView(uint32 _mip_level = 0, uint32 _mip_cnt = 1, uint32 _base_layer = 0, uint32 _layer_cnt = 1);

    void SetName(const std::string_view _name) override;
    bool b_present : 1             = false;
    void PublishPresentationSourceReady(bool _ready) noexcept {
        presentation_source_ready.store(_ready, std::memory_order_release);
    }
    [[nodiscard]] bool IsPresentationSourceReady() const noexcept {
        return presentation_source_ready.load(std::memory_order_acquire);
    }

private:
    friend class VulkanDescriptorHeap;

    UnorderedMap<VulkanTextureDescriptorKey, uint32, VulkanTextureDescriptorKey::Hasher>
        m_descriptor_cache_slots;

    void        Destroy() override;
    VkImageView GetOrCreateView(
        UnorderedMap<uint64, VkImageView>& cache,
        VkImageViewType                    view_type,
        uint32                             mip_level,
        uint32                             mip_count,
        uint32                             base_layer,
        uint32                             layer_count
    );
    struct TextureAlloc {
        VkImage       image;
        VmaAllocation alloc;
    } m_alloc;
    UnorderedMap<uint64, VkImageView> m_views;
    UnorderedMap<uint64, VkImageView> m_attachment_views;
    mutable std::mutex                m_views_mutex;
    std::atomic_bool                  presentation_source_ready{false};
};

using VulkanBufferRef  = CountableRef<VulkanBuffer>;
using VulkanTextureRef = CountableRef<VulkanTexture>;

inline VulkanBuffer* ResourceCast(Buffer* resource) {
    return static_cast<VulkanBuffer*>(resource);
}

inline VulkanTexture* ResourceCast(Texture* resource) {
    return static_cast<VulkanTexture*>(resource);
}

} // namespace Moer::Render
