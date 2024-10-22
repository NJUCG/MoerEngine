#pragma once
#include "VulkanCommand.h"
#include "VulkanAllocator.h"
// #include "VulkanDevice.h"
#include "io/IOCommon.h"
#include "misc/LockFree.h"
#include "misc/STL.h"
#include "rhi/RHICommand.h"
#include <functional>
#include <mutex>
namespace Moer::Render {
    class VkNativeQueue {
    public:
        VkNativeQueue(EQueueType _type, VulkanDevice& _device);
        ~VkNativeQueue();

        void       Submit(VulkanCmdList& _cmdlist, VkFence _fence = VK_NULL_HANDLE);
        void       SubmitEmpty(VkFence _fence = VK_NULL_HANDLE);
        void       Wait(VulkanFence* _fence, uint64 _timeline, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
        void       Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
        void       Signal(VulkanFence* _fence, uint64 _timeline, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        void       Signal(VkSemaphore _semaphore, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
        VkQueue    GetHandle() const { return queue; }
        EQueueType GetType() const { return type; }

        void BeginLabel(std::string_view _label, float4 _color);
        void EndLabel();
        void InsertLabel(std::string_view _label, float4 _color);

    private:
        Array<VkSemaphoreSubmitInfo> wait_infos;
        Array<VkSemaphoreSubmitInfo> signal_infos;
        VkQueue                      queue;
        EQueueType                   type;
    };

    class VkCommandQueue : public CommandQueue {
    public:
        struct FencePlaceHoler {};
        using EventType = std::variant<
            UniquePtr<VulkanAllocator>,
            Array<std::function<void()>>,
            VulkanFence*,
            SignalEvent,
            WaitEvent,
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
            timeline = MoerNew(VulkanFence(vk_device));
            thread   = std::jthread(&VkCommandQueue::ExecuteThread, this);
            enabled  = true;
        }

        ~VkCommandQueue() {
            enabled = false;
            queue_cv.notify_all();
            thread.join();
            //clear allocators
            Array<VulkanAllocator*> allocs;
            allocators.PopAll(allocs);
            for (auto& allocator : allocs) {
                MoerDelete(allocator);
            }
            MoerDelete(timeline);
        }
        WaitEvent Execute(CmdSubmit&& _submit) override;
        void      Wait(WaitEvent _event) override;
        void      Present(SwapchainRef _viewport, TextureView _view) override;
        void      Sync() override;

        void                                      ExecuteThread();
        VulkanDevice&                             vk_device;
        LockFreeQueueBase<VulkanAllocator, false> allocators;
        DEQueue<QueueEvent>                       event_queue;

    private:
        UniquePtr<VulkanAllocator> GetAllocator();
        void                       Complete(uint64 _timeline);
        void                       Signal();

    private:
        uint                    last_frame     = 0;
        std::atomic<uint64>     executed_frame = 0;
        VulkanFence*            timeline       = nullptr;
        std::mutex              event_mutex;
        bool                    enabled{false};
        std::condition_variable queue_cv;// wake up execute thread from sleeping
        VkNativeQueue           queue;

        Queue<VulkanFence*> present_fences;
        std::mutex          present_mutex;
        std::jthread        thread;
    };

    class VkCopyQueue : public CopyQueue {
    public:
        VkCopyQueue(VulkanDevice& _device);
        ~VkCopyQueue();
        struct Placeholder {};
        using EventType = std::variant<
            UniquePtr<VulkanAllocator>,
            Array<std::function<void()>>,
            IOSignalEvt,
            IOWaitEvt,
            Placeholder>;
        struct IOEvent {
            EventType event;
            uint64    timeline;
            bool      wake_thread;
            template<typename Arg>
                requires std::is_constructible_v<EventType, Arg&&>
            IOEvent(Arg&& _event, uint64 _timeline, bool _wake_thread) : event(std::forward<Arg>(_event)), timeline(_timeline), wake_thread(_wake_thread) {
            }

            IOEvent(IOEvent&& _other) noexcept : event(std::move(_other.event)), timeline(_other.timeline), wake_thread(_other.wake_thread) {
            }
        };

        IOWaitEvt Execute(IOSubmission&& _submit) override;
        IOWaitEvt Execute(CmdSubmit&& _submit) override;
        FenceRef  GetFenceHandle() override;
        void      Sync(uint64 _timeline) override;

        virtual void CopyFrom(BufferView _src, BufferView _dst) override {};
        virtual void CopyFrom(TextureView _src, TextureView _dst) override {};
        virtual void CopyFrom(TextureView _src, BufferView _dst) override {};
        virtual void CopyFrom(BufferView _src, TextureView _dst) override {};
        virtual void CopyFrom(std::span<byte> _data, BufferView _dst) override {};
        virtual void CopyFrom(std::span<byte> _data, TextureView _dst) override {};

        ///

        void ExecuteThread();

    private:
        UniquePtr<VulkanAllocator>                GetAllocator();
        void                                      Complete(uint64 _timeline);
        LockFreeQueueBase<VulkanAllocator, false> allocators;
        DEQueue<IOEvent>                          event_queue;

    private:
        VulkanDevice& device;

        uint                    last_frame     = 0;
        std::atomic<uint64>     executed_frame = 0;
        VulkanFence*            timeline       = nullptr;
        std::mutex              event_mutex;
        bool                    enabled{false};
        std::condition_variable queue_cv;// wake up execute thread from sleeping
        VkNativeQueue           queue;
        std::jthread            thread;

        //tmp
        LockFreeQueueBase<Command> commands;

        std::mutex exec_mutex;
    };
}// namespace Moer::Render