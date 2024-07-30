
#ifndef VULKAN_COMMAND_H
#define VULKAN_COMMAND_H
#include "log/LogSystem.h"
#include "misc/Traits.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"

#include "misc/STL.h"

#include "VulkanRHIResource.h"
#include "rhi/RHIResource.h"
#include "vulkan/vulkan_core.h"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <type_traits>
#include <variant>
#include <vulkan/vulkan.h>
namespace Moer::Render {
    class VulkanDevice;
    class VulkanDescriptorSetAllocator;
    class VulkanRHIGraphicsPipelineState;

    struct PushConstantInfo;
    class VulkanRHICommandListBase : public VulkanDeviceObject {
    public:
        VulkanRHICommandListBase(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        virtual ~VulkanRHICommandListBase();
        void Begin();
        void End();
        void Reset();

        void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);

        void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset);

        void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst);
        void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst);
        void CopyBufferToTexture(RHIBuffer* src_buffer, RHITexture* dst_texture, const RHICopyBufferToTextureInfo& _info);

        void CopyTextureToBuffer(RHITexture* src_texture, RHIBuffer* dst_buffer, const RHICopyTextureToBufferInfo& _info);

        //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
        //must not be used for multi-sampled source or destination textures, use resolve instead
        void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst);

        //To resolve a multi-sample color texture to a non-multisample color texture
        void ResolveTexture(
            const RHIResolveTextureInfo& _blit_info,
            RHITexture*                  _src,
            RHITexture*                  _dst);

        void                   SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency);
        inline VkCommandBuffer GetHandle() const { return m_command_buffer; }

        void TransitionTextureBase(RHITexture* _target, ETextureStateFlags _target_state, EPassType _pass_type, uint8_t _mip_level, uint8_t _mip_cnt);
        void ExecuteTransitionBase();

    protected:
        VkCommandPool        m_current_command_pool;
        VkCommandBuffer      m_command_buffer;
        VkCommandBufferLevel m_level;

        VkDependencyInfo m_dependency_info{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        using uint = Moer::uint;
        Moer::Array<VkImageMemoryBarrier2>  m_image_barriers;
        Moer::Array<VkBufferMemoryBarrier2> m_buffer_barriers;
        Moer::Array<VkMemoryBarrier2>       m_memory_barriers;
        uint                                m_image_barrier_count  = 0;
        uint                                m_buffer_barrier_count = 0;
        uint                                m_memory_barrier_count = 0;
    };

    class VulkanCommandAllocator : public VulkanDeviceObject {
    public:
        VulkanCommandAllocator() = default;
        VulkanCommandAllocator(VulkanCommandAllocator&& _other) {
            m_command_pool = std::move(_other.m_command_pool);
        };

        VulkanCommandAllocator& operator=(VulkanCommandAllocator&& _other) {
            if (this != &_other) {
                m_command_pool = std::move(_other.m_command_pool);
            }
            return *this;
        }

        VulkanCommandAllocator(const VulkanCommandAllocator& _other) = delete;
        VulkanCommandAllocator(VulkanDevice* _device);
        ~VulkanCommandAllocator() = default;

        void Reset();

        void Dispose();

        inline VkCommandPool GetHandle(ECommandListType _type) const { return m_command_pool[(size_t)_type]; }

    private:
        Moer::StaticArray<VkCommandPool, (size_t)ECommandListType::Num> m_command_pool;
    };
    class VulkanCmdList {
    private:
        VkCommandBuffer           command_buffer;
        class VulkanCmdAllocator* allocator;
        VulkanDevice&             device;

    public:
        VulkanCmdList(VulkanCmdAllocator* _allocator, VulkanDevice& _device);
        void CopyBuffer(VulkanBuffer* _src, VulkanBuffer* _dst, uint64 _size, uint64 _src_offset, uint64 _dst_offset);
        void CopyData(BufferView& _dst, const void* _data, uint64 _size);
        void DrawIndexedInstanced(uint32_t _index_count, uint32_t _instance_count, uint32_t _start_index_location, uint32_t _base_vertex_location, uint32_t _start_instance_location);
        void DrawInstanced(uint32_t _vertex_count, uint32_t _instance_count, uint32_t _start_vertex_location, uint32_t _start_instance_location);
        void DrawIndirectCnt(VulkanBuffer* _arg_buffer, uint64 _arg_offset, VulkanBuffer* _count_buffer, uint64 _count_buffer_offset, uint32_t _max_draw_count, uint32_t _stride);
        void CopyTexture(VulkanTexture* _src, VulkanTexture* _dst, uint3 _extent, uint3 _src_offset, uint3 _dst_offset, uint32 _src_mip_level, uint32 _dst_mip_level);
        void BeginRendering(VkRenderingInfo&& _info);
        void EndRendering();
        void SetVertexBuffers(uint _start_index, uint _num_buffers, std::span<VkBuffer>, std::span<uint64> _offsets);
        void SetIndexBuffer(VulkanBuffer* _buffer, uint64 _offset, VkIndexType _type);

        void SetScissor(const VkRect2D& _scissor);
        void SetViewPort(const VkViewport& _viewport);

        void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z);
        void DispatchIndirect(VulkanBuffer* _buffer, uint64 _offset);

        void UploadDescriptors(PipelineHandle& _pso_handle);
        void UploadPushConstants(PipelineHandle& _pso_handle, std::span<const uint> _data);

        void            SetPso(const PipelineHandle& _pso_handle);
        VkCommandBuffer GetHandle() const { return command_buffer; }
    };
    //allocator for tmp buffer and other tmp resources
    class VulkanCmdAllocator : public VulkanDeviceObject {
    private:
        VkQueueFlags                 queue_type;
        VkCommandPool                command_pool;
        std::optional<VulkanCmdList> command_list;

    public:
        VulkanCmdAllocator(VulkanDevice* _device, VkQueueFlagBits _queue_type);
        VkCommandPool  GetHandle() const { return command_pool; }
        VulkanCmdList& GetCommandList() {
            if (!command_list.has_value()) {
                LOG_ERROR("Command list is not initialized");
            }
            return *command_list;
        }
    };
    class VulkanAllocator : public VulkanDeviceObject {
    public:
        VulkanAllocator(VulkanDevice* _device);
        ~VulkanAllocator();
        BufferView     AllocateBuffer(uint64 _size, uint _align);
        VulkanCmdList& GetCmdList() {
            return cmd_list.value();
        }
        void ResetBufferAlloc();
        void ResetCmdList();
        void Complete(VulkanFence* _fence, uint64 _timeline);
        void Reset();
        void AddOnComplete(std::function<void()>&& _func) {
            on_complete.push_back(std::move(_func));
        }
        //staging buffer allocate with block strategy
    private:
        uint64 small_block_size;
        uint64 large_threshold;
        struct TmpBufferAllocator : VulkanDeviceObject {
            TmpBufferAllocator(VulkanDevice* _device);
            uint64 Allocate(uint64 _size);
            void   DeAllocate(uint64 _handle);
        };
        struct StackAllocator {
            uint64 init_capacity;
            uint64 capacity;
            double growth_factor;
            struct Chunk {
                uint64 handle;
                uint64 offset;
            };
            struct Buffer {
                uint64 handle;
                uint64 size;
                uint64 offset;
            };
            StackAllocator(TmpBufferAllocator*, uint64 _init_capacity, double _growth_factor);

            TmpBufferAllocator* allocator;
            Array<Buffer>       allocated_buffers;
            Chunk               Allocate(uint64 _size, uint _align);
            Chunk               Allocate(uint64 _size);
            void                Reset();
            void                Dispose();
        };
        std::optional<VulkanCmdList>      cmd_list;
        std::optional<VulkanCmdAllocator> cmd_allocator;
        Array<VulkanBuffer*>              large_buffers;
        TmpBufferAllocator                allocator;

        StackAllocator               small_allocator;
        Array<std::function<void()>> on_complete;

        struct LargeAllocator {
            TmpBufferAllocator* allocator;
            Array<uint64>       allocated_buffers;
            LargeAllocator(TmpBufferAllocator* _allocator);
            uint64 Allocate(uint64 _size, uint _align);
            void   DeAllocate(uint64 _handle);
            void   ResetBufferAlloc();
            void   Dispose();
        };
    };

    static_assert(std::is_trivially_destructible_v<VulkanCommandAllocator>);
    class VulkanRHIGraphicsCommandList final : public RHIGraphicsCommandList,
                                               public VulkanRHICommandListBase {
    public:
        VulkanRHIGraphicsCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        virtual ~VulkanRHIGraphicsCommandList();

        void* GetNativeHandle() const override {
            return m_command_buffer;
        }

        void SetPipelineState(RHIGfxPso* _graphics_pso) override;
        void SetPipelineState(RHIComputePso* _compute_pso) override;
        void BeginRecording() override;
        void EndRecording() override;
        void Reset() override;
        void ClearState(RHIGfxPso* _graphics_pso) override;

        void DrawIndexedInstanced(uint32_t _index_count,
                                  uint32_t _instance_count,
                                  uint32_t _start_index_location,
                                  uint32_t _start_vertex_location,
                                  uint32_t _start_instance_location) override;

        void DrawIndexedIndirect(
            RHIBuffer* _argument_buffer,
            uint64_t   _arg_offset,
            RHIBuffer* _count_buffer,
            uint64_t   _count_buffer_offset,
            uint32_t   _max_draw_count,
            uint32_t   _stride) override;

        void Draw(uint32_t _vertex_count, uint32_t _instance_count, uint32_t _start_vertex_location, uint32_t _start_instance_location) override;

        void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) override;

        void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) override;

        void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
        void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
        void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) override;

        void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) override;

        //To copy regions of a source texture into a destination texture, potentially performing format conversion, arbitrary scaling, and filtering.
        //must not be used for multi-sampled source or destination textures, use resolve instead
        void BlitTexture(const RHIBlitTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;

        //To resolve a multi-sample color texture to a non-multisample color texture
        void ResolveTexture(const RHIResolveTextureInfo& _blit_info, RHITexture* _src, RHITexture* _dst) override;

        void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;

        void SetCullMode(ERasterizerCullMode _cull_mode) override;
        void SetPrimitiveTopology(EPrimitiveTopology _topology) override;
        void SetViewPorts(uint32_t num_viewports, const ViewPort* p_viewports) override;
        void SetViewPort(const ViewPort& _viewport) override;
        void SetScissors(uint32_t num_scissors, const Rect2D* p_scissors) override;
        void SetScissor(const Rect2D& _scissor) override;
        void SetBlendFactors(const float _factors[4]) override;

        void BindVertexBuffers(
            uint32_t            _start_index,
            uint32_t            _num_buffers,
            const RHIBufferRef* p_vertex_buffers,
            const uint32_t*     _offsets) override;

        void BindIndexBuffer(
            const RHIBuffer*  p_index_buffer,
            uint32_t          _offset,
            EIndexElementType _type) override;

        void FillBuffer(RHIBuffer* _buffer, uint64_t _offset, uint64_t _size, uint32_t _data) override;

        void SetAttachments() override {
        }

        void ClearDepthStencil() override;
        void ClearUAVInt(
            RHIUAV*               _uav,
            const Moer::Vector4i& _values) override;
        void ClearUAVFloat(
            RHIUAV*               _uav,
            const Moer::Vector4f& _values) override;

        void BeginRenderPass(const RHIRenderPassInfo& _pass_info, const char* _pass_name) override;
        void EndRenderPass() override;

        void NextSubpass() override;

        //todo: query data declaration
        void BeginQuery(RHIRenderQuery* _query) override;
        void EndQuery(RHIRenderQuery* _query) override;

        void GetQueryData(
            ERenderQueryType _query_type,
            uint32_t         _first_index,
            uint32_t         _num_queries,
            RHIBuffer*       _dst_buffer,
            uint64_t         _dst_offset) override;

        void ExecuteSubCommands(uint32_t                _num,
                                RHIGraphicsCommandList* _sub_commands) override;

        void BeginLabel(const char* _label) override;
        void EndLabel() override;

        void TransitionTexture(RHITexture* _target, ETextureStateFlags _target_usage, EPassType _pass_type, uint8_t _mip_level, uint8_t _mip_cnt) override;
        void ExecuteTransition() override;

    protected:
        friend class VulkanRHICommandQueue;

    private:
        VulkanRHIGraphicsPipelineState*                                               m_current_pipeline_state;
        std::variant<VulkanRHIGraphicsPipelineState*, VulkanRHIComputePipelineState*> current_pso;

    private:
        VkRenderingAttachmentInfo FromColorAttachmentInfo(const RHIRenderPassInfo::ColorAttachmentInfo& _color_attachment_info) const;
        VkRenderingAttachmentInfo FromDepthStencilAttachmentInfo(const RHIRenderPassInfo::DepthStencilAttachmentInfo& _depth_stencil_attachment_info) const;

        void PrepareDrawCommand();
        void PrepareDispatch();

        // MARK: bound sets rely on corresponding command list, it maybe wrong when muti-threading recording.
        Moer::Array<VkDescriptorSet> m_bound_sets;
    };

    class VulkanRHICopyCommandList final : public RHICopyCommandList, public VulkanRHICommandListBase {
    public:
        VulkanRHICopyCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        virtual ~VulkanRHICopyCommandList();
        void* GetNativeHandle() const override {
            return m_command_buffer;
        }
        void BeginRecording() override;
        void EndRecording() override;
        void Reset() override;

        void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
        void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
        void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) override;

        void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) override;
        void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;
    };

    class VulkanRHIComputeCommandList final : public RHIComputeCommandList, public VulkanRHICommandListBase {
    public:
        VulkanRHIComputeCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

        virtual ~VulkanRHIComputeCommandList();

        void* GetNativeHandle() const override {
            return m_command_buffer;
        }
        void SetPipelineState(RHIComputePso* _compute_pso) override;
        void BeginRecording() override;
        void EndRecording() override;
        void Reset() override;

        void Dispatch(uint32_t _group_count_x, uint32_t _group_count_y, uint32_t _group_count_z) override;
        void DispatchIndirect(RHIBuffer* _buffer, uint64_t _offset) override;

        void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
        void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
        void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) override;

        void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) override;

        void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;

    protected:
        void PrepareDispatch();

    private:
        void PrepareDispatchCommand();
        // MARK: bound sets rely on corresponding command list, it maybe wrong when muti-threading recording.
        Moer::Array<VkDescriptorSet> m_bound_sets;

    private:
        VulkanRHIComputePipelineState* m_current_pipeline_state = nullptr;
    };
    class VulkanRHIRayTracingCommandList final : public RHIRayTracingCommandList, public VulkanRHICommandListBase {
    public:
        VulkanRHIRayTracingCommandList(VulkanDevice* _device, VkCommandPool _pool, VkCommandBufferLevel _level = VK_COMMAND_BUFFER_LEVEL_PRIMARY);

        virtual ~VulkanRHIRayTracingCommandList();

        void* GetNativeHandle() const override {
            return m_command_buffer;
        }
        void SetPipelineState(RHIRTPso* _raytracing_pso) override;
        void BeginRecording() override;
        void EndRecording() override;
        void Reset() override;

        void TraceRay(uint32_t _width, uint32_t _height, uint32_t _depth) override;
        void TraceRayIndirect() override;

        void CopyBuffer(const RHICopyBufferInfo& _copy_info, RHIBuffer* _src, RHIBuffer* _dst) override;
        void CopyTexture(const RHICopyTextureInfo& _copy_info, RHITexture* _src, RHITexture* _dst) override;
        void CopyBufferToTexture(const RHICopyBufferToTextureInfo& _info, RHIBuffer* src_buffer, RHITexture* dst_texture) override;

        void CopyTextureToBuffer(const RHICopyTextureToBufferInfo& _info, RHITexture* src_texture, RHIBuffer* dst_buffer) override;

        void SetPipelineBarrier(const RHIBarrierDependencyInfo& _dependency) override;

    private:
        void PrepareTraceRayCommand();
        // MARK: bound sets rely on corresponding command list, it maybe wrong when muti-threading recording.
        Moer::Array<VkDescriptorSet> m_bound_sets;

    private:
        VulkanRHIRayTracingPipelineState* m_current_pipeline_state = nullptr;
    };

    class VulkanRHICommandQueue final : public RHICommandQueue,
                                        public VulkanDeviceObject {
    public:
        VulkanRHICommandQueue(VulkanDevice* _device, ECommandQueueType _type);
        virtual ~VulkanRHICommandQueue();
        virtual void SubmitCommands(
            uint32_t                  _num_command_lists,
            const RHICommandListBase* _command_lists,
            const RHISubmitInfo*      _submit_info = nullptr) override;
        inline VkQueue GetHandle() { return queue; }

        virtual void WaitForQueueComplete() override;

    private:
        VkQueue queue;
    };

    class VkNativeQueue {
    public:
        VkNativeQueue(EQueueType _type, VulkanDevice& _device);
        ~VkNativeQueue();

        void    Submit(VulkanCmdList& _cmdlist);
        void    Wait(VulkanFence* _fence, uint64 _timeline);
        void    Signal(VulkanFence* _fence, uint64 _timeline);
        VkQueue GetHandle() const { return queue; }

    private:
        Array<VkSemaphoreSubmitInfo> wait_infos;
        Array<VkSemaphoreSubmitInfo> signal_infos;
        VkQueue                      queue;
    };

    class VkCommandQueue : public CommandQueue {
    public:
        struct FencePlaceHoler {};
        using EventType = std::variant<
            UniquePtr<VulkanAllocator>,
            Array<std::function<void()>>,
            FencePlaceHoler>;

        struct QueueEvent {
            EventType event;
            uint64    timeline;
            bool      wake_thread;
            template<typename Arg>
                requires std::is_constructible_v<EventType, Arg&&>
            QueueEvent(Arg&& _event, uint64 _timeline, bool _wake_thread) : event(std::forward<Arg>(_event)), timeline(_timeline), wake_thread(_wake_thread) {
            }

            QueueEvent(QueueEvent&& _other) noexcept : event(std::move(_other.event)), timeline(_other.timeline), wake_thread(_other.wake_thread) {
            }
        };

        VkCommandQueue(VulkanDevice& _device, EQueueType _type) : CommandQueue(), vk_device(_device), queue(_type, _device) {
            timeline = MoerNew(VulkanFence(EFenceUsageFlags::TIMELINE, vk_device));
        }
        void Execute(CmdSubmit&& _submit) override;
        void Present(RHIViewport* _viewport, TextureView _view) override;
        void Present(SwapchainRef _viewport, TextureView _view) override;
        void Sync() override;

        void                               ExecuteThread();
        VulkanDevice&                      vk_device;
        LockFreeQueueBase<VulkanAllocator> allocators;
        DEQueue<QueueEvent>                event_queue;

    private:
        UniquePtr<VulkanAllocator> GetAllocator();
        void                       Complete(uint64 _timeline);
        void                       Signal();

    private:
        uint                    last_frame;
        std::atomic<uint64>     executed_frame;
        VulkanFence*            timeline;
        std::mutex              event_mutex;
        bool                    enabled{false};
        std::condition_variable queue_cv;// wake up execute thread from sleeping
        Event*                  event;
        VkNativeQueue           queue;

        Queue<VulkanFence*> present_fences;
        std::mutex          present_mutex;
    };
}// namespace Moer::Render
#endif//VULKAN_COMMAND_H
