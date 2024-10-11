#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H

#include "taskgraph/Event.h"
#include "misc/STL.h"

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "../RHIImpl.h"

#include <volk.h>
#include "VulkanTypeDefs.h"
#include "VulkanExtension.h"
#include "VulkanDeviceFeature.h"
#include "VulkanDeviceProperty.h"
#include "VulkanDescriptor.h"
#include "VulkanCommand.h"
#include "vulkan/vulkan_core.h"

#include <vk_mem_alloc.h>

#include <optional>

namespace Moer::Render {
    class VulkanDescriptorSetsLayout;
    class VulkanDescriptorSetAllocator;
    class VulkanDescriptorSetWriter;

    struct QueueFamilyIndices {
        std::optional<uint32_t> graphics;
        std::optional<uint32_t> present;
        std::optional<uint32_t> transfer;
        std::optional<uint32_t> compute;
        std::optional<uint32_t> raytracing;

        inline bool IsComplete() const { return graphics.has_value() && present.has_value() && transfer.has_value() && compute.has_value() && raytracing.has_value(); }
        uint        Size() const {
            uint size = 0;
            if (graphics.has_value()) size++;
            if (present.has_value()) size++;
            if (transfer.has_value()) size++;
            if (compute.has_value()) size++;
            if (raytracing.has_value()) size++;
            return size;
        }
    };

    struct VulkanRHIConfig {
        uint32 api_version = VK_API_VERSION_1_3;
    };

    struct VulkanDeviceInfo {
        TVulkanDeviceExtensionArray      enabled_extensions{};
        VulkanOptionalDeviceExtensions   optional_extensions{};
        VulkanDeviceFeatures             core_features{};
        VulkanCoreDeviceProperties       core_properties{};
        VulkanOptionalDeviceProperties   optional_properties{};
        VkPhysicalDeviceMemoryProperties memery_properties{};
        TQueueFamilyPropertiesArray      queue_family_props{};
        QueueFamilyIndices               queue_family_indices{};
    };

    class VulkanDevice : public RenderDevice::Impl {
    public:
        VulkanDevice(const VulkanRHIConfig&& _config);
        virtual ~VulkanDevice();

        PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) override;
        PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders) override;

        TextureRef CreateTexture(Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint32_t _array_size) override;
        BufferRef  CreateBuffer(uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage) override;

        BindlessArrayRef CreateBindlessArray(uint _max_size) override;
        FenceRef         CreateFence() override;

        // Raytracing
        RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) override;

        RaytracingSceneRef CreateRaytracingScene() override;

        CommandQueue& GetCommandQueue(EQueueType _type) override;

        SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) override;

    public:
        void             EnqueueDeferredRelease(RHIResource* _object);
        void             FlushDeferredReleases();
        constexpr uint   ImmutableSamplerCount() const { return immutable_sampler_count; }
        const VkSampler* GetImmutableSamplers() const { return immutable_samplers.data(); }
        const VkSampler  GetSampler(Sampler _sampler) const;
        const uint       GetSamplerIdx(Sampler _sampler) const {
            uint filter  = uint(_sampler.filter);
            uint address = uint(_sampler.address_mode);
            uint compare = uint(_sampler.compare_function);
            return (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
        }

    public:
    public:
        inline VkPhysicalDevice GetGpu() const {
            return m_gpu;
        }
        inline VkInstance GetInstance() const {
            return m_instance;
        }
        inline VkDevice GetDevice() const {
            return m_device;
        }
        VulkanDescriptorHeap& GetGlobalDescriptorHeap() {
            return m_global_descriptor_heap;
        }
        inline VmaAllocator GetVmaAllocator() const {
            return m_allocator;
        }
        inline const VulkanOptionalDeviceExtensions& GetOptionalExtensions() const {
            return m_device_info.optional_extensions;
        }
        inline const VulkanDeviceFeatures& GetCoreFeatures() const {
            return m_device_info.core_features;
        }
        inline const VulkanCoreDeviceProperties& GetCoreProperties() const {
            return m_device_info.core_properties;
        }
        inline const VulkanOptionalDeviceProperties& GetOptionalProperties() const {
            return m_device_info.optional_properties;
        }
        inline VkPhysicalDeviceMemoryProperties GetMemoryProperties() const {
            return m_device_info.memery_properties;
        }
        inline QueueFamilyIndices GetQueueFamilyIndices() const {
            return m_device_info.queue_family_indices;
        }
        inline VkDescriptorSetLayout GetEmptyDescriptorSetLayout() const {
            return empty_descriptor_set_layout;
        }
        uint GetQueueFamilyIndex(VkQueueFlags _queue_flags) {
            return GetQueueFamilyIndex(m_device_info.queue_family_props, _queue_flags);
        }

        inline VkQueue GetGraphicsQueue() const {
            return m_graphics_queue;
        }
        inline VkQueue GetPresentQueue() const {
            return m_present_queue;
        }
        inline VkQueue GetComputeQueue() const {
            return m_compute_queue;
        }
        inline VkQueue GetTransferQueue() const {
            return m_transfer_queue;
        }
        inline VkQueue GetRayTracingQueue() const {
            return m_raytracing_queue;
        }

        VulkanCommandAllocator& GetCurrentCommandAllocator();

    private:
        Array<VulkanCommandAllocator> m_command_allocators;

    private:
        VkInstance            m_instance                  = VK_NULL_HANDLE;
        VkPhysicalDevice      m_gpu                       = VK_NULL_HANDLE;
        VkDevice              m_device                    = VK_NULL_HANDLE;
        VkQueue               m_graphics_queue            = VK_NULL_HANDLE;
        VkQueue               m_present_queue             = VK_NULL_HANDLE;
        VkQueue               m_compute_queue             = VK_NULL_HANDLE;
        VkQueue               m_raytracing_queue          = VK_NULL_HANDLE;
        VkQueue               m_transfer_queue            = VK_NULL_HANDLE;
        VkDescriptorSetLayout empty_descriptor_set_layout = VK_NULL_HANDLE;
        EventRef              init_event{};

        VulkanDeviceInfo m_device_info{};

        VkDebugUtilsMessengerEXT m_debug_utils_messenger = VK_NULL_HANDLE;

        VmaAllocator                                    m_allocator = VK_NULL_HANDLE;
        VulkanDescriptorHeap                            m_global_descriptor_heap{};
        UniquePtr<VkCommandQueue>                       gfx_queue{};
        UniquePtr<VkCommandQueue>                       compute_queue{};
        UniquePtr<VkCommandQueue>                       transfer_queue{};
        LockFreeQueueBase<RHIResource, false, 64>       deferred_release_queue{};
        static constexpr uint                           immutable_sampler_count = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
        StaticArray<VkSampler, immutable_sampler_count> immutable_samplers{};

    public:
        static constexpr uint bindless_sampler_cnt = 256;
        static constexpr uint cmd_alloc_limits     = 3;
        DeviceInternalShaders internal_shaders;

    private:
        friend VkCommandQueue;
        //configs

    private:
        void             InitVulkanInstance(uint32 _api_version);
        VkPhysicalDevice SelectGpu(uint32 _api_version);
        void             InitGpu(uint32 _api_version);
        void             CreateDevice(uint32 _api_version);
        void             CreateMemoryAllocator(VkInstance _instance, uint32 _api_version);
        void             CreateDescriptorHeap();
        void             DestroyDescriptorHeap();
        void             CreateInternalResources();
        void             DestroyInternalResources();
        void             CreateImmutableSamplers();
        void             DestroyImmutableSamplers();
        void             PostInit() override;

        void CreateInternalShaders();

        void Destroy();

        static Set<std::string>            GetGpuExtensions(VkPhysicalDevice _gpu);
        static TQueueFamilyPropertiesArray GetQueueFamilyProperties(VkPhysicalDevice _gpu);

        int32_t            GetQueueFamilyIndex(const Moer::Array<VkQueueFamilyProperties>& queue_family_props, VkQueueFlags _queue_flags) const;
        QueueFamilyIndices QueryQueueFamilyIndices(VkPhysicalDevice _gpu) const;

#pragma region[ debug ]
        void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& _create_info);
        void SetupDebugUtilsMessengerEXT();
#pragma endregion
    };
}// namespace Moer::Render
#endif// VULKAN_DEVICE_H
