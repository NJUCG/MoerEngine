#pragma once

#include "VulkanCommon.h"
#include "VulkanDeviceObject.h"
#include "VulkanMemoryAllocator.h"
#include "rhi/RHIResource.h"

#include <atomic>
#include <mutex>

namespace Moer::Render {

class VulkanMemoryManager final {
public:
    VulkanMemoryManager()                                      = delete;
    VulkanMemoryManager(const VulkanMemoryManager&)            = delete;
    VulkanMemoryManager& operator=(const VulkanMemoryManager&) = delete;

    static VmaAllocationCreateFlags MEGenerateVmaMemoryFlags(EBufferUsageFlags _flags);
    static VmaMemoryUsage           MEGenerateVmaMemoryUsage();
};

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
    UnorderedMap<VulkanBufferDescriptorKey, uint, VulkanBufferDescriptorKey::Hasher>;

// Native allocation policy used during creation; resource lifetime stays with its owner.
struct VulkanBufferAllocationInfo {
    VkBufferUsageFlags       vk_usage{0};
    VmaAllocationCreateFlags allocation_flags{0};
    VmaMemoryUsage           memory_usage{VMA_MEMORY_USAGE_AUTO};
    bool                     needs_device_address{false};
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
    VulkanBuffer(
        std::string_view  _name,
        const BufferInfo& _info,
        VulkanDevice&     _device,
        VkBuffer          _handle,
        VmaAllocation     _alloc,
        bool              _defer_destroy,
        bool              _get_address = false
    );
    uint64                     DeviceAddress() const;
    inline const VmaAllocation GetAllocation() const {
        return m_alloc.alloc;
    }

    inline VkBuffer GetHandle() const {
        return m_alloc.buffer;
    }

    VkDescriptorType GetDescriptorType() const {
        return m_descriptor_type;
    }

private:
    friend class VulkanDescriptorHeap;

    VulkanBufferDescriptorCacheSlots& GetDescriptorCacheSlots(VkDescriptorType _type);
    VulkanBufferDescriptorCacheSlots  m_descriptor_cache_slots[4];

    BufferAlloc      m_alloc;
    uint64           m_device_address = 0;
    VkDescriptorType m_descriptor_type;
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
    [[nodiscard]] static VkImageLayout PreferredLayoutForUsage(ETextureUsageFlags _usage) noexcept {
        const bool presentation_source =
            (_usage & ETextureUsageFlags::PRESENTATION_SOURCE) == ETextureUsageFlags::PRESENTATION_SOURCE;
        const bool prefer_sample =
            !presentation_source && (_usage & ETextureUsageFlags::SAMPLED) == ETextureUsageFlags::SAMPLED &&
            (_usage & ETextureUsageFlags::UNORDERED_ACCESS) == ETextureUsageFlags::UNDEFINED;
        return prefer_sample ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
    }

    uint               GetMipByteSize(uint _mip_idx) const override;
    [[nodiscard]] bool SupportsOwningReadback() const noexcept override {
        return true;
    }
    VkImageView GetView(
        uint _mip_level  = 0,
        uint _mip_cnt    = 1,
        uint _base_layer = 0,
        uint _layer_cnt  = VK_REMAINING_ARRAY_LAYERS
    );
    VkImageView
    GetAttachmentView(uint _mip_level = 0, uint _mip_cnt = 1, uint _base_layer = 0, uint _layer_cnt = 1);

    void SetName(const std::string_view _name) override;
    bool b_has_preferred_state : 1 = false;
    bool b_present : 1             = false;
    void PublishPresentationSourceReady(bool _ready) noexcept {
        presentation_source_ready.store(_ready, std::memory_order_release);
    }
    [[nodiscard]] bool IsPresentationSourceReady() const noexcept {
        return presentation_source_ready.load(std::memory_order_acquire);
    }
    VkImageLayout GetPreferredLayout() const {
        return m_preferred_layout;
    };

    VkImageLayout GetQueuePreferredLayout(EQueueType _queue) {
        switch (_queue) {
            case EQueueType::Graphics:
                return m_preferred_layout;
            case EQueueType::Compute:
                return m_preferred_layout;
            case EQueueType::Copy:
                return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            default:
                return VK_IMAGE_LAYOUT_GENERAL;
        }
    }

private:
    friend class VulkanDescriptorHeap;

    UnorderedMap<VulkanTextureDescriptorKey, uint, VulkanTextureDescriptorKey::Hasher>
        m_descriptor_cache_slots;

    void Destroy() override;
    struct TextureAlloc {
        VkImage       image;
        VmaAllocation alloc;
    } m_alloc;
    UnorderedMap<uint64, VkImageView> m_views;
    UnorderedMap<uint64, VkImageView> m_attachment_views;
    mutable std::mutex                m_views_mutex;
    VkImageLayout                     m_preferred_layout = VK_IMAGE_LAYOUT_GENERAL;
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
