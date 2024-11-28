#ifndef VULKAN_RHI_DESCRIPTOR_H
#define VULKAN_RHI_DESCRIPTOR_H

#include <volk.h>
#include "rhi/RHIResource.h"
#include "VulkanTypeDefs.h"
#include "VulkanRHIResource.h"
#include "spirv_reflect.h"

#define VK_DESCRIPTOR_TYPE_BEGIN_RANGE (VK_DESCRIPTOR_TYPE_SAMPLER)
#define VK_DESCRIPTOR_TYPE_END_RANGE   (VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)
#define VK_DESCRIPTOR_TYPE_RANGE_SIZE  3

#include <mutex>

namespace Moer::Render {
    struct DescriptorSetInfo;
    struct DescriptorSetBindingInfo;

    class VulkanDevice;
    class VulkanDescriptorSetWriter;

    struct VulkanDescriptorASInfo {
        VkAccelerationStructureKHR as;
        uint64_t                   update_bit;
        uint64_t                   padding;
    };

    union DescriptorResource {
        VkDescriptorImageInfo  image;
        VkDescriptorBufferInfo buffer;
        VulkanDescriptorASInfo as;
    };

    union VulkanHashableDescriptorInfo {
        struct {
            uint64_t              max_0;
            uint64_t              max_1;
            VkDescriptorSetLayout layout_handle;
        } layout;

        DescriptorResource resource;
    };

    struct VulkanShaderResourceState {
        SpvReflectDescriptorType desc_type;
        uint                     b_sampled : 8;
        SpvReflectResourceType   resource_type : 8;

        VulkanShaderResourceState() = default;
        VulkanShaderResourceState(SpvReflectDescriptorType _type, SpvReflectResourceType _resource_type) : desc_type(_type), resource_type(_resource_type), b_sampled(0) {}

        VulkanShaderResourceState(uint64 _value) {
            memcpy(this, &_value, sizeof(VulkanShaderResourceState));
        }
        uint64 operator()() const {
            uint64 value;
            memcpy(&value, this, sizeof(VulkanShaderResourceState));
            return value;
        }
    };

    class VulkanDescriptorSetsLayout final : public VulkanDeviceObject {
    public:
        VulkanDescriptorSetsLayout(VulkanDevice* _device, const Moer::Array<TDescriptorSetLayoutBindingArray>& _descriptor_bindings);

        ~VulkanDescriptorSetsLayout();

        inline uint32_t GetDescriptorSetCount() const {
            return m_layouts.size();
        }

        inline const Moer::Array<VkDescriptorSetLayout>& GetLayouts() const {
            return m_layouts;
        }

        inline const TDescriptorCountMap& GetDescriptorTypeCount() const {
            return m_descriptor_type_count;
        }

    private:
        Moer::Array<VkDescriptorSetLayout> m_layouts;
        TDescriptorCountMap                m_descriptor_type_count;
    };

    class VulkanDescriptorSetAllocator final : public VulkanDeviceObject {
    public:
        VulkanDescriptorSetAllocator() = default;
        VulkanDescriptorSetAllocator(VulkanDevice* _device);
        ~VulkanDescriptorSetAllocator();

        bool GetDescriptorSets(uint32_t _hash_key, const VulkanDescriptorSetsLayout& _layout, Moer::Array<VulkanDescriptorSetWriter>& _writers, Moer::Array<VkDescriptorSet>& _sets);

        void ResetAll();

        void CleanUp();

        class VulkanDescriptorSetCachePool final : public VulkanDeviceObject {
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
            void InitBindlessPool();

        private:
            VkDescriptorPool m_pool          = VK_NULL_HANDLE;
            VkDescriptorPool m_bindless_pool = VK_NULL_HANDLE;

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
        Moer::Array<VulkanHashableDescriptorInfo> hashable_descriptor_info;
        Moer::Array<VkDescriptorImageInfo>        descriptor_image_info;
        Moer::Array<VkDescriptorBufferInfo>       descriptor_buffer_info;
        Moer::Array<VulkanDescriptorASInfo>       descriptor_as_info;

        Moer::Array<VkWriteDescriptorSet>                         descriptor_writes;
        Moer::Array<VkWriteDescriptorSetAccelerationStructureKHR> as_writes;
    };

    class VulkanDescriptorSetWriter final {
        friend VulkanPipelineResourceCache;

    public:
        VulkanDescriptorSetWriter(VulkanPipelineResourceCache* _cache) : m_cache(_cache) {}

        void Init(const Moer::Array<DescriptorSetBindingInfo>& _types, VulkanHashableDescriptorInfo* _hash_info_head, VkWriteDescriptorSet* _descriptor_write_head, VkDescriptorImageInfo* _image_info_head, VkDescriptorBufferInfo* _buffer_info_head, VkWriteDescriptorSetAccelerationStructureKHR* _as_write_head, VulkanDescriptorASInfo* _as_info_head);

        void SetDescriptorSet(VkDescriptorSet _set);

