#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "misc/MacroUtils.h"
#include "VulkanDescriptor.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanDevice.h"

void VulkanDescriptorSetsLayout::Init(const std::vector<TDescriptorSetLayout>& _layout_mappings, VulkanPipelineResourceCache* _cache) {
    m_layouts.resize(_layout_mappings.size(), VK_NULL_HANDLE);
    auto& writers = _cache->m_writers;
    writers.resize(_layout_mappings.size(), {_cache});
    _cache->m_descriptor_sets.resize(_layout_mappings.size(), VK_NULL_HANDLE);

    auto& hash_infos = _cache->m_descriptor_resource_container.hashable_descriptor_infos;

    uint32_t hash_index = 0, binding_index = 0;
    for (uint32_t i = 0; i < _layout_mappings.size(); ++i) {
        binding_index = 0;

        m_layouts[i] = _layout_mappings[i].first;

        writers[i].m_hash_info_head = hash_index;
        writers[i].m_write_set.resize(_layout_mappings[i].second.size());

        hash_infos.insert(hash_infos.end(), _layout_mappings[i].second.size() + 1, {});
        hash_infos[hash_index].layout = {UINT64_MAX, UINT64_MAX, m_layouts[i]};

        ++hash_index;// index + 1 for layout
        for (auto& binding : _layout_mappings[i].second) {
            m_sets_binding_count[binding.descriptorType] += binding.descriptorCount;
            m_descriptor_binding_infos[i][binding.binding] = {binding.descriptorType, binding.descriptorCount, hash_index};

            writers[i].m_index_of_binding[binding.binding] = binding_index;

            ++binding_index;
            ++hash_index;
        }
    }
    // _cache->m_descriptor_resource_container.hashable_descriptor_infos.resize(hash_index);
    _cache->m_descriptor_resource_container.descriptor_buffer_infos.resize(_layout_mappings.size());
    _cache->m_descriptor_resource_container.descriptor_image_infos.resize(_layout_mappings.size());
}

VulkanDescriptorSetAllocator::VulkanDescriptorSetAllocator() : VulkanDeviceObject(nullptr) {}

VulkanDescriptorSetAllocator::~VulkanDescriptorSetAllocator() {
    // Destructor implementation
    CleanUp();
}

void VulkanDescriptorSetAllocator::Init(VulkanDevice* device) {
    this->m_device = device;
}

bool VulkanDescriptorSetAllocator::GetDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, std::vector<VkDescriptorSet>& _sets) {
    for (const auto& pool : m_cache_pools) {
        if (pool->FindDescriptorSets(_hash_key, _sets)) {
            return true;
        }
    }

    while (!m_cache_pools.front()->CreateDescriptorSets(_hash_key, _layout, _sets)) {
        CreatePool(_layout);
    }
}

bool VulkanDescriptorSetAllocator::AllocateDescriptorSet(const VulkanDescriptorSetsLayout& _layout, VkDescriptorSet* _set) {
    if (m_current_pool == VK_NULL_HANDLE) {
        m_current_pool = GetAvailablePool(_layout);
        m_used_pools.push_back(m_current_pool);
    }
    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext              = nullptr;
    alloc_info.descriptorSetCount = _layout.GetDescriptorSetCount();
    alloc_info.pSetLayouts        = _layout.GetLayouts().data();

    bool     need_reallocate  = false;
    uint32_t reallocate_limit = 10;
    while (--reallocate_limit) {
        if (need_reallocate) {
            m_current_pool = GetAvailablePool(_layout);
            m_used_pools.push_back(m_current_pool);
        }
        alloc_info.descriptorPool = m_current_pool;
        auto result               = vkAllocateDescriptorSets(m_device->GetDevice(), &alloc_info, _set);
        switch (result) {
            case VK_SUCCESS:
                // all good, return
                need_reallocate = false;
                break;
            case VK_ERROR_FRAGMENTED_POOL:
            case VK_ERROR_OUT_OF_POOL_MEMORY:
                // reallocate pool
                need_reallocate = true;
                break;
            default:
                LOG_ERROR("Allocate: Unexpected error code: {}.", static_cast<int32_t>(result));
                // unrecoverable error
                return false;
        }
    }

    if (reallocate_limit == 0) {
        LOG_ERROR("Allocate: Failed to allocate descriptor sets.");
        return false;
    }

    return true;
}

