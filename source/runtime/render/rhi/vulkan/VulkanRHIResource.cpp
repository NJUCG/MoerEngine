#include "VulkanRHIResource.h"
#include "PixelFormat.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "log/LogSystem.h"
#include "misc/MMemory.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace Moer::Render {

VmaAllocationCreateFlags VulkanMemoryManager::MEGenerateVmaMemoryFlags(EBufferUsageFlags _flags) {
    if ((_flags & EBufferUsageFlags::CPU_VISIBLE) == EBufferUsageFlags::CPU_VISIBLE)
        return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    return 0;
}

VmaMemoryUsage VulkanMemoryManager::MEGenerateVmaMemoryUsage() {
    return VMA_MEMORY_USAGE_AUTO;
}

uint64 EncodeViewKey(uint _mip_level, uint _mip_cnt, uint _base_layer, uint _layer_cnt) {
    return ((uint64)_mip_level) | ((uint64)_mip_cnt << 16) | ((uint64)_base_layer << 32) |
           ((uint64)_layer_cnt << 48);
}

VkImageTiling GetVkImageTilling(VkImageCreateInfo _create_infos, VulkanDevice& _device) {
    //query device property
    VkImageFormatProperties2 properties2{};
    properties2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
    VkPhysicalDeviceImageFormatInfo2 format_info{};
    format_info.sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
    format_info.format = _create_infos.format;
    format_info.type   = _create_infos.imageType;
    format_info.tiling = _create_infos.tiling;
    format_info.usage  = _create_infos.usage;
    format_info.flags  = _create_infos.flags;
    properties2.pNext  = VK_NULL_HANDLE;
    VkResult result = vkGetPhysicalDeviceImageFormatProperties2(_device.GetGpu(), &format_info, &properties2);
    if (result != VK_SUCCESS) {
        // LOG_CRITICAL("Failed to get image format properties");
        return VK_IMAGE_TILING_LINEAR;
    }
    return format_info.tiling;
}
VulkanTexture::VulkanTexture(const TextureInfo& _info, VulkanDevice* _device, VkImage _image) :
    Texture(_info),
    VulkanDeviceObject(_device) {
    b_present          = (_info.usage & ETextureUsageFlags::PRESENT) == ETextureUsageFlags::PRESENT;
    m_preferred_layout = PreferredLayoutForUsage(_info.usage);

    if (_image != VK_NULL_HANDLE) {
        m_alloc.image = _image;
        m_alloc.alloc = {};
        return;
    }
    // 验证 texture 参数
    if (_info.extent.x == 0 || _info.extent.y == 0) {
        LOG_ERROR(
            "Invalid texture extent: ({}, {}). Texture name: {}",
            _info.extent.x,
            _info.extent.y,
            _info.debug_name.has_value() ? _info.debug_name->c_str() : "Unknown"
        );
        assert(false && "Texture extent cannot be zero");
    }

    VkImageCreateInfo image_create_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_create_info.flags       = VulkanEnumTranslator::METoVKImageCreateFlags(_info.dimension);
    image_create_info.imageType   = VulkanEnumTranslator::METoVKImageType(_info.dimension);
    image_create_info.format      = VulkanEnumTranslator::METoVKFormat(_info.format);
    image_create_info.extent      = {uint(_info.extent.x), uint(_info.extent.y), _info.depth};
    image_create_info.mipLevels   = _info.num_mips;
    image_create_info.arrayLayers = _info.array_size;
    image_create_info.samples     = VulkanEnumTranslator::METoVKSampleCountFlagBits(_info.num_samples);
    image_create_info.tiling      = VK_IMAGE_TILING_OPTIMAL;
    image_create_info.usage       = VulkanEnumTranslator::METoVKImageUsageFlags(_info.usage) |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_create_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

#if WITH_CUDA
    image_create_info.pNext = GetExternalMemoryImageCreateInfoPtr(nullptr);
#endif

    auto tilling             = GetVkImageTilling(image_create_info, *_device);
    image_create_info.tiling = tilling;
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VulkanMemoryManager::MEGenerateVmaMemoryUsage();

    VmaAllocator allocator = m_device->GetVmaAllocator();
    VkResult     result    = vmaCreateImage(
        allocator, &image_create_info, &alloc_create_info, &m_alloc.image, &m_alloc.alloc, nullptr
    );
    if (result != VK_SUCCESS) {
        std::string texture_name = _info.debug_name.has_value() ? _info.debug_name->c_str() : "Unknown";
        LOG_CRITICAL(
            "Failed to create texture '{}': extent=({},{}), format={}, mips={}, layers={}, "
            "VkResult={}",
            texture_name,
            _info.extent.x,
            _info.extent.y,
            uint(_info.format),
            _info.num_mips,
            _info.array_size,
            int(result)
        );
        VK_CHECK_RESULT(result);
    }
    if (!_info.debug_name.has_value()) {
        SetName("UserTexture");
    } else {
        SetName(_info.debug_name->c_str());
    }
}

VkImageView VulkanTexture::GetView(uint _mip_level, uint _mip_cnt, uint _base_layer, uint _layer_cnt) {
    std::lock_guard<std::mutex> lock(m_views_mutex);
    uint64                      key = EncodeViewKey(_mip_level, _mip_cnt, _base_layer, _layer_cnt);
    auto                        it  = m_views.find(key);
    if (it != m_views.end()) {
        return it->second;
    }
    VkImageView           view;
    VkImageViewCreateInfo view_create_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_create_info.image = m_alloc.image;

    // 如果是 CubeMap 但只请求了 1 层，必须降级为 2D View
    if (GetDimension() == ETextureDimension::TEX_CUBE && _layer_cnt == 1) {
        view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    } else {
        view_create_info.viewType = VulkanEnumTranslator::METoVKImageViewType(GetDimension());
    }

    view_create_info.format     = VulkanEnumTranslator::METoVKFormat(GetFormat());
    view_create_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };
    view_create_info.subresourceRange.aspectMask =
        VulkanEnumTranslator::METoVKImageAspectFlags(GetAspectFlags());
    view_create_info.subresourceRange.baseMipLevel   = _mip_level;
    view_create_info.subresourceRange.levelCount     = _mip_cnt; // 含义：这次我想看多少，而不是纹理总共有多少
    view_create_info.subresourceRange.baseArrayLayer = _base_layer;
    view_create_info.subresourceRange.layerCount = _layer_cnt; // 含义：这次我想看多少，而不是纹理总共有多少
    view_create_info.pNext                       = nullptr;
    view_create_info.flags                       = 0;

    VK_CHECK_RESULT(vkCreateImageView(m_device->GetDevice(), &view_create_info, nullptr, &view));
    m_views[key] = view;
    return view;
}

VkImageView
VulkanTexture::GetAttachmentView(uint _mip_level, uint _mip_cnt, uint _base_layer, uint _layer_cnt) {
    const bool requires_attachment_view = _layer_cnt > 1 || GetDimension() == ETextureDimension::TEX_CUBE ||
                                          GetDimension() == ETextureDimension::TEX_CUBE_ARRAY;
    if (!requires_attachment_view) {
        return GetView(_mip_level, _mip_cnt, _base_layer, _layer_cnt);
    }

    std::lock_guard<std::mutex> lock(m_views_mutex);
    const uint64                key = EncodeViewKey(_mip_level, _mip_cnt, _base_layer, _layer_cnt);
    const auto                  it  = m_attachment_views.find(key);
    if (it != m_attachment_views.end()) {
        return it->second;
    }

    VkImageViewCreateInfo view_create_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_create_info.image      = m_alloc.image;
    view_create_info.viewType   = _layer_cnt == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_create_info.format     = VulkanEnumTranslator::METoVKFormat(GetFormat());
    view_create_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };
    view_create_info.subresourceRange.aspectMask =
        VulkanEnumTranslator::METoVKImageAspectFlags(GetAspectFlags());
    view_create_info.subresourceRange.baseMipLevel   = _mip_level;
    view_create_info.subresourceRange.levelCount     = _mip_cnt;
    view_create_info.subresourceRange.baseArrayLayer = _base_layer;
    view_create_info.subresourceRange.layerCount     = _layer_cnt;

    VkImageView view = VK_NULL_HANDLE;
    VK_CHECK_RESULT(vkCreateImageView(m_device->GetDevice(), &view_create_info, nullptr, &view));
    m_attachment_views.emplace(key, view);
    return view;
}

uint VulkanTexture::GetMipByteSize(uint _mip_level) const {
    uint         mip_width  = std::max(1u, GetWidth() >> _mip_level);
    uint         mip_height = std::max(1u, GetHeight() >> _mip_level);
    uint         mip_depth  = std::max(1u, GetDepth() >> _mip_level);
    const uint64 byte_size  = GetSizeFromPixelFormat(GetFormat(), uint3{mip_width, mip_height, mip_depth});
    MOER_ASSERT(
        byte_size <= std::numeric_limits<uint>::max(), "Texture mip size exceeds uint: {}", byte_size
    );
    return static_cast<uint>(byte_size);
}

VulkanBuffer::~VulkanBuffer() {
    if (m_alloc.buffer != VK_NULL_HANDLE && m_alloc.alloc != VK_NULL_HANDLE) {
        vmaDestroyBuffer(m_device->GetVmaAllocator(), m_alloc.buffer, m_alloc.alloc);
    }

    for (uint i = 0; i < 4; ++i) {
        for (const auto& idx : m_descriptor_cache_slots[i]) {
            m_device->GetGlobalDescriptorHeap().FreeBufferDescriptorSlot(idx.second);
        }
    }
}

VulkanBuffer::VulkanBuffer(std::string_view _name, const BufferInfo& _info, VulkanDevice& _device) :
    VulkanBuffer(
        _name,
        _info,
        _device,
        VulkanBufferAllocationInfo{
            .vk_usage = VulkanEnumTranslator::METoVKBufferUsageFlags(_info.usage) |
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .allocation_flags     = VulkanMemoryManager::MEGenerateVmaMemoryFlags(_info.usage),
            .memory_usage         = VulkanMemoryManager::MEGenerateVmaMemoryUsage(),
            .needs_device_address = true,
        },
        true
    ) {}

VulkanBuffer::VulkanBuffer(
    std::string_view                  _name,
    const BufferInfo&                 _info,
    VulkanDevice&                     _device,
    const VulkanBufferAllocationInfo& _allocation_info,
    bool                              _defer_destroy
) :
    Buffer(_info),
    VulkanDeviceObject(&_device) {
    if (_info.size == 0 || _info.stride == 0) {
        throw std::invalid_argument("Vulkan buffer size and stride must be nonzero");
    }
    if (_info.size > std::numeric_limits<VkDeviceSize>::max() / _info.stride) {
        throw std::overflow_error("Vulkan buffer byte size exceeds VkDeviceSize");
    }
    VkBufferCreateInfo buffer_info{
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = _info.size * _info.stride,
        .usage       = _allocation_info.vk_usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
#if WITH_CUDA
    buffer_info.pNext = GetExternalMemoryBufferCreateInfoPtr(nullptr);
#endif
    const VmaAllocationCreateInfo allocation_info{
        .flags = _allocation_info.allocation_flags,
        .usage = _allocation_info.memory_usage,
    };
    const VkResult result = vmaCreateBuffer(
        _device.GetVmaAllocator(), &buffer_info, &allocation_info, &m_alloc.buffer, &m_alloc.alloc, nullptr
    );
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::format(
                "failed to create Vulkan buffer '{}' ({} bytes, VkResult {})",
                _name,
                buffer_info.size,
                static_cast<int>(result)
            )
        );
    }
    try {
        b_deferred_delete = _defer_destroy;
        m_descriptor_type = VulkanEnumTranslator::METoVkBufferDescriptorType(_info.usage);
        if (_allocation_info.needs_device_address) {
            const VkBufferDeviceAddressInfo address_info{
                .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                .buffer = m_alloc.buffer,
            };
            m_device_address = vkGetBufferDeviceAddress(_device.GetDevice(), &address_info);
        }
        SetName(_name);
    } catch (...) {
        vmaDestroyBuffer(_device.GetVmaAllocator(), m_alloc.buffer, m_alloc.alloc);
        m_alloc = {};
        throw;
    }
}

