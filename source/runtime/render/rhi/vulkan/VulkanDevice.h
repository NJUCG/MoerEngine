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
#include "VulkanFault.h"
#include "VulkanDeviceProperty.h"
#include "VulkanPlatform.h"
#include "VulkanQueue.h"
#include "VulkanTypeDefs.h"
#include "vulkanextension/VulkanExtension.h"

// #include <vk_mem_alloc.h>
#include "VulkanMemoryAllocator.h"

#include <optional>
#include <atomic>
#include <shared_mutex>

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
    uint32 api_version                  = VK_API_VERSION_1_3;
    bool   rhi_thread                   = false;
    bool   rhi_bypass                   = true;
    bool   thread_profile_logging       = false;
    bool   parallel_recording           = false;
    uint32 parallel_record_workers      = 0;
    bool   parallel_record_verify       = false;
    bool   parallel_record_profile      = false;
    uint32 parallel_record_min_work_units_per_job = 64;
    uint64 parallel_record_worker_throw_trigger = 0;
    uint64 present_submit_fault_trigger = 0;
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

    TextureRef
    CreateTexture(std::string_view _name, const TextureInfo& _info) override;

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

    RHIQueueTopology GetQueueTopology() const override;

    SwapchainRef CreateSwapchain(const SwapchainCreateInfo& _info) override;

    IOInterfaceRef CreateIOInterface(CopyQueue& _copy_queue) override;

    void FlushDebugMessages() const override;

    bool SupportsTessellation() const override {
        return m_device_info.core_features.core_1_0.tessellationShader == VK_TRUE;
    }

    uint32_t GetMaxTessellationFactor() const override {
        return SupportsTessellation() ?
                   m_device_info.core_properties.core_1_0.limits.maxTessellationGenerationLevel :
                   0;
    }

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
    [[nodiscard]] RENDER_API uint32_t GetTimestampValidBits(
        EQueueType _queue_type
    ) const;
    [[nodiscard]] RENDER_API VkQueueFlags GetQueueFamilyFlags(
        EQueueType _queue_type
    ) const;
    [[nodiscard]] RENDER_API EVulkanTimestampQueryResetMode
    GetTimestampQueryResetMode(
        EQueueType _queue_type,
        uint32_t   _effective_valid_bits
    ) const;
    [[nodiscard]] RENDER_API EVulkanTimestampQueryResetMode
    GetTimestampQueryResetMode(
        EQueueType _queue_type
    ) const;
    [[nodiscard]] RENDER_API bool SupportsTimestampQueries(
        EQueueType _queue_type
    ) const;
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
    VulkanOperationResult SubmitOnQueue(
        VkQueue                       _queue,
        const VkSubmitInfo2&          _submit_info,
        VkFence                       _fence,
        const VulkanOperationContext& _context
    );
    VulkanOperationResult PresentOnQueue(
        VkQueue                       _queue,
        const VkPresentInfoKHR&       _present_info,
        const VulkanOperationContext& _context
    );
    VulkanOperationResult AcquireNextImage(
        VkSwapchainKHR                _swapchain,
        uint64                        _timeout,
        VkSemaphore                   _semaphore,
        VkFence                       _fence,
        uint32*                       _image_index,
        const VulkanOperationContext& _context
    );
    VkResult WaitQueueIdle(VkQueue _queue, const VulkanOperationContext& _context);
    VkResult ResetCommandPool(VkCommandPool _pool, const VulkanOperationContext& _context);
    VkResult ResetFence(VkFence _fence, const VulkanOperationContext& _context);
    using FaultAdmissionLock = std::unique_lock<std::shared_mutex>;
    [[nodiscard]] FaultAdmissionLock AcquireFaultAdmission() {
        return FaultAdmissionLock(native_queue_gate);
    }

    bool TryLatchFirstFault(
        const VulkanOperationContext& _context,
        VkResult                      _result,
        bool                          _injected = false,
        bool                          _predrained = false,
        bool                          _force_terminal = false
    );
    [[nodiscard]] bool IsFaulted() const;
    [[nodiscard]] bool IsDeviceLost() const;
    [[nodiscard]] VkResult GetFirstFaultResult() const;
    [[noreturn]] void EmergencyExitWithoutVulkanCleanup(
        const VulkanOperationContext& _context, VkResult _result
    );
    [[nodiscard]] bool ShouldInjectPresentSubmit();
    [[nodiscard]] uint64 GetPresentSubmitFaultTrigger() const {
        return present_submit_fault_trigger;
    }
    VulkanOperationResult InjectPresentSubmitFault(const VulkanOperationContext& _context);
    void RecordRejectedSubmit();
    void RecordRejectedPresent();
    void RecordAllocatorQuarantine();
    void RecordSkippedCommandPoolReset();
    void RecordQueueSyncComplete();