void VulkanDescriptorSetAllocator::ResetAll() {
    for (auto& pool : m_used_pools) {
        vkResetDescriptorPool(m_device->GetDevice(), pool, 0);
        m_free_pools.push_back(pool);
    }

    m_used_pools.clear();
    m_current_pool = VK_NULL_HANDLE;
}

void VulkanDescriptorSetAllocator::CleanUp() {
    for (const auto& pool : m_free_pools) {
        vkDestroyDescriptorPool(m_device->GetDevice(), pool, nullptr);
    }
    for (const auto& pool : m_used_pools) {
        vkDestroyDescriptorPool(m_device->GetDevice(), pool, nullptr);
    }
    m_current_pool = VK_NULL_HANDLE;
}

VkDescriptorPool VulkanDescriptorSetAllocator::GetAvailablePool(const VulkanDescriptorSetsLayout& _layout) {
    if (m_free_pools.empty()) {
        return CreatePool(_layout);
    }

    VkDescriptorPool pool = m_free_pools.back();
    m_free_pools.pop_back();

    return pool;
}

VkDescriptorPool VulkanDescriptorSetAllocator::CreatePool(const VulkanDescriptorSetsLayout& _layout) {
    TDescriptorCountMap pool_size_info;
    const uint32_t      set_count = _layout.GetDescriptorSetCount();

    for (const auto& [type, count] : _layout.GetSetsBindingCount()) {
        pool_size_info[type] = count / set_count + 1;
    }

    std::vector<VkDescriptorPoolSize> pool_sizes;

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

    VkDescriptorPool pool;
    VK_CHECK_RESULT(vkCreateDescriptorPool(m_device->GetDevice(), &pool_info, nullptr, &pool));
    return pool;
}

uint32_t VulkanDescriptorSetAllocator::GetMaxSets(uint32_t _set_count) const {
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

void VulkanDescriptorSetWriter::WriteSampler(uint16_t _binding, const VkDescriptorImageInfo& _sampler, const VkDescriptorSet& _set, uint32_t _count, VkDescriptorType _type) {
    VkWriteDescriptorSet write{};
    write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext            = nullptr;
    write.dstSet           = _set;
    write.dstBinding       = _binding;
    write.dstArrayElement  = 0;
    write.descriptorCount  = _count;
    write.descriptorType   = _type;
    write.pImageInfo       = &_sampler;
    write.pBufferInfo      = nullptr;
    write.pTexelBufferView = nullptr;

    // update some vkDescriptorWrite info
    m_write_set[m_index_of_binding[_binding]] = std::move(write);
    m_cache->UpdateDescriptorSetHashInfo(m_hash_info_head + m_index_of_binding[_binding], _sampler);
}

void VulkanDescriptorSetWriter::WriteImage(uint16_t _binding, const VkDescriptorImageInfo& _image, const VkDescriptorSet& _set, uint32_t _count, VkDescriptorType _type) {
    VkWriteDescriptorSet write{};
    write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext            = nullptr;
    write.dstSet           = _set;
    write.dstBinding       = _binding;
    write.dstArrayElement  = 0;
    write.descriptorCount  = _count;
    write.descriptorType   = _type;
    write.pImageInfo       = &_image;
    write.pBufferInfo      = nullptr;
    write.pTexelBufferView = nullptr;

    // update some vkDescriptorWrite info
    m_write_set[m_index_of_binding[_binding]] = std::move(write);
    m_cache->UpdateDescriptorSetHashInfo(m_hash_info_head + m_index_of_binding[_binding], _image);
}

void VulkanDescriptorSetWriter::WriteBuffer(uint16_t _binding, const VkDescriptorBufferInfo& _buffer, const VkDescriptorSet& _set, uint32_t _count, VkDescriptorType _type) {
    VkWriteDescriptorSet write{};
    write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext            = nullptr;
    write.dstSet           = _set;
    write.dstBinding       = _binding;
    write.dstArrayElement  = 0;
    write.descriptorCount  = _count;
    write.descriptorType   = _type;
    write.pImageInfo       = nullptr;
    write.pBufferInfo      = &_buffer;
    write.pTexelBufferView = nullptr;

    // update some vkDescriptorWrite info
    m_write_set[m_index_of_binding[_binding]] = std::move(write);
    m_cache->UpdateDescriptorSetHashInfo(m_hash_info_head + m_index_of_binding[_binding], _buffer);
}
