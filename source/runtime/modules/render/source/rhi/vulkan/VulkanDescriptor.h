#ifndef VULKAN_RHI_DESCRIPTOR_H
#define VULKAN_RHI_DESCRIPTOR_H

#define VK_DESCRIPTOR_TYPE_BEGIN_RANGE (VK_DESCRIPTOR_TYPE_SAMPLER)
#define VK_DESCRIPTOR_TYPE_END_RANGE   (VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
#define VK_DESCRIPTOR_TYPE_RANGE_SIZE  3

#include "rhi/vulkan/misc/VulkanTypeDefs.h"
#include "VulkanRHIResource.h"

#include <vulkan/vulkan.h>

class VulkanDevice;
class VulkanDescriptorSetWriter;

union DescriptorResource {
    VkDescriptorImageInfo      sampler;
    VkDescriptorImageInfo      image_view;
    VkDescriptorBufferInfo     buffer;
    VkAccelerationStructureKHR as;
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
    };
    using TDescriptorBindingInfo = Moer::UnorderedMap<uint8_t, Moer::UnorderedMap<uint32_t, DescriptorBindingInfo>>;

public:
    VulkanDescriptorSetsLayout()  = default;
    ~VulkanDescriptorSetsLayout() = default;

    void Init(const Moer::Array<TDescriptorSetLayoutInfo>& _layout_mappings, VulkanPipelineResourceCache* _cache);

    inline uint32_t GetDescriptorSetCount() const {
        return m_layouts.size();
    }

    inline const Moer::Array<VkDescriptorSetLayout>& GetLayouts() const {
        return m_layouts;
    }

    inline const TDescriptorCountMap& GetSetsBindingCount() const {
        return m_sets_binding_count;
    }

    inline const TDescriptorBindingInfo& GetDescriptorBindingInfos() const {
        return m_descriptor_binding_infos;
    }

private:
    Moer::Array<VkDescriptorSetLayout> m_layouts;
    TDescriptorCountMap                m_sets_binding_count;
    TDescriptorBindingInfo             m_descriptor_binding_infos;
    // infos[space][slot] = {type, count}
};

class VulkanDescriptorSetAllocator : public VulkanDeviceObject {
public:
    VulkanDescriptorSetAllocator();
    ~VulkanDescriptorSetAllocator();

    void Init(VulkanDevice* device);

    bool GetDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, Moer::Array<VulkanDescriptorSetWriter>& _writers, Moer::Array<VkDescriptorSet>& _sets);

    void ResetAll();

    void CleanUp();

    class VulkanDescriptorSetCachePool : public VulkanDeviceObject {
    public:
        VulkanDescriptorSetCachePool(VulkanDevice* _device, const float _default_pool_size[VK_DESCRIPTOR_TYPE_RANGE_SIZE], uint32_t _set_count);
        VulkanDescriptorSetCachePool(VulkanDevice* _device, const VulkanDescriptorSetsLayout& _layout);
        ~VulkanDescriptorSetCachePool();

        bool FindDescriptorSets(uint32_t _hash_key, Moer::Array<VkDescriptorSet>& _sets);
        bool CreateDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, Moer::Array<VulkanDescriptorSetWriter>& _writers, Moer::Array<VkDescriptorSet>& _sets);
        bool AllocateDescriptorSet(VkDescriptorSetLayout _layout, VkDescriptorSet& _set);
        void Reset();
        void CleanUp();

    private:
        VkDescriptorPool m_pool;

        Moer::UnorderedMap<uint32_t, Moer::Array<VkDescriptorSet>> m_allocated_sets;
        Moer::UnorderedMap<uint32_t, VkDescriptorSet>              m_allocated_set;

    private:
        static uint32_t GetMaxSets(uint32_t _set_count);
    };

private:
    std::list<std::unique_ptr<VulkanDescriptorSetCachePool>> m_cache_pools;

private:
    void CreatePool(const VulkanDescriptorSetsLayout& _layout);
};

struct VulkanDescriptorSetWriteContainer {
    Moer::Array<VulkanHashableDescriptorInfo>            hashable_descriptor_infos;
    Moer::Array<Moer::Array<VkDescriptorImageInfo>>      descriptor_image_infos;
    Moer::Array<Moer::Array<VkDescriptorBufferInfo>>     descriptor_buffer_infos;
    Moer::Array<Moer::Array<VkAccelerationStructureKHR>> descriptor_as_infos;
    // Moer::Array<VkWriteDescriptorSet>         descriptor_writes;
};

class VulkanDescriptorSetWriter : public VulkanDeviceObject {
    friend VulkanDescriptorSetsLayout;

public:
    VulkanDescriptorSetWriter(VulkanPipelineResourceCache* _cache) : m_cache(_cache) {}

    void SetDescriptorSet(VkDescriptorSet _set);

    void WriteSampler(uint16_t _set, uint16_t _binding, const Moer::Array<VkDescriptorImageInfo>& _sampler, VkDescriptorType _type);
    void WriteImage(uint16_t _set, uint16_t _binding, const Moer::Array<VkDescriptorImageInfo>& _image, VkDescriptorType _type);
    void WriteBuffer(uint16_t _set, uint16_t _binding, const Moer::Array<VkDescriptorBufferInfo>& _buffer, VkDescriptorType _type);
    void WriteAS(uint16_t _set, uint16_t _binding, const Moer::Array<VkAccelerationStructureKHR>& _as, VkDescriptorType _type);

    uint32_t GetSetKey() const;

    inline uint32_t                    GetNumWrites() const { return m_write_set.size(); }
    inline const VkWriteDescriptorSet* GetWrites() const { return m_write_set.data(); }

private:
    uint32_t m_hash_info_head;
    uint32_t m_hash_info_count;

    Moer::Array<VkWriteDescriptorSet> m_write_set;
    Moer::Array<VkWriteDescriptorSetAccelerationStructureKHR> m_write_set_as;
    // <binding, index of write set>
    Moer::UnorderedMap<uint16_t, uint32_t> m_index_of_binding;
    // <binding, index of hashable info>
    Moer::UnorderedMap<uint16_t, uint32_t> m_index_of_hash_info;
    // <binding, index of image>
    Moer::UnorderedMap<uint16_t, uint32_t> m_index_of_image;
    // <binding, index of buffer>
    Moer::UnorderedMap<uint16_t, uint32_t> m_index_of_buffer;
    // <bining, index of as info>
    Moer::UnorderedMap<uint16_t, uint32_t> m_index_of_as;

    VulkanPipelineResourceCache* m_cache;

private:
    void UpdateSetHashInfo();
};

#endif