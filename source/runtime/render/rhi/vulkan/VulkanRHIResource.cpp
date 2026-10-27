#include "VulkanRHIResource.h"
#include "PixelFormat.h"
#include "VulkanDescriptor.h"
#include "VulkanDevice.h"
#include "VulkanEnumTranslator.h"
#include "VulkanMacroUtils.h"
#include "log/LogSystem.h"
#include "misc/MMemory.h"

#include <algorithm>
#include <format>
#include <limits>
#include <stdexcept>

namespace Moer::Render {

namespace {

VmaAllocationCreateFlags GetBufferAllocationFlags(EBufferUsageFlags flags) {
    if ((flags & EBufferUsageFlags::CPU_VISIBLE) == EBufferUsageFlags::CPU_VISIBLE)
        return VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    return 0;
}

uint64 EncodeViewKey(uint _mip_level, uint _mip_cnt, uint _base_layer, uint _layer_cnt) {
    return ((uint64)_mip_level) | ((uint64)_mip_cnt << 16) | ((uint64)_base_layer << 32) |
           ((uint64)_layer_cnt << 48);
}

void ValidateImageFormatSupport(VkPhysicalDevice gpu, const VkImageCreateInfo& image_info) {
    VkPhysicalDeviceImageFormatInfo2 format_info{
        .sType  = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .format = image_info.format,
        .type   = image_info.imageType,
        .tiling = image_info.tiling,
        .usage  = image_info.usage,
        .flags  = image_info.flags,
    };
#if WITH_CUDA
    // Match the external-memory configuration used by texture creation.
    const VkPhysicalDeviceExternalImageFormatInfo external_info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };
    format_info.pNext = &external_info;
#endif
    VkImageFormatProperties2 properties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    VK_CHECK_RESULT(vkGetPhysicalDeviceImageFormatProperties2(gpu, &format_info, &properties));

    const auto& limits = properties.imageFormatProperties;
    if (image_info.extent.width == 0 || image_info.extent.height == 0 || image_info.extent.depth == 0 ||
        image_info.extent.width > limits.maxExtent.width ||
        image_info.extent.height > limits.maxExtent.height || image_info.extent.depth > limits.maxExtent.depth ||
        image_info.mipLevels == 0 || image_info.mipLevels > limits.maxMipLevels ||
        image_info.arrayLayers == 0 || image_info.arrayLayers > limits.maxArrayLayers ||
        (image_info.samples & limits.sampleCounts) == 0) {
        throw std::invalid_argument("Vulkan texture dimensions, mip levels, layers or samples exceed format support");
    }
}

} // namespace

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
            .allocation_flags     = GetBufferAllocationFlags(_info.usage),
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
    const auto queue_families = _allocation_info.queue_family_indices;
    if (queue_families.size() > 1) {
        if (queue_families.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::overflow_error("Vulkan buffer queue family count exceeds uint32_t");
        }
        for (auto family = queue_families.begin(); family != queue_families.end(); ++family) {
            if (std::find(queue_families.begin(), family, *family) != family) {
                throw std::invalid_argument("Vulkan buffer concurrent queue families must be distinct");
            }
        }
        buffer_info.sharingMode           = VK_SHARING_MODE_CONCURRENT;
        buffer_info.queueFamilyIndexCount = static_cast<uint32_t>(queue_families.size());
        buffer_info.pQueueFamilyIndices   = queue_families.data();
    }
#if WITH_CUDA
    const auto external_memory_info = MakeExternalMemoryBufferCreateInfo();
    buffer_info.pNext = &external_memory_info;
