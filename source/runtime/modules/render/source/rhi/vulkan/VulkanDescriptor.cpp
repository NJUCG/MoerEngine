#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "misc/MacroUtils.h"
#include "VulkanDescriptor.h"
#include "VulkanPipelineResourceCache.h"
#include "VulkanDevice.h"

#include "misc/Crc32.h"
#include "vulkan/vulkan_core.h"
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

void VulkanDescriptorSetsLayout::Init(const Moer::Array<TDescriptorSetLayoutInfo>& _layout_mappings, VulkanPipelineResourceCache* _cache) {
    m_layouts.resize(_layout_mappings.size(), VK_NULL_HANDLE);
    auto& writers = _cache->m_descriptor_set_writers;
    writers.resize(_layout_mappings.size(), {_cache});
    _cache->m_descriptor_sets.resize(_layout_mappings.size(), VK_NULL_HANDLE);

    auto& hash_infos   = _cache->m_descriptor_resource_container.hashable_descriptor_infos;
    auto& image_infos  = _cache->m_descriptor_resource_container.descriptor_image_infos;
    auto& buffer_infos = _cache->m_descriptor_resource_container.descriptor_buffer_infos;

    uint32_t hash_index = 0, binding_index = 0, image_index = 0, buffer_index = 0;
    for (uint32_t i = 0; i < _layout_mappings.size(); ++i) {
        binding_index = 0;
        image_index   = 0;
        buffer_index  = 0;

        m_layouts[i] = _layout_mappings[i].first;

        writers[i].m_hash_info_head = hash_index;
        writers[i].m_write_set.resize(_layout_mappings[i].second.size());

        hash_infos.insert(hash_infos.end(), _layout_mappings[i].second.size() + 1, {});
        hash_infos[hash_index].layout = {UINT64_MAX, UINT64_MAX, m_layouts[i]};

        ++hash_index;// index + 1 for layout

        uint32_t image_count = 0, buffer_count = 0;
        for (auto& binding : _layout_mappings[i].second) {
            m_sets_binding_count[binding.descriptorType] += binding.descriptorCount;

            switch (binding.descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    image_count += binding.descriptorCount;
                    writers[i].m_index_of_image[binding.binding] = image_index++;
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
                    buffer_count += binding.descriptorCount;
                    writers[i].m_index_of_buffer[binding.binding] = buffer_index++;
                    break;
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                    break;
                default:
                    LOG_WARNING("Unsupported descriptor type: {}", static_cast<uint32_t>(binding.descriptorType));
            }
            m_descriptor_binding_infos[i][binding.binding] = {binding.descriptorType, binding.descriptorCount, hash_index};

            writers[i].m_index_of_binding[binding.binding] = binding_index++;

            ++hash_index;
        }
        image_infos.emplace_back(Moer::Array<VkDescriptorImageInfo>(image_count));
        buffer_infos.emplace_back(Moer::Array<VkDescriptorBufferInfo>(buffer_count));
    }
}

VulkanDescriptorSetAllocator::VulkanDescriptorSetAllocator() : VulkanDeviceObject(nullptr) {}

VulkanDescriptorSetAllocator::~VulkanDescriptorSetAllocator() {
    // Destructor implementation
    CleanUp();
}

void VulkanDescriptorSetAllocator::Init(VulkanDevice* device) {
    this->m_device = device;
    // add default pool
    const uint32_t default_set_count = 4096;
    m_cache_pools.emplace_front(std::make_unique<VulkanDescriptorSetCachePool>(m_device, default_pool_size, default_set_count));
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

    for (const auto& [type, count] : _layout.GetSetsBindingCount()) {
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
    assert(result);
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

void VulkanDescriptorSetWriter::SetDescriptorSet(VkDescriptorSet _set) {
    for (auto& write : m_write_set) {
        write.dstSet = _set;
    }
}

void VulkanDescriptorSetWriter::WriteSampler(uint16_t _set, uint16_t _binding, const VkDescriptorImageInfo& _sampler, uint32_t _count, VkDescriptorType _type) {
    const auto& sampler_info = m_cache->UpdateDescriptorImageInfo(_set, m_index_of_image[_binding], _sampler);

    VkWriteDescriptorSet write{};
    write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext            = nullptr;
    write.dstSet           = VK_NULL_HANDLE;
    write.dstBinding       = _binding;
    write.dstArrayElement  = 0;
    write.descriptorCount  = _count;
    write.descriptorType   = _type;
    write.pImageInfo       = &sampler_info;
    write.pBufferInfo      = nullptr;
    write.pTexelBufferView = nullptr;

    // update some vkDescriptorWrite info
    m_write_set[m_index_of_binding[_binding]] = std::move(write);
    VulkanHashableDescriptorInfo info;
    info.resource.sampler = _sampler;
    m_cache->UpdateDescriptorSetHashInfo(m_hash_info_head + m_index_of_binding[_binding] + 1, info);
}

void VulkanDescriptorSetWriter::WriteImage(uint16_t _set, uint16_t _binding, const VkDescriptorImageInfo& _image, uint32_t _count, VkDescriptorType _type) {
    const auto& image_info = m_cache->UpdateDescriptorImageInfo(_set, m_index_of_image[_binding], _image);

    VkWriteDescriptorSet write{};
    write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext            = nullptr;
    write.dstSet           = VK_NULL_HANDLE;
    write.dstBinding       = _binding;
    write.dstArrayElement  = 0;
    write.descriptorCount  = _count;
    write.descriptorType   = _type;
    write.pImageInfo       = &image_info;
    write.pBufferInfo      = nullptr;
    write.pTexelBufferView = nullptr;

    // update some vkDescriptorWrite info
    m_write_set[m_index_of_binding[_binding]] = std::move(write);
    VulkanHashableDescriptorInfo info;
    info.resource.image_view = _image;
    m_cache->UpdateDescriptorSetHashInfo(m_hash_info_head + m_index_of_binding[_binding] + 1, info);
}

void VulkanDescriptorSetWriter::WriteBuffer(uint16_t _set, uint16_t _binding, const VkDescriptorBufferInfo& _buffer, uint32_t _count, VkDescriptorType _type) {
    const auto& buffer_info = m_cache->UpdateDescriptorBufferInfo(_set, m_index_of_buffer[_binding], _buffer);

    VkWriteDescriptorSet write{};
    write.sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext            = nullptr;
    write.dstSet           = VK_NULL_HANDLE;
    write.dstBinding       = _binding;
    write.dstArrayElement  = 0;
    write.descriptorCount  = _count;
    write.descriptorType   = _type;
    write.pImageInfo       = nullptr;
    write.pBufferInfo      = &buffer_info;
    write.pTexelBufferView = nullptr;

    // update some vkDescriptorWrite info
    m_write_set[m_index_of_binding[_binding]] = std::move(write);
    VulkanHashableDescriptorInfo info;
    info.resource.buffer = _buffer;
    m_cache->UpdateDescriptorSetHashInfo(m_hash_info_head + m_index_of_binding[_binding] + 1, info);
}

uint32_t VulkanDescriptorSetWriter::GetSetKey() const {
    return crc32_8bytes(
        &m_cache->m_descriptor_resource_container.hashable_descriptor_infos[m_hash_info_head],
        sizeof(VulkanHashableDescriptorInfo) * (m_write_set.size() + 1));
}
