#ifndef VULKAN_RHI_DESCRIPTOR_H
#define VULKAN_RHI_DESCRIPTOR_H

#include "rhi/vulkan/misc/VulkanTypeDefs.h"
#include "VulkanRHIResource.h"

#include <vulkan/vulkan.h>

#include <set>
#include <memory>

class VulkanDevice;

union DescriptorResource {
    VkDescriptorImageInfo  sampler;
    VkDescriptorImageInfo  image_view;
    VkDescriptorBufferInfo buffer;
};

union VulkanHashableDescriptorInfo {
    struct {
        uint64_t              max_0;
        uint64_t              max_1;
        VkDescriptorSetLayout layout_handle;
    } layout;

    DescriptorResource resource;
};

class VulkanDescriptorSetsLayout {
    struct DescriptorBindingInfo {
        VkDescriptorType type;
        uint32_t         count;
        uint32_t         hash_info_index;
    };
    using TDescriptorBindingInfo = std::unordered_map<uint8_t, std::unordered_map<uint32_t, DescriptorBindingInfo>>;

public:
    VulkanDescriptorSetsLayout() {}
    ~VulkanDescriptorSetsLayout();

    void Init(const std::vector<TDescriptorSetLayout>& _layout_mappings, VulkanPipelineResourceCache* _cache);

    inline uint32_t GetDescriptorSetCount() const {
        return m_layouts.size();
    }

    inline const std::vector<VkDescriptorSetLayout>& GetLayouts() const {
        return m_layouts;
    }

    inline const TDescriptorCountMap& GetSetsBindingCount() const {
        return m_sets_binding_count;
    }

    inline const TDescriptorBindingInfo& GetDescriptorBindingInfos() const {
        return m_descriptor_binding_infos;
    }

private:
    std::vector<VkDescriptorSetLayout> m_layouts;
    TDescriptorCountMap                m_sets_binding_count;
    TDescriptorBindingInfo             m_descriptor_binding_infos;
    // infos[space][slot] = {type, count, hash_info_index}
};

class VulkanDescriptorSetAllocator : public VulkanDeviceObject {
public:
    VulkanDescriptorSetAllocator();
    ~VulkanDescriptorSetAllocator();

    void Init(VulkanDevice* device);

    bool GetDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, std::vector<VkDescriptorSet>& _sets);

    bool AllocateDescriptorSet(const VulkanDescriptorSetsLayout& _layouts, VkDescriptorSet* _set);

    void ResetAll();

    void CleanUp();

    class VulkanDescriptorSetCachePool {
    public:
        VulkanDescriptorSetCachePool(const VulkanDescriptorSetsLayout& _layout);
        ~VulkanDescriptorSetCachePool();

        bool GetDescriptorSet(uint32_t _hash_key, VkDescriptorSet* _set);
        bool AllocateDescriptorSet(uint32_t _hash_key, VkDescriptorSet* _set);
        void Reset();
        void CleanUp();

    private:
        VkDescriptorPool m_pool;

        std::unordered_map<uint32_t, VkDescriptorSet> m_allocated_sets;
    };

private:
    std::list<std::unique_ptr<VulkanDescriptorSetCachePool>> m_cache_pools;

private:
    VkDescriptorPool GetAvailablePool(const VulkanDescriptorSetsLayout& _layouts);

    VkDescriptorPool CreatePool(const VulkanDescriptorSetsLayout& _layout);

    uint32_t GetMaxSets(uint32_t _set_count) const;
};

struct VulkanDescriptorSetWriteContainer {
    std::vector<VulkanHashableDescriptorInfo>                         hashable_descriptor_infos;
    std::vector<std::unordered_map<uint16_t, VkDescriptorImageInfo>>  descriptor_image_infos;
    std::vector<std::unordered_map<uint16_t, VkDescriptorBufferInfo>> descriptor_buffer_infos;
    std::vector<VkWriteDescriptorSet>                                 descriptor_writes;
};

class VulkanDescriptorSetWriter : public VulkanDeviceObject {
    friend VulkanDescriptorSetsLayout;

public:
    VulkanDescriptorSetWriter(VulkanPipelineResourceCache* _cache) : m_cache(_cache) {}

    void WriteSampler(uint16_t _binding, const VkDescriptorImageInfo& _sampler, const VkDescriptorSet& _set, uint32_t _count, VkDescriptorType _type);
    void WriteImage(uint16_t _binding, const VkDescriptorImageInfo& _image, const VkDescriptorSet& _set, uint32_t _count, VkDescriptorType _type);
    void WriteBuffer(uint16_t _binding, const VkDescriptorBufferInfo& _buffer, const VkDescriptorSet& _set, uint32_t _count, VkDescriptorType _type);

    uint32_t GetSetKey() const;

private:
    uint32_t m_hash_info_head;

    std::vector<VkWriteDescriptorSet>      m_write_set;
    std::unordered_map<uint16_t, uint32_t> m_index_of_binding;

    VulkanPipelineResourceCache* m_cache;

private:
    void UpdateSetHashInfo();
};

#endif