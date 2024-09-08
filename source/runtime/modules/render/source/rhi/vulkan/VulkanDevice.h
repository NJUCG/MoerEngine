#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H

#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "VulkanTypeDefs.h"
#include "VulkanExtension.h"
#include "VulkanDeviceFeature.h"
#include "VulkanDeviceProperty.h"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <optional>
#include "VulkanMacroUtils.h"
#include "VulkanCommand.h"
#include "../RHIImpl.h"
#include "taskgraph/Event.h"
#include "vulkan/vulkan_core.h"
namespace Moer::Render {

    // struct DeviceInitializer {
    //     VkInstance                   instance = nullptr;
    //     VkPhysicalDeviceType         gpu_type = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    //     VkSurfaceKHR                 surface;
    //     uint32_t                     api_version        = VK_API_VERSION_1_3;
    //     VulkanPhysicalDeviceFeatures enabled_features   = {};
    //     TVulkanDeviceExtensionArray  enabled_extensions = {};
    // };

    // class VulkanDevice {
    // public:
    //     VulkanDevice();
    //     ~VulkanDevice();

    //     void Init(const DeviceInitializer& _initializer);
    //     void InitMemoryAllocator(VkInstance _instance);
    //     void Destroy();

    //     operator VkDevice() const {
    //         return m_device;
    //     };

    //     inline VkPhysicalDevice GetGpu() const {
    //         return m_gpu;
    //     }
    //     inline VkDevice GetDevice() const {
    //         return m_device;
    //     }
    //     inline VmaAllocator GetVmaAllocator() const {
    //         return m_allocator;
    //     }
    //     inline VulkanDescriptorSetAllocator* GetDescriptorAllocator() const {
    //         return m_descriptor_allocator;
    //     }
    //     inline const VulkanEnabledDeviceExtensions& GetEnabledExtensions() const {
    //         return m_enabled_extensions;
    //     }
    //     inline const VulkanOptionalDeviceExtensions& GetOptionalExtensions() const {
    //         return m_optional_extensions;
    //     }
    //     inline const VulkanPhysicalDeviceFeatures& GetCoreFeatures() const {
    //         return m_core_features;
    //     }
    //     inline const VulkanPhysicalDeviceProperties& GetCoreProperties() const {
    //         return m_core_properties;
    //     }
    //     inline const VulkanOptionalDeviceProperties& GetOptionalProperties() const {
    //         return m_optional_properties;
    //     }
    //     inline VkPhysicalDeviceMemoryProperties GetMemoryProperties() const {
    //         return m_memery_properties;
    //     }
    //     inline QueueFamilyIndices GetQueueFamilyIndices() const {
    //         return m_queue_family_indices;
    //     }
    //     inline VkQueue GetGraphicsQueue() const {
    //         return m_graphics_queue;
    //     }
    //     inline VkQueue GetPresentQueue() const {
    //         return m_present_queue;
    //     }
    //     inline VkQueue GetComputeQueue() const {
    //         return m_compute_queue;
    //     }
    //     inline VkQueue GetTransferQueue() const {
    //         return m_transfer_queue;
    //     }
    //     inline VkQueue GetRayTracingQueue() const {
    //         return m_raytracing_queue;
    //     }
    //     class VulkanCommandAllocator* GetCurrentCommandAllocator();
    //     class VulkanStagingBuffer*    AquireStagingBuffer(uint64_t _byte_size);
    //     void                          ReleaseStagingBuffer(class VulkanStagingBuffer*);

    // private:
    //     VkPhysicalDevice                 m_gpu;
    //     VulkanEnabledDeviceExtensions    m_enabled_extensions;
    //     VulkanOptionalDeviceExtensions   m_optional_extensions;
    //     VulkanPhysicalDeviceFeatures     m_core_features;
    //     VulkanPhysicalDeviceProperties   m_core_properties;
    //     VulkanOptionalDeviceProperties   m_optional_properties;
    //     VkPhysicalDeviceMemoryProperties m_memery_properties;
    //     TQueueFamilyPropertiesArray      m_queue_family_props;
    //     QueueFamilyIndices               m_queue_family_indices;

    //     VkDevice m_device;
    //     VkQueue  m_graphics_queue;
    //     VkQueue  m_present_queue;
    //     VkQueue  m_compute_queue;
    //     VkQueue  m_raytracing_queue;
    //     VkQueue  m_transfer_queue;

    //     VmaAllocator                  m_allocator;
    //     VulkanDescriptorSetAllocator* m_descriptor_allocator;

    //     Moer::Array<class VulkanCommandAllocator*> m_command_allocators;

    //     struct StagingBufferPool;
    //     StagingBufferPool* m_staging_buffer_pool;

    // private:
    //     VkPhysicalDevice SelectGpu(const DeviceInitializer& _init);

    //     void InitGpu(const DeviceInitializer& _initializer);
    //     void CreateDevice(uint32_t _api_version);
    //     void CreateMemoryAllocator();
    //     void CreateDescriptorAllocator();
    //     void CreateCommandAllocators();

    //     TExtensionArray                  GetGpuExtensions(VkPhysicalDevice _gpu) const;
    //     VkPhysicalDeviceMemoryProperties GetMemoryProperties(VkPhysicalDevice _gpu) const;
    //     TQueueFamilyPropertiesArray      GetQueueFamilyProperties(VkPhysicalDevice _gpu) const;

    //     //    uint32_t                         GetMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties, VkBool32* mem_type_found = nullptr) const;
    //     int32_t            GetQueueFamilyIndex(const Moer::Array<VkQueueFamilyProperties>& queue_family_props, VkQueueFlags _queue_flags) const;
    //     QueueFamilyIndices QueryQueueFamilyIndices(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) const;

    //     bool CheckEnabledExtensionsSupported(VkPhysicalDevice _gpu, const TVulkanDeviceExtensionArray& _enabled_extensions) const;
    //     bool CheckEnabledFeaturesSupported(VkPhysicalDevice _gpu, const VulkanPhysicalDeviceFeatures& _enabled_features, uint32_t _api_version);

    //     void CreateStagingBufferPool();

    //     void DestroyStagingBufferPool();
    // };
}// namespace Moer::Render
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
    struct DeviceInitializer {
        VkInstance                   instance           = nullptr;
        VkPhysicalDeviceType         gpu_type           = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        uint32_t                     api_version        = VK_API_VERSION_1_3;
        VulkanPhysicalDeviceFeatures enabled_features   = {};
        TVulkanDeviceExtensionArray  enabled_extensions = {};
    };

    class VulkanDevice : public RenderDevice::Impl {
    public:
        VulkanDevice(DeviceInitInfo&& _info) : RenderDevice::Impl(std::move(_info)), m_gpu(VK_NULL_HANDLE), m_optional_extensions(), m_core_features(), m_core_properties(), m_optional_properties(), m_memery_properties(), m_queue_family_props(), m_queue_family_indices(),
                                               m_device(VK_NULL_HANDLE), m_graphics_queue(VK_NULL_HANDLE), m_present_queue(VK_NULL_HANDLE), m_compute_queue(VK_NULL_HANDLE), m_transfer_queue(VK_NULL_HANDLE),
                                               m_allocator(VK_NULL_HANDLE), m_descriptor_allocator(nullptr), init_event() {
            
            CreateInstance();
            DeviceInitializer initializer;
            initializer.instance = m_instance;
            initializer.api_version      = VK_API_VERSION_1_3;
            initializer.enabled_features = VulkanDeviceFeature::GetMESupportedDeviceFeatures(initializer.api_version);
            RHIInfo rhi_info             = {
                            .rhi_type            = ERHIType::Vulkan,
                            .max_frame_in_flight = 2,
                            .ray_tracing         = true};
            initializer.enabled_extensions = VulkanDeviceExtension::GetMESupportedDeviceExtensions(rhi_info);
            //Init Gpu device and create queues
            Init(initializer);
            InitMemoryAllocator(m_instance);
        }

        virtual ~VulkanDevice();
        PipelineHandle CreatePipeline(GfxPsoCreateInfo&& _pso_info, PipelineShaderInfo&& _shaders) override;
        PipelineHandle CreatePipeline(PipelineShaderInfo&& _shaders) override;

        TextureRef CreateTexture(Extent3D _size, EPixelFormat _format, ETextureUsageFlags _usage, uint32_t _mip_cnt, uint32_t _array_size) override;
        BufferRef  CreateBuffer(uint _element_cnt, uint _byte_stride, EBufferUsageFlags _usage) override;

        BufferRef CreateStagingBuffer(uint64 _byte_size) override;

        FenceRef CreateFence() override;

        // RHIViewportRef CreateViewport(const RHIViewportInitializer& _init) override;

        // BackBufferInfo GetNextBackBufferInfo(RHIViewport* _viewport) override;

        // void PresentViewport(RHIViewport* _viewport, RHIFence* _render_end_fence) override;

        CommandQueue& GetCommandQueue(EQueueType _type) override;

        SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) override;

    public:
        void EnqueueDeferredRelease(RHIResource* _object);
        void FlushDeferredReleases();
        constexpr uint ImmutableSamplerCount() const { return immutable_sampler_count; }
        const VkSampler* GetImmutableSamplers() const{ return immutable_samplers.data(); }
    public:
        void Init(const DeviceInitializer& _initializer);
        void InitMemoryAllocator(VkInstance _instance);
        void CreateInstance();
        void Destroy();

        inline VkPhysicalDevice GetGpu() const {
            return m_gpu;
        }
        inline VkInstance GetInstance() const {
            return m_instance;
        }
        inline VkDevice GetDevice() const {
            return m_device;
        }
        inline VmaAllocator GetVmaAllocator() const {
            return m_allocator;
        }
        inline VulkanDescriptorSetAllocator* GetDescriptorAllocator() const {
            return m_descriptor_allocator;
        }
        inline const VulkanEnabledDeviceExtensions& GetEnabledExtensions() const {
            return m_enabled_extensions;
        }
        inline const VulkanOptionalDeviceExtensions& GetOptionalExtensions() const {
            return m_optional_extensions;
        }
        inline const VulkanPhysicalDeviceFeatures& GetCoreFeatures() const {
            return m_core_features;
        }
        inline const VulkanPhysicalDeviceProperties& GetCoreProperties() const {
            return m_core_properties;
        }
        inline const VulkanOptionalDeviceProperties& GetOptionalProperties() const {
            return m_optional_properties;
        }
        inline VkPhysicalDeviceMemoryProperties GetMemoryProperties() const {
            return m_memery_properties;
        }
        inline QueueFamilyIndices GetQueueFamilyIndices() const {
            return m_queue_family_indices;
        }
        uint GetQueueFamilyIndex(VkQueueFlags _queue_flags) {
            return GetQueueFamilyIndex(m_queue_family_props, _queue_flags);
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
        VkInstance                       m_instance;
        VkPhysicalDevice                 m_gpu;
        VulkanEnabledDeviceExtensions    m_enabled_extensions;
        VulkanOptionalDeviceExtensions   m_optional_extensions;
        VulkanPhysicalDeviceFeatures     m_core_features;
        VulkanPhysicalDeviceProperties   m_core_properties;
        VulkanOptionalDeviceProperties   m_optional_properties;
        VkPhysicalDeviceMemoryProperties m_memery_properties;
        TQueueFamilyPropertiesArray      m_queue_family_props;
        QueueFamilyIndices               m_queue_family_indices;
        Moer::Array<std::string>         m_instance_layers;
        Moer::Array<std::string>         m_instance_extensions;
        Moer::Array<std::string>         m_enabled_instance_extensions;

        VkDevice m_device;
        VkQueue  m_graphics_queue;
        VkQueue  m_present_queue;
        VkQueue  m_compute_queue;
        VkQueue  m_raytracing_queue;
        VkQueue  m_transfer_queue;
        EventRef init_event;

        PFN_vkCreateDebugUtilsMessengerEXT  vk_create_debug_utils_messenger_ext  = VK_NULL_HANDLE;
        PFN_vkDestroyDebugUtilsMessengerEXT vk_destroy_debug_utils_messenger_ext = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT            debug_utils_messenger                = VK_NULL_HANDLE;
        PFN_vkCmdBeginDebugUtilsLabelEXT    vk_cmd_begin_debug_utils_label_ext   = VK_NULL_HANDLE;
        PFN_vkCmdEndDebugUtilsLabelEXT      vk_cmd_end_debug_utils_label_ext     = VK_NULL_HANDLE;
        PFN_vkCmdInsertDebugUtilsLabelEXT   vk_cmd_insert_debug_utils_label_ext  = VK_NULL_HANDLE;
        PFN_vkSetDebugUtilsObjectNameEXT    vk_set_debug_utils_object_name_ext   = VK_NULL_HANDLE;

        VmaAllocator                  m_allocator;
        VulkanDescriptorSetAllocator* m_descriptor_allocator;
        UniquePtr<VkCommandQueue>     gfx_queue;
        UniquePtr<VkCommandQueue>     compute_queue;
        UniquePtr<VkCommandQueue>     transfer_queue;
        LockFreeQueueBase<RHIResource, 64> deferred_release_queue;
        static constexpr uint immutable_sampler_count = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num) ;
        StaticArray<VkSampler, immutable_sampler_count> immutable_samplers;
    private:
        friend VkCommandQueue;
        //configs
        uint cmd_alloc_limits = 2;

    private:
        VkPhysicalDevice SelectGpu(const DeviceInitializer& _init);

        void InitGpu(const DeviceInitializer& _initializer);
        void CreateDevice(uint32_t _api_version);
        void CreateMemoryAllocator();
        void CreateDescriptorAllocator();

        TExtensionArray                  GetGpuExtensions(VkPhysicalDevice _gpu) const;
        VkPhysicalDeviceMemoryProperties GetMemoryProperties(VkPhysicalDevice _gpu) const;
        TQueueFamilyPropertiesArray      GetQueueFamilyProperties(VkPhysicalDevice _gpu) const;

        //    uint32_t                         GetMemoryType(uint32_t type_bits, VkMemoryPropertyFlags properties, VkBool32* mem_type_found = nullptr) const;
        int32_t            GetQueueFamilyIndex(const Moer::Array<VkQueueFamilyProperties>& queue_family_props, VkQueueFlags _queue_flags) const;
        QueueFamilyIndices QueryQueueFamilyIndices(VkPhysicalDevice _gpu) const;

        bool CheckEnabledExtensionsSupported(VkPhysicalDevice _gpu, const TVulkanDeviceExtensionArray& _enabled_extensions) const;
        bool CheckEnabledFeaturesSupported(VkPhysicalDevice _gpu, const VulkanPhysicalDeviceFeatures& _enabled_features, uint32_t _api_version);

#pragma region[ debug ]
        void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& _create_info);
        void SetupDebugUtilsMessengerEXT();
        void SetupDebugProcs();
#pragma endregion
    };
}// namespace Moer::Render
#endif// VULKAN_DEVICE_H
