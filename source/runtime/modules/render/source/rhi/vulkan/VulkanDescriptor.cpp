#include "VulkanDescriptor.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanDevice.h"
#include "VulkanUtil.h"

#include "misc/MacroUtils.h"
#include "rhi/vulkan/misc/VulkanMacroUtils.h"

#include <vulkan/vulkan_core.h>
#include <cassert>

const float default_pool_size[VK_DESCRIPTOR_TYPE_RANGE_SIZE] = {
    4096,// VK_DESCRIPTOR_TYPE_SAMPLER
    4096,// VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
    4096,// VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
    //1 / 2.0,// VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER
    //1 / 4.0,// VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
    //4,      // VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
    //1 / 8.0,// VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC
    //1 / 8.0 // VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
};

VulkanDescriptorSetsLayout::VulkanDescriptorSetsLayout(VulkanDevice* _device, const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings) : VulkanDeviceObject(_device) {
    m_layouts.resize(_descriptor_bindings.size(), VK_NULL_HANDLE);
    for (uint32_t set_idx = 0; set_idx < _descriptor_bindings.size(); ++set_idx) {
        for (const auto& binding : _descriptor_bindings[set_idx]) {
            m_descriptor_type_count[binding.descriptorType] += binding.descriptorCount;
        }
        VkDescriptorSetLayoutCreateInfo set_layout_ci{};
        set_layout_ci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_layout_ci.bindingCount = _descriptor_bindings[set_idx].size();
        set_layout_ci.pBindings    = _descriptor_bindings[set_idx].data();
        VK_CHECK_RESULT(vkCreateDescriptorSetLayout(m_device->GetDevice(), &set_layout_ci, nullptr, &m_layouts[set_idx]));
    }
}

VulkanDescriptorSetsLayout::~VulkanDescriptorSetsLayout() {
    for (const auto& layout : m_layouts) {
        vkDestroyDescriptorSetLayout(m_device->GetDevice(), layout, nullptr);
    }
}

VulkanDescriptorSetAllocator::VulkanDescriptorSetAllocator(VulkanDevice* _device) : VulkanDeviceObject(_device) {
    // add default pool
    const uint32_t default_set_count = 4096;
    m_cache_pools.emplace_front(std::make_unique<VulkanDescriptorSetCachePool>(m_device, default_pool_size, default_set_count));
}

VulkanDescriptorSetAllocator::~VulkanDescriptorSetAllocator() {
    // Destructor implementation
    CleanUp();
}

bool VulkanDescriptorSetAllocator::GetDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, Moer::Array<VulkanDescriptorSetWriter>& _writers, Moer::Array<VkDescriptorSet>& _sets) {
    for (const auto& pool : m_cache_pools) {
        if (pool->FindDescriptorSets(_hash_key, _sets)) {
            return true;
        }
    }

    while (!m_cache_pools.front()->CreateDescriptorSets(_hash_key, _layout, _writers, _sets)) {
        CreatePool(_layout);
    }
    return true;
}

void VulkanDescriptorSetAllocator::ResetAll() {
    for (const auto& pool : m_cache_pools) {
        pool->Reset();
    }
}

void VulkanDescriptorSetAllocator::CleanUp() {
    for (const auto& pool : m_cache_pools) {
        pool->CleanUp();
    }
    m_cache_pools.clear();
}

void VulkanDescriptorSetAllocator::CreatePool(const VulkanDescriptorSetsLayout& _layout) {
    m_cache_pools.emplace_front(std::make_unique<VulkanDescriptorSetCachePool>(m_device, _layout));
}

VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::VulkanDescriptorSetCachePool(VulkanDevice* _device, const float _default_pool_size[VK_DESCRIPTOR_TYPE_RANGE_SIZE], uint32_t _set_count) : VulkanDeviceObject(_device) {
    Moer::Array<VkDescriptorPoolSize> pool_sizes(VK_DESCRIPTOR_TYPE_RANGE_SIZE);

    const uint32_t max_sets = GetMaxSets(_set_count);
    for (uint32_t i = 0; i < VK_DESCRIPTOR_TYPE_RANGE_SIZE; ++i) {
        pool_sizes[i].type            = static_cast<VkDescriptorType>(VK_DESCRIPTOR_TYPE_BEGIN_RANGE + i);
        pool_sizes[i].descriptorCount = static_cast<uint32_t>(_default_pool_size[i] * max_sets);
    }
    pool_sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096});
    pool_sizes.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096});
    pool_sizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096});
    pool_sizes.push_back({VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 4096});

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.pNext         = nullptr;
    pool_info.flags         = 0;
    pool_info.maxSets       = max_sets;
    pool_info.poolSizeCount = pool_sizes.size();
    pool_info.pPoolSizes    = pool_sizes.data();

    VK_CHECK_RESULT(vkCreateDescriptorPool(m_device->GetDevice(), &pool_info, nullptr, &m_pool));
}

VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::VulkanDescriptorSetCachePool(VulkanDevice* _device, const VulkanDescriptorSetsLayout& _layout) : VulkanDeviceObject(_device) {
    TDescriptorCountMap pool_size_info;
    const uint32_t      set_count = _layout.GetDescriptorSetCount();

    for (const auto& [type, count] : _layout.GetDescriptorTypeCount()) {
        pool_size_info[type] = count / set_count + 1;
    }

    Moer::Array<VkDescriptorPoolSize> pool_sizes;

    const uint32_t max_sets = GetMaxSets(set_count);
    for (const auto& [type, count] : pool_size_info) {
        VkDescriptorPoolSize pool_size{};
        pool_size.type            = type;
        pool_size.descriptorCount = count * max_sets;
        pool_sizes.push_back(pool_size);
    }

    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.pNext         = nullptr;
    pool_info.flags         = 0;
    pool_info.maxSets       = max_sets;
    pool_info.poolSizeCount = pool_sizes.size();
    pool_info.pPoolSizes    = pool_sizes.data();

    VK_CHECK_RESULT(vkCreateDescriptorPool(m_device->GetDevice(), &pool_info, nullptr, &m_pool));
}

VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::~VulkanDescriptorSetCachePool() {
    CleanUp();
}

bool VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::FindDescriptorSets(uint32_t _hash_key, Moer::Array<VkDescriptorSet>& _sets) {
    auto found_sets = m_allocated_sets.find(_hash_key);
    if (found_sets == m_allocated_sets.end()) {
        return false;
    }
    _sets = found_sets->second;
    return true;
}

bool VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::CreateDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, Moer::Array<VulkanDescriptorSetWriter>& _writers, Moer::Array<VkDescriptorSet>& _sets) {
    Moer::Array<VkDescriptorSet> new_sets(_writers.size());

    for (uint32_t i = 0; i < _writers.size(); ++i) {
        const auto  set_key   = _writers[i].GetSetKey();
        const auto& found_set = m_allocated_set.find(_writers[i].GetSetKey());
        if (found_set != m_allocated_set.end()) {
            new_sets[i] = found_set->second;
            continue;
        }

        if (!AllocateDescriptorSet(_layout.GetLayouts()[i], new_sets[i])) {
            return false;
        }

        m_allocated_set[set_key] = new_sets[i];
        _writers[i].SetDescriptorSet(new_sets[i]);

        vkUpdateDescriptorSets(
            m_device->GetDevice(),
            _writers[i].GetNumWrites(),
            _writers[i].GetWrites(),
            0,
            nullptr);
    }

    _sets = new_sets;

    m_allocated_sets[_hash_key] = std::move(new_sets);

    return true;
}

bool VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::AllocateDescriptorSet(VkDescriptorSetLayout _layout, VkDescriptorSet& _set) {
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext              = nullptr;
    alloc_info.descriptorPool     = m_pool;
    alloc_info.descriptorSetCount = 1;
    alloc_info.pSetLayouts        = &_layout;

    auto result = vkAllocateDescriptorSets(m_device->GetDevice(), &alloc_info, &_set) == VK_SUCCESS;
    // assert(result);
    return result;
}

void VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::Reset() {
    if (m_pool) {
        vkResetDescriptorPool(m_device->GetDevice(), m_pool, 0);
        m_allocated_sets.clear();
        m_allocated_set.clear();
    }
}

void VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::CleanUp() {
    if (m_pool) {
        vkDestroyDescriptorPool(m_device->GetDevice(), m_pool, nullptr);
        m_pool = VK_NULL_HANDLE;
        m_allocated_sets.clear();
        m_allocated_set.clear();
    }
}

uint32_t VulkanDescriptorSetAllocator::VulkanDescriptorSetCachePool::GetMaxSets(uint32_t _set_count) {
    GET_HIGH_BIT_UINT32(_set_count);

    /** MARK... default implementation
    if (_set_count < 2) return 2;
    if (_set_count < 4) return 4;
    if (_set_count < 8) return 8;
    if (_set_count < 16) return 16;
    if (_set_count < 32) return 32;
    if (_set_count < 64) return 64;
    if (_set_count < 128) return 128;
    if (_set_count < 256) return 256;
    if (_set_count < 512) return 512;
    if (_set_count < 1024) return 1024;
    return _set_count + 1;
    */
}

void VulkanDescriptorSetWriter::Init(const Moer::Array<DescriptorSetBindingInfo>& _binding_info, VulkanHashableDescriptorInfo* _hash_info_head, VkWriteDescriptorSet* _descriptor_write_head, VkDescriptorImageInfo* _image_info_head, VkDescriptorBufferInfo* _buffer_info_head, VkWriteDescriptorSetAccelerationStructureKHR* _as_write_head, VulkanDescriptorASInfo* _as_info_head) {
    m_hash_info_head        = _hash_info_head;
    m_descriptor_write_head = _descriptor_write_head;
    m_write_count           = _binding_info.size();

    uint32_t write_index = 0;

    for (const auto& binding : _binding_info) {
        _descriptor_write_head->sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        _descriptor_write_head->pNext           = nullptr;
        _descriptor_write_head->dstBinding      = binding.binding;
        _descriptor_write_head->descriptorCount = 1;
        _descriptor_write_head->descriptorType  = binding.type;

        switch (binding.type) {
            case VK_DESCRIPTOR_TYPE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                _descriptor_write_head->pImageInfo = _image_info_head++;
                break;
            // case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            // case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            //     break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                _descriptor_write_head->pBufferInfo = _buffer_info_head++;
                break;
            case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
                _as_write_head->sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
                _as_write_head->pNext                      = nullptr;
                _as_write_head->accelerationStructureCount = 1;
                _as_write_head->pAccelerationStructures    = &_as_info_head->as;
                ++_as_info_head;
                _descriptor_write_head->pNext = _as_write_head++;
                break;
            default:
                LOG_ERROR("Unsupported descriptor type: {}", VK_TYPE_TO_STRING(VkDescriptorType, binding.type));
                // MARK: it maybe not robust enough
                break;
        }
        ++_descriptor_write_head;
        m_write_index_map[binding.binding] = write_index++;
    }
}

void VulkanDescriptorSetWriter::SetDescriptorSet(VkDescriptorSet _set) {
    for (uint32_t i = 0; i < m_write_count; ++i) {
        m_descriptor_write_head[i].dstSet = _set;
    }
}

void VulkanDescriptorSetWriter::WriteSampler(uint32_t _binding, VkSampler _sampler, VkImageView _image_view, VkImageLayout _image_layout) {
    WriteImageInner<VK_DESCRIPTOR_TYPE_SAMPLER>(m_write_index_map[_binding], _sampler, _image_view, _image_layout);
}

void VulkanDescriptorSetWriter::WriteSampledImage(uint32_t _binding, VkSampler _sampler, VkImageView _image_view, VkImageLayout _image_layout) {
    WriteImageInner<VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE>(m_write_index_map[_binding], _sampler, _image_view, _image_layout);
}

void VulkanDescriptorSetWriter::WriteStorageImage(uint32_t _binding, VkImageView _image_view, VkImageLayout _image_layout) {
    WriteImageInner<VK_DESCRIPTOR_TYPE_STORAGE_IMAGE>(m_write_index_map[_binding], VK_NULL_HANDLE, _image_view, _image_layout);
}

void VulkanDescriptorSetWriter::WriteUniformBuffer(uint32_t _binding, VkBuffer _buffer, VkDeviceSize _offset, VkDeviceSize _range) {
    WriteBufferInner<VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER>(m_write_index_map[_binding], _buffer, _offset, _range);
}

void VulkanDescriptorSetWriter::WriteStorageBuffer(uint32_t _binding, VkBuffer _buffer, VkDeviceSize _offset, VkDeviceSize _range) {
    WriteBufferInner<VK_DESCRIPTOR_TYPE_STORAGE_BUFFER>(m_write_index_map[_binding], _buffer, _offset, _range);
}

void VulkanDescriptorSetWriter::WriteAccelerationStructure(uint32_t _binding, VkAccelerationStructureKHR _as, uint64_t _update_bit) {
    const auto write_index = m_write_index_map[_binding];

    VulkanDescriptorASInfo as_info{_as, _update_bit, UINT64_MAX};

    const VkWriteDescriptorSetAccelerationStructureKHR* write_as = nullptr;

    const auto* cursor = reinterpret_cast<const VkBaseInStructure*>(m_descriptor_write_head[write_index].pNext);
    while (cursor) {
        if (cursor->sType == VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR) {
            write_as = (reinterpret_cast<const VkWriteDescriptorSetAccelerationStructureKHR*>(cursor));
            break;
        }
        cursor = cursor->pNext;
    }

    VK_CHECK_NULLPTR(write_as, "AccelerationStructure descriptor set write is nullptr!", return);
    CHECK_ASSERT((write_as->accelerationStructureCount == 1), "AccelerationStructure descriptor set write count is not 1!");

    const_cast<VkWriteDescriptorSetAccelerationStructureKHR*>(write_as)->pAccelerationStructures = &as_info.as;

    m_hash_info_head[write_index].resource.as = as_info;
}

template<VkDescriptorType DescriptorType>
void VulkanDescriptorSetWriter::WriteImageInner(uint32_t _write_index, VkSampler _sampler, VkImageView _image_view, VkImageLayout _image_layout) {
    CHECK_ASSERT((m_descriptor_write_head[_write_index].descriptorType == DescriptorType), "WriteImageInner: Descriptor type mismatch at index {}: write {} which was expecting {}!", _write_index, VK_TYPE_TO_STRING(VkDescriptorType, m_descriptor_write_head[_write_index].descriptorType), VK_TYPE_TO_STRING(VkDescriptorType, DescriptorType));

    auto* image_info = const_cast<VkDescriptorImageInfo*>(m_descriptor_write_head[_write_index].pImageInfo);

    image_info->sampler     = _sampler;
    image_info->imageView   = _image_view;
    image_info->imageLayout = _image_layout;

    m_hash_info_head[_write_index].resource.image = *image_info;
}

template<VkDescriptorType DescriptorType>
void VulkanDescriptorSetWriter::WriteBufferInner(uint32_t _write_index, VkBuffer _buffer, VkDeviceSize _offset, VkDeviceSize _range) {
    CHECK_ASSERT((m_descriptor_write_head[_write_index].descriptorType == DescriptorType), "WriteBufferInner: Descriptor type mismatch at index {}: write {} which was expecting {}!", _write_index, VK_TYPE_TO_STRING(VkDescriptorType, m_descriptor_write_head[_write_index].descriptorType), VK_TYPE_TO_STRING(VkDescriptorType, DescriptorType));

    auto* buffer_info = const_cast<VkDescriptorBufferInfo*>(m_descriptor_write_head[_write_index].pBufferInfo);

    buffer_info->buffer = _buffer;
    buffer_info->offset = _offset;
    buffer_info->range  = _range;

    m_hash_info_head[_write_index].resource.buffer = *buffer_info;
}

uint32_t VulkanDescriptorSetWriter::GetSetKey() const {
    return Moer::RHI::Vulkan::Util::MemCrc32(
        m_hash_info_head,
        sizeof(VulkanHashableDescriptorInfo) * (m_write_count + 1));
}