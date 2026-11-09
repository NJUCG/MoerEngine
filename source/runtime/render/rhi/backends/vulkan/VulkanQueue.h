#pragma once

#include "VulkanSubmission.h"
#include "VulkanTimeline.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

namespace Moer::Render {

class VulkanNativeQueue {
public:
    VulkanNativeQueue(EQueueType _type, VulkanDevice& _device);
    ~VulkanNativeQueue() = default;
    VulkanNativeQueue(const VulkanNativeQueue&) = delete;
    VulkanNativeQueue& operator=(const VulkanNativeQueue&) = delete;
    VulkanNativeQueue(VulkanNativeQueue&&) = delete;
    VulkanNativeQueue& operator=(VulkanNativeQueue&&) = delete;

    VulkanOperationResult
    Submit(VulkanCommandList& _cmdlist, const VulkanOperationContext& _context, VkFence _fence = VK_NULL_HANDLE);
    VulkanOperationResult Submit(
        std::span<VulkanCommandList* const> _cmdlists,
        const VulkanOperationContext&       _context,
        VkFence                             _fence = VK_NULL_HANDLE
    );
    VulkanOperationResult SubmitEmpty(const VulkanOperationContext& _context, VkFence _fence = VK_NULL_HANDLE);
    void
    Wait(VulkanTimeline* _fence, uint64 _timeline, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    void Wait(VkSemaphore _sem, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    void Signal(
        VulkanTimeline*       _fence,
        uint64                _timeline,
        VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
    );
    void Signal(VkSemaphore _semaphore, VkPipelineStageFlags2 _stage = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    void DiscardPendingSubmitState() noexcept {
        wait_infos.clear();
        signal_infos.clear();
    }
    VkQueue GetHandle() const {
        return queue;
    }
    EQueueType GetType() const {
        return type;
    }

    void BeginLabel(std::string_view _label, float4 _color);
    void EndLabel();
    void InsertLabel(std::string_view _label, float4 _color);

private:
    Array<VkSemaphoreSubmitInfo> wait_infos;
    Array<VkSemaphoreSubmitInfo> signal_infos;
    VulkanDevice&                device;
    VkQueue                      queue;
    EQueueType                   type;

    // Borrow the device's canonical mutex for this native queue from construction.
    std::mutex& submit_mutex;
};

class VulkanCommandQueue : public CommandQueue {
public:
    VulkanCommandQueue(VulkanDevice& _device, EQueueType _type);
    ~VulkanCommandQueue();

    WaitEvent   Execute(CmdSubmit&& _submit) override;
    void        Wait(WaitEvent _event) override;
    void        Present(SwapchainRef _viewport, TextureView _view, PresentReceiptRef _receipt = {}) override;
    void        Sync() override;
    ProfileData GetProfilerEntry() override;

private:
    friend class VulkanSubmissionExecutor;

    // Executor ownership and phase entry points.
    [[nodiscard]] bool ClaimRuntimeOwnership();
    void               ReleaseRuntimeOwnership() noexcept;
    void               CancelRuntimeDependencyWaits() noexcept;

    [[nodiscard]] VulkanPendingSubmission ReserveSubmission(CmdSubmit&& _submit) noexcept;

    // Determines the native submission result without transferring ownership.
    // Executor publishes affected failures before committing Completion.
    VulkanSubmissionResult SubmitRecordedForRuntime(VulkanPendingSubmission& _recorded) noexcept;
    void RejectRecordedForRuntime(VulkanPendingSubmission&& _recorded, VkResult _result, bool _recoverable) noexcept;
    void RejectForRuntime(
        CmdSubmit&& _submit,
        VkResult    _result,
        bool        _recoverable
    ) noexcept;
    VulkanSubmissionResult
    PresentForRuntime(SwapchainRef _swapchain, TextureView _source, PresentReceiptRef _receipt) noexcept;

    // Native submission and transfer to Completion.
    VulkanOperationResult
                           SubmitRecorded(VulkanPendingSubmission& _recorded, const std::atomic_bool* _continue_waiting = nullptr);
    VulkanSubmissionResult PresentSwapchain(
        SwapchainRef&&    _swapchain,
        TextureRef&&      _source_texture,
        TextureView       _source_view,
        PresentReceiptRef _receipt,
        uint64            _timeline,
        uint64            _serial
    ) noexcept;
    void
    CommitRuntimeSubmitCompletion(VulkanPendingSubmission& _recorded, const VulkanOperationResult& _outcome) noexcept;

    // Completion confirmation, callbacks and resource retirement.
    void CompletionThreadMain();

    void ProcessCompletion(VulkanSubmissionCompletion& _completion);
    void CompleteAll(uint64 _timeline);

    VulkanDevice&     vk_device;
    VulkanNativeQueue queue;
    VulkanTimelineRef timeline{nullptr};

    // The gate transfers recording ownership with each submission packet.
    std::mutex                   exec_mtx;
    RHITransferableOwnershipGate runtime_execution_gate;
    std::atomic_bool             executor_claimed{false};
    std::atomic_bool             runtime_dependency_waits_enabled{true};
    std::atomic<uint64>          last_reserved_timeline{0};

    // CPU retirement also includes requests which never reached the GPU.
    std::mutex                          event_mutex;
    std::condition_variable_any         queue_cv;
    DEQueue<VulkanSubmissionCompletion> event_queue;
    bool                                completion_worker_running{false};
    std::jthread                        completion_thread;
    uint64                              cpu_retired_timeline{0}; // Protected by event_mutex.
    uint64                              retirement_enqueued_serial{0};
    uint64                              retirement_settled_serial{0}; // Protected by event_mutex.
};

class VulkanCopyQueue : public CopyQueue {
public:
    VulkanCopyQueue(VulkanDevice& _device);
    ~VulkanCopyQueue();

    WaitEvent Execute(CmdSubmit&& _submit) override;
    FenceRef  GetFenceHandle() override;
    void      Sync(uint64 _timeline) override;

    void CopyFrom(BufferView _src, BufferView _dst) override {};
    void CopyFrom(TextureView _src, TextureView _dst) override {};
    void CopyFrom(TextureView _src, BufferView _dst) override {};
    void CopyFrom(BufferView _src, TextureView _dst) override {};
    void CopyFrom(std::span<byte> _data, BufferView _dst) override {};
    void CopyFrom(std::span<byte> _data, TextureView _dst) override {};

private:
    friend class VulkanDevice;
    friend class VulkanSubmissionExecutor;

    [[nodiscard]] bool      ClaimRuntimeOwnership();
    void                    ReleaseRuntimeOwnership() noexcept;
    void                    EnableRuntimeDependencyWaits() noexcept;
    void                    CancelRuntimeDependencyWaits() noexcept;
    VulkanPendingSubmission ReserveSubmission(CmdSubmit&& _submit) noexcept;
    // Retains the packet until Executor publishes failures and commits Completion.
    VulkanSubmissionResult SubmitRecordedForRuntime(VulkanPendingSubmission& _recorded) noexcept;
    void
         RejectRecordedForRuntime(VulkanPendingSubmission&& _recorded, VkResult _result, bool _recoverable) noexcept;
    void RejectForRuntime(
        CmdSubmit&& _submit,
        VkResult    _result,
        bool        _recoverable
    ) noexcept;

    void CommitRuntimeSubmitCompletion(
        VulkanPendingSubmission&     _recorded,
        const VulkanOperationResult& _outcome
    ) noexcept;
    void CompletionThreadMain();
    void CompleteAll(uint64 _timeline);

    VulkanDevice&     device;
    VulkanNativeQueue queue;
    VulkanTimelineRef timeline{nullptr};

    std::mutex                   exec_mutex;
    RHITransferableOwnershipGate runtime_execution_gate;
    std::atomic_bool             executor_claimed{false};
    std::atomic_bool             dependency_waits_enabled{true};
    std::atomic<uint64>          last_reserved_timeline{0};

    std::mutex                          event_mutex;
    std::condition_variable_any         queue_cv;
    std::condition_variable             settled_cv;
    DEQueue<VulkanSubmissionCompletion> event_queue;
    bool                                completion_worker_running{false}; // Protected by event_mutex.
    std::jthread                        completion_thread;
    uint64                              cpu_retired_timeline{0}; // Protected by event_mutex.
    uint64                              retirement_enqueued_serial{0};
    uint64                              retirement_settled_serial{0}; // Protected by event_mutex.
};

} // namespace Moer::Render