        void WriteSampler(uint32_t _binding, VkSampler _sampler, VkImageView _image_view, VkImageLayout _image_layout);
        void WriteSampledImage(uint32_t _binding, VkSampler _sampler, VkImageView _image_view, VkImageLayout _image_layout);
        void WriteStorageImage(uint32_t _binding, VkImageView _image_view, VkImageLayout _image_layout);
        void WriteUniformBuffer(uint32_t _binding, VkBuffer _buffer, VkDeviceSize _offset, VkDeviceSize _range);
        void WriteStorageBuffer(uint32_t _binding, VkBuffer _buffer, VkDeviceSize _offset, VkDeviceSize _range);
        void WriteAccelerationStructure(uint32_t _binding, VkAccelerationStructureKHR _as, uint64_t _update_bit);

        uint32_t GetSetKey() const;

        inline uint32_t                    GetNumWrites() const { return m_write_count; }
        inline const VkWriteDescriptorSet* GetWrites() const { return m_descriptor_write_head; }

    protected:
        template<VkDescriptorType DescriptorType>
        void WriteImageInner(uint32_t _write_index, VkSampler _sampler, VkImageView _image_view, VkImageLayout _image_layout);
        void WriteImage(uint32_t _binding, VkSampler _sampler, VkImageView _image_view, VkImageLayout _image_layout);
        void WriteBuffer(uint32_t _binding, VkBuffer _buffer, VkDeviceSize _offset, VkDeviceSize _range);
        template<VkDescriptorType DescriptorType>
        void WriteBufferInner(uint32_t _write_index, VkBuffer _buffer, VkDeviceSize _offset, VkDeviceSize _range);

    private:
        VulkanHashableDescriptorInfo* m_hash_info_head;
        VkWriteDescriptorSet*         m_descriptor_write_head;
        uint32_t                      m_write_count;

        // mapping: binding --> write index
        Moer::UnorderedMap<uint32_t, uint32_t> m_write_index_map;

        VulkanPipelineResourceCache* m_cache;
    };

#pragma region[ descriptor buffer ext ]

    struct VulkanDescritporSetLayout {
        VkDescriptorSetLayout layout;
        uint                  size;
        uint                  offset;
    };
    struct BufferCpuDescHandle {
        uint        data[4];
        void*       Data() { return data; }
        const void* Data() const { return data; }
    };
    struct ImageCpuDescHandle {
        uint data[4];
    };
    struct VulkanDescriptorHeap {
        VulkanBuffer* buffer_desc_buffer = nullptr;
        VulkanBuffer* image_desc_buffer  = nullptr;

    public:
        VulkanDescriptorHeap() = default;
        VulkanDescriptorHeap(VulkanDevice& _device);
        ~VulkanDescriptorHeap();

        Array<byte> buffer_desc_data;
        Array<byte> image_desc_data;
        Array<byte> accel_desc_data;

        Array<uint> buffer_free_list;
        Array<uint> image_free_list;
        Array<uint> accel_free_list;

        uint64 buffer_offset;
        uint64 image_offset;
        uint64 accel_offset;

        uint GetBufferDescIdx(const BufferView& _in_buffer);
        void FreeBufferDescIdx(uint _idx);
        uint GetImageDescIdx(const TextureView* _in_image, VkImageLayout _layout);
        void FreeImageDescIdx(uint _idx);
        uint GetSamplerDescIdx(Sampler _sampler);
        uint GetAccelDescIdx(VulkanAccelerationStructure* _as);
        uint FreeAccelDescIdx(uint _idx);

    public:
        uint64 CurrentFrameOffset(uint _frame_idx) const;
        void   BeginPushDescriptors(uint _frame_idx);

        void EndPushDescriptors(uint _frame_idx);

        // void PushBufferDesc(uint64 _src_offset, uint64 _set_offset);
        void PushUniformDesc(uint64 _src_offset, uint64 _set_offset);
        void PushStorageDesc(uint64 _src_offset, uint64 _set_offset);
        void PushImageDesc(uint64 _src_offset, uint64 _set_offset);
        void PushSamplerDesc(uint64 _src_offset, uint64 _set_offset);
        void PushAccelDesc(uint64 _src_offset, uint64 _set_offset);

        void IncrementOffset(uint64 _size);

    public:
        VulkanDevice* m_device;

        VulkanBuffer* ring_desc_buffer;

        uint storage_desc_stride;
        uint uniform_desc_stride;
        uint buffer_desc_stride;

        uint image_desc_stride;
        uint sample_desc_stride;
        uint accel_desc_stride;

        std::mutex m_mutex;
        uint64     texture_desc_offset;

        uint          ring_buffer_cnt;
        uint64        ring_buffer_size;
        Array<uint64> ring_buffer_offsets;
        uint64        current_offset;
        uint8*        map_ptr;
    };

    struct DescriptorBufferManager {
        uint desc_buffer_offset_alignment;
    };
#pragma endregion
}// namespace Moer::Render

#endif