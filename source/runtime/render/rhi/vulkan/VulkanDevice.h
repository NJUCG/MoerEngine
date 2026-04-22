#ifndef VULKAN_DEVICE_H
#define VULKAN_DEVICE_H

#include "PixelFormat.h"
#include "misc/STL.h"
#include "taskgraph/Event.h"

#include "../RHIImpl.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include "VulkanCommand.h"
#include "VulkanDescriptor.h"
#include "VulkanDeviceFeature.h"
#include "VulkanDeviceProperty.h"
#include "VulkanPlatform.h"
#include "VulkanQueue.h"
#include "VulkanTypeDefs.h"
#include "vulkan/vulkan_core.h"
#include "vulkanextension/VulkanExtension.h"

// #include <vk_mem_alloc.h>
#include "VulkanMemoryAllocator.h"

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

    inline bool IsComplete() const {
        return graphics.has_value() && present.has_value() && transfer.has_value() && compute.has_value() &&
               raytracing.has_value();
    }
    uint Size() const {
        uint size = 0;
        if (graphics.has_value())
            size++;
        if (present.has_value())
            size++;
        if (transfer.has_value())
            size++;
        if (compute.has_value())
            size++;
        if (raytracing.has_value())
            size++;
        return size;
    }
};

struct VulkanRHIConfig {
    uint32 api_version = VK_API_VERSION_1_3;
};

struct VulkanDeviceInfo {
    TVulkanDeviceExtensionArray      enabled_extensions{};
    VulkanOptionalDeviceExtensions   optional_extensions{};
    bool                             has_surface_maintenance1_instance{false};
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

    TextureRef CreateTexture(
        std::string_view   _name,
        ETextureDimension  _dimension,
        Extent3D           _size,
        EPixelFormat       _format,
        ETextureUsageFlags _usage,
        uint32_t           _mip_cnt,
        uint               _array_size
    ) override;

    BufferRef CreateBuffer(
        std::string_view  _name,
        uint              _element_cnt,
        uint              _byte_stride,
        EBufferUsageFlags _usage,
        EPixelFormat      _format
    ) override;

    BindlessArrayRef CreateBindlessArray(uint _max_size) override;
    FenceRef         CreateFence() override;

    // Raytracing
    RaytracingGeometryRef CreateRaytracingGeometry(const RaytracingGeometryInfo& _init) override;

    RaytracingSceneRef CreateRaytracingScene() override;

    CommandQueue& GetCommandQueue(EQueueType _type) override;

    CopyQueue& GetCopyQueue() override;

    SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) override;

    IOInterfaceRef CreateIOInterface(CopyQueue& _copy_queue) override;

    void FlushDebugMessages() const override;

    // Cooperative support related
    bool                            IsExtensionCooperativeEnabled() const override;
    const CooperativeExtensionInfo& GetCooperativeExtensionInfo() const override;
    bool                            TryConvertCooperativeVectorMatrix(
        const CooperativeVectorConversionDesc& _desc,
        std::span<const byte>                  _src_data,
        std::span<byte>                        _dst_data
    ) const override;

    // wait idle
    void WaitIdle() override;

    // 判断当前物理设备是否为 AMD（基于 vendorID）
    bool IsAmdGpu() const;

public:
    void           EnqueueDeferredRelease(RHIResource* _object);
    void           FlushDeferredReleases();
    constexpr uint ImmutableSamplerCount() const {
        return immutable_sampler_count;
    }
    const VkSampler* GetImmutableSamplers() const {
        return immutable_samplers.data();
    }
    const VkSampler GetSampler(Sampler _sampler) const;
    const uint      GetSamplerIdx(Sampler _sampler) const {
        uint filter  = uint(_sampler.filter);
        uint address = uint(_sampler.address_mode);
        uint compare = uint(_sampler.compare_function);
        return (uint(SF_Num) * uint(SAM_Num)) * compare + (uint(SF_Num)) * address + filter;
    }

    void SetResourceName(uint64 _object, VkObjectType _object_type, const std::string_view _name);
    void CopyData(const BufferView& _dst, const void* _data, uint64 _size);
    void CopyData(void* _dst, const BufferView& _src, uint64 _size);

public:
    RuntimePlugin* LoadPlugin(std::string_view _name) override;

    struct Ext {
        using Ctor = std::function<RuntimePlugin*(VulkanDevice*)>;
        using Dtor = std::function<void(RuntimePlugin*)>;
        RuntimePlugin* ext;
        Ctor           ctor;
        Dtor           dtor;
        Ext(Ctor ctor, Dtor dtor) : ext{nullptr}, ctor{ctor}, dtor{dtor} {}
        Ext(Ext const&) = delete;
        Ext(Ext&& rhs) : ext{rhs.ext}, ctor{rhs.ctor}, dtor{rhs.dtor} {
            rhs.ext = nullptr;
        }
        ~Ext() {
            if (ext) {
                dtor(ext);
            }
        }
    };

private:
    std::mutex                     ext_mutex;
    UnorderedMap<std::string, Ext> exts;
    CooperativeExtensionInfo       m_cooperative_extension_info{};

    void LoadDefaultExtensions();

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
        assert(m_global_descriptor_heap != nullptr && "Global descriptor heap is not initialized");
        return *m_global_descriptor_heap;
    }
    inline VmaAllocator GetVmaAllocator() const {
        return m_allocator;
    }
    inline const VulkanOptionalDeviceExtensions& GetOptionalExtensions() const {
        return m_device_info.optional_extensions;
    }
    inline bool HasDescriptorHeapRuntime() const {
        return m_device_info.optional_extensions.m_has_descriptor_heap_runtime;
    }
    VkResult WriteResourceDescriptors(
        uint32                             _descriptor_count,
        const VkResourceDescriptorInfoEXT* _descriptor_infos,
        const VkHostAddressRangeEXT*       _dst_ranges
    ) const;
    VkResult WriteSamplerDescriptors(
        uint32                       _descriptor_count,
        const VkSamplerCreateInfo*   _sampler_infos,
        const VkHostAddressRangeEXT* _dst_ranges
    ) const;
    void CmdBindResourceHeap(VkCommandBuffer _command_buffer, const VkBindHeapInfoEXT* _bind_info) const;
    void CmdBindSamplerHeap(VkCommandBuffer _command_buffer, const VkBindHeapInfoEXT* _bind_info) const;
    void CmdPushData(VkCommandBuffer _command_buffer, const VkPushDataInfoEXT* _push_info) const;
    uint64 GetPhysicalDescriptorSize(VkDescriptorType _type) const;
    bool SupportsTimestampQueries(EQueueType _type) const;
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
    // 查询当前设备是否启用了指定的 device extension（基于已启用扩展列表）
    bool HasDeviceExtension(std::string_view _ext_name) const;
    uint GetQueueFamilyIndex(VkQueueFlags _queue_flags) {
        return GetQueueFamilyIndex(m_device_info.queue_family_props, _queue_flags);
    }

    uint GetQueueFamilyIndex(EQueueType _queue_type) const;

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

    VmaAllocator                              m_allocator = VK_NULL_HANDLE;
    UniquePtr<VulkanDescriptorHeap>           m_global_descriptor_heap{};
    UniquePtr<VkCommandQueue>                 gfx_queue{}; // VkCommandQueue是MoerEngine的封装！
    UniquePtr<VkCommandQueue>                 compute_queue{};
    UniquePtr<VkCopyQueue>                    copy_queue{};
    // 这个锁只在AMD GPU上使用，因为AMD GPU没有TransferQueue
    // 在现代NVIDIA GPU上，这个锁不会被触发，接近0开销，不用在意性能
    std::mutex                                m_shared_queue_submit_mutex;
    LockFreeQueueBase<RHIResource, false, 64> deferred_release_queue{};
    static constexpr uint immutable_sampler_count = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
    StaticArray<VkSampler, immutable_sampler_count> immutable_samplers{};
    PFN_vkCmdBindResourceHeapEXT               m_vk_cmd_bind_resource_heap{nullptr};
    PFN_vkCmdBindSamplerHeapEXT                m_vk_cmd_bind_sampler_heap{nullptr};
    PFN_vkWriteResourceDescriptorsEXT          m_vk_write_resource_descriptors{nullptr};
    PFN_vkWriteSamplerDescriptorsEXT           m_vk_write_sampler_descriptors{nullptr};
    PFN_vkCmdPushDataEXT                       m_vk_cmd_push_data{nullptr};

public:
    static constexpr uint            bindless_sampler_cnt = 256;
    static constexpr uint            cmd_alloc_limits     = 3;
    UniquePtr<DeviceInternalShaders> internal_shaders;

private:
    friend VkCommandQueue;
    //configs

private:
    void             InitVulkanInstance(uint32 _api_version);
    VkPhysicalDevice SelectGpu(uint32 _api_version);
    void             InitGpu(uint32 _api_version);
    void             CreateDevice(uint32 _api_version);
    void             CreateMemoryAllocator(VkInstance _instance, uint32 _api_version);
    void             FinalizeDescriptorHeapRuntimeCapability();
    void             LoadDescriptorHeapRuntimeCommands();
    bool             ValidateDescriptorHeapRuntimeCommands() const;
    void             CreateDescriptorHeap();
    void             DestroyDescriptorHeap();
    void             CreateInternalResources();
    void             DestroyInternalResources();
    void             CreateImmutableSamplers();
    void             DestroyImmutableSamplers();
    void             PostInit() override;

    void CreateInternalShaders();
    void DestroyInternalShaders();

    void Destroy();

    static Set<std::string> GetGpuExtensions(VkPhysicalDevice _gpu, const char* _layer_name = nullptr);
    static TQueueFamilyPropertiesArray GetQueueFamilyProperties(VkPhysicalDevice _gpu);

    int32_t GetQueueFamilyIndex(
        const Moer::Array<VkQueueFamilyProperties>& queue_family_props,
        VkQueueFlags                                _queue_flags
    ) const;
    QueueFamilyIndices QueryQueueFamilyIndices(VkPhysicalDevice _gpu) const;

#pragma region[ debug ]
    void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& _create_info);
    void SetupDebugUtilsMessengerEXT();
#pragma endregion
};
} // namespace Moer::Render
#endif // VULKAN_DEVICE_H