uint64 VulkanBuffer::DeviceAddress() const {
    return m_device_address;
}

VulkanBuffer::VulkanBuffer(
    std::string_view  _name,
    const BufferInfo& _info,
    VulkanDevice&     _device,
    VkBuffer          _handle,
    VmaAllocation     _alloc,
    bool              _defer_destroy,
    bool              _get_address
) :
    Buffer(_info),
    VulkanDeviceObject(&_device) {
    m_alloc.buffer    = _handle;
    m_alloc.alloc     = _alloc;
    b_deferred_delete = _defer_destroy;
    m_descriptor_type = VulkanEnumTranslator::METoVkBufferDescriptorType(_info.usage);
    if (_get_address) {
        VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        info.buffer      = m_alloc.buffer;
        m_device_address = vkGetBufferDeviceAddress(m_device->GetDevice(), &info);
    }
    SetName(_name);
}

VulkanBufferDescriptorCacheSlots& VulkanBuffer::GetDescriptorCacheSlots(VkDescriptorType _type) {
    switch (_type) {
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            return m_descriptor_cache_slots[0];
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            return m_descriptor_cache_slots[1];
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            return m_descriptor_cache_slots[2];
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            return m_descriptor_cache_slots[3];
        default:
            assert(false);
    }
    return m_descriptor_cache_slots[0];
}

VulkanTexture::~VulkanTexture() {
    //destroy image views
    {
        std::lock_guard<std::mutex> lock(m_views_mutex);
        for (auto& [key, view] : m_views) {
            vkDestroyImageView(m_device->GetDevice(), view, VK_NULL_HANDLE);
        }
        m_views.clear();
        for (auto& [key, view] : m_attachment_views) {
            vkDestroyImageView(m_device->GetDevice(), view, VK_NULL_HANDLE);
        }
        m_attachment_views.clear();
    }
    if (m_alloc.image != VK_NULL_HANDLE && m_alloc.alloc != VK_NULL_HANDLE) {
        vmaDestroyImage(m_device->GetVmaAllocator(), m_alloc.image, m_alloc.alloc);
    }
    //free descriptors
    for (auto& [key, desc] : m_descriptor_cache_slots) {
        m_device->GetGlobalDescriptorHeap().FreeImageDescriptorSlot(desc);
    }
}

void VulkanBuffer::Destroy() {
    if (b_deferred_delete) {
        m_device->EnqueueDeferredRelease(this);
        return;
    }
    MoerDelete(this);
}

void VulkanBuffer::SetName(const std::string_view _name) {
    debug_name = _name;
    m_device->SetResourceName(uint64(m_alloc.buffer), VK_OBJECT_TYPE_BUFFER, _name);
}

void VulkanTexture::Destroy() {
    if (b_deferred_delete) {
        m_device->EnqueueDeferredRelease(this);
        return;
    }
    MoerDelete(this);
}

void VulkanTexture::SetName(const std::string_view _name) {
    debug_name = _name;
    m_device->SetResourceName(uint64(m_alloc.image), VK_OBJECT_TYPE_IMAGE, _name);
}

} // namespace Moer::Render
