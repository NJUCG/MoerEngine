#include "VulkanDevice.h"
#include "VulkanDescriptor.h"

#include "rhi/vulkan/misc/VulkanMacroUtils.h"

VulkanDescriptorAllocator::~VulkanDescriptorAllocator() {
    // Destructor implementation
    CleanUp();
}

void VulkanDescriptorAllocator::Init(VulkanDevice* device) {
    this->m_device = device;
}

bool VulkanDescriptorAllocator::Allocate(VkDescriptorSet& _set, const VulkanDescriptorSetsLayout& _layout) {
    if (m_current_pool == VK_NULL_HANDLE) {
        m_current_pool = GetAvailablePool(_layout);
        m_used_pools.push_back(m_current_pool);
    }

    VkDescriptorSetAllocateInfo alloc_info{};
    alloc_info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    alloc_info.pNext              = nullptr;
    alloc_info.descriptorPool     = m_current_pool;
    alloc_info.descriptorSetCount = _layout.GetDescriptorSetCount();
    alloc_info.pSetLayouts        = _layout.GetLayouts().data();

    auto result          = vkAllocateDescriptorSets(m_device->GetDevice(), &alloc_info, &_set);
    bool need_reallocate = false;
    switch (result) {
        case VK_SUCCESS:
            //all good, return
            return true;
        case VK_ERROR_FRAGMENTED_POOL:
        case VK_ERROR_OUT_OF_POOL_MEMORY:
            //reallocate pool
            need_reallocate = true;
            break;
        default:
            //unrecoverable error
            return false;
    }

    if (need_reallocate) {
        m_current_pool = GetAvailablePool(_layout);
        m_used_pools.push_back(m_current_pool);

        VK_CHECK_RESULT(vkAllocateDescriptorSets(m_device->GetDevice(), &alloc_info, &_set));
    }

    return true;
}

void VulkanDescriptorAllocator::ResetAll() {
    for (auto& pool : m_used_pools) {
        vkResetDescriptorPool(m_device->GetDevice(), pool, 0);
        m_free_pools.push_back(pool);
    }

    m_used_pools.clear();
    m_current_pool = VK_NULL_HANDLE;
}

void VulkanDescriptorAllocator::CleanUp() {
    for (const auto& pool : m_free_pools) {
        vkDestroyDescriptorPool(m_device->GetDevice(), pool, nullptr);
    }
    for (const auto& pool : m_used_pools) {
        vkDestroyDescriptorPool(m_device->GetDevice(), pool, nullptr);
    }
    m_current_pool = VK_NULL_HANDLE;
}

VkDescriptorPool VulkanDescriptorAllocator::GetAvailablePool(const VulkanDescriptorSetsLayout& _layout) {
    if (m_free_pools.empty()) {
        return CreatePool(_layout);
    }

    VkDescriptorPool pool = m_free_pools.back();
    m_free_pools.pop_back();

    return pool;
}

VkDescriptorPool VulkanDescriptorAllocator::CreatePool(const VulkanDescriptorSetsLayout& _layout) {
    TDescriptorMap pool_size_info;
    const uint32_t set_count = _layout.GetDescriptorSetCount();

    for (const auto& [type, count] : _layout.GetBindingCount()) {
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

uint32_t VulkanDescriptorAllocator::GetMaxSets(uint32_t _set_count) const {
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