#endif
    const VmaAllocationCreateInfo allocation_info{
        .flags = _allocation_info.allocation_flags,
        .usage = _allocation_info.memory_usage,
    };
    const VkResult result = vmaCreateBufferWithAlignment(
        _device.GetVmaAllocator(),
        &buffer_info,
        &allocation_info,
        _allocation_info.min_alignment,
        &m_alloc.buffer,
        &m_alloc.alloc,
        nullptr
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
            throw std::invalid_argument("unsupported buffer descriptor cache type");
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

VulkanTexture::VulkanTexture(const TextureInfo& _info, VulkanDevice* _device, VkImage _image) :
    Texture(_info),
    VulkanDeviceObject(_device) {
    b_present          = (_info.usage & ETextureUsageFlags::PRESENT) == ETextureUsageFlags::PRESENT;

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
        throw std::invalid_argument("Vulkan texture extent cannot be zero");
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
    const auto external_memory_info = MakeExternalMemoryImageCreateInfo();
    image_create_info.pNext = &external_memory_info;
#endif

    ValidateImageFormatSupport(_device->GetGpu(), image_create_info);
    VmaAllocationCreateInfo alloc_create_info{};
    alloc_create_info.flags = 0;
    alloc_create_info.usage = VMA_MEMORY_USAGE_AUTO;

    VmaAllocator allocator = m_device->GetVmaAllocator();
    VkResult     result    = vmaCreateImage(
        allocator, &image_create_info, &alloc_create_info, &m_alloc.image, &m_alloc.alloc, nullptr
    );
    if (result != VK_SUCCESS) {
        std::string texture_name = _info.debug_name.has_value() ? _info.debug_name->c_str() : "Unknown";
        LOG_CRITICAL(
            "Failed to create texture '{}': extent=({},{},{}), format={}, mips={}, layers={}, "
            "samples={}, usage={}, tiling={}, VkResult={}",
            texture_name,
            _info.extent.x,
            _info.extent.y,
            _info.depth,
            uint(_info.format),
            _info.num_mips,
            _info.array_size,
            uint(image_create_info.samples),
            image_create_info.usage,
            uint(image_create_info.tiling),
            int(result)
        );
        throw std::runtime_error(std::format("failed to create Vulkan texture (VkResult {})", int(result)));
    }
    try {
        SetName(_info.debug_name.value_or("UserTexture"));
    } catch (...) {
        vmaDestroyImage(allocator, m_alloc.image, m_alloc.alloc);
        m_alloc = {};
        throw;
    }
}

VkImageView VulkanTexture::GetView(uint _mip_level, uint _mip_cnt, uint _base_layer, uint _layer_cnt) {
    const VkImageViewType view_type = GetDimension() == ETextureDimension::TEX_CUBE && _layer_cnt == 1 ?
                                          VK_IMAGE_VIEW_TYPE_2D :
                                          VulkanEnumTranslator::METoVKImageViewType(GetDimension());
    return GetOrCreateView(m_views, view_type, _mip_level, _mip_cnt, _base_layer, _layer_cnt);
}

VkImageView
VulkanTexture::GetAttachmentView(uint _mip_level, uint _mip_cnt, uint _base_layer, uint _layer_cnt) {
    const bool requires_attachment_view = _layer_cnt > 1 || GetDimension() == ETextureDimension::TEX_CUBE ||
                                          GetDimension() == ETextureDimension::TEX_CUBE_ARRAY;
    if (!requires_attachment_view) {
        return GetView(_mip_level, _mip_cnt, _base_layer, _layer_cnt);
    }
    const VkImageViewType view_type = _layer_cnt == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    return GetOrCreateView(m_attachment_views, view_type, _mip_level, _mip_cnt, _base_layer, _layer_cnt);
}

VkImageView VulkanTexture::GetOrCreateView(
    UnorderedMap<uint64, VkImageView>& cache,
    VkImageViewType                    view_type,
    uint                               mip_level,
    uint                               mip_count,
    uint                               base_layer,
    uint                               layer_count
) {
    std::lock_guard<std::mutex> lock(m_views_mutex);
    const uint64                key = EncodeViewKey(mip_level, mip_count, base_layer, layer_count);
    const auto                  it  = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image      = m_alloc.image;
    view_info.viewType   = view_type;
    view_info.format     = VulkanEnumTranslator::METoVKFormat(GetFormat());
    view_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY
    };
    view_info.subresourceRange = {
        VulkanEnumTranslator::METoVKImageAspectFlags(GetAspectFlags()),
        mip_level,
        mip_count,
        base_layer,
        layer_count
    };

    VkImageView    view   = VK_NULL_HANDLE;
    const VkResult result = vkCreateImageView(m_device->GetDevice(), &view_info, nullptr, &view);
    if (result != VK_SUCCESS) {
        throw std::runtime_error(
            std::format("failed to create Vulkan image view (VkResult {})", int(result))
        );
    }
    try {
        cache.emplace(key, view);
    } catch (...) {
        vkDestroyImageView(m_device->GetDevice(), view, nullptr);
        throw;
    }
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