private:
    std::mutex& GetQueueHostMutex(VkQueue _queue);
    Array<std::mutex*> GetUniqueQueueHostMutexes();
    bool TryBeginFirstFault(VkResult _result);
    void PublishFirstFaultLocked(
        const VulkanOperationContext& _context,
        VkResult                      _result,
        bool                          _injected,
        bool                          _predrained
    );

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

    VmaAllocator              m_allocator = VK_NULL_HANDLE;
    VulkanDescriptorHeap      m_global_descriptor_heap{};
    UniquePtr<VkCommandQueue> gfx_queue{}; // VkCommandQueue是MoerEngine的封装！
    UniquePtr<VkCommandQueue> compute_queue{};
    UniquePtr<VkCopyQueue>    copy_queue{};
    bool                      rhi_thread_enabled            = false;
    bool                      thread_profile_logging       = false;
    bool                      parallel_recording           = false;
    uint32                    parallel_record_workers      = 0;
    bool                      parallel_record_verify       = false;
    bool                      parallel_record_profile      = false;
    uint32                    parallel_record_min_work_units_per_job = 64;
    uint64                    parallel_record_worker_throw_trigger = 0;
    uint64                    present_submit_fault_trigger = 0;
    std::atomic<uint64>       present_submit_attempts{0};

    std::atomic<EVulkanFaultPublishState> fault_state{EVulkanFaultPublishState::Healthy};
    std::atomic_bool                        device_lost_observed{false};
    VulkanFaultRecord                       first_fault{};
    std::atomic<uint32>                     first_fault_count{0};
    std::atomic<uint64>                     native_submit_call_count{0};
    std::atomic<uint64>                     native_present_call_count{0};
    std::atomic<uint64>                     fault_submit_call_snapshot{0};
    std::atomic<uint64>                     fault_present_call_snapshot{0};
    std::atomic<uint64>                     rejected_submit_count{0};
    std::atomic<uint64>                     rejected_present_count{0};
    std::atomic<uint64>                     allocator_quarantine_count{0};
    std::atomic<uint64>                     skipped_command_pool_reset_count{0};
    std::atomic<uint64>                     queue_sync_complete_count{0};
    std::atomic_bool                        shutdown_sync_completed{false};
    std::atomic_bool                        emergency_exit_started{false};
    // Shared access covers native calls; fault publication takes exclusive access.
    std::shared_mutex                         native_queue_gate;
    // Every host call operating on the same VkQueue must use the same mutex.
    std::mutex                                m_graphics_queue_mutex;
    std::mutex                                m_present_queue_mutex;
    std::mutex                                m_compute_queue_mutex;
    std::mutex                                m_transfer_queue_mutex;
    std::mutex                                m_raytracing_queue_mutex;
    LockFreeQueueBase<RHIResource, false, 64> deferred_release_queue{};
    static constexpr uint immutable_sampler_count = uint(SF_Num) * uint(SAM_Num) * uint(SCF_Num);
    StaticArray<VkSampler, immutable_sampler_count> immutable_samplers{};

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
    void LogFaultSummary() const;

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
