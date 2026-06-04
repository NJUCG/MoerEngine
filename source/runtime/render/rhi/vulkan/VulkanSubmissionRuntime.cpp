#include "VulkanSubmissionRuntime.h"

#include "VulkanInterruptRuntime.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanRHITrace.h"
#include "VulkanThreadHeartbeat.h"
#include "log/LogSystem.h"
#include "rhi/GPUEventStream.h"
#include "rhi/RHIImpl.h"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <thread>

namespace Moer::Render {

class VulkanSubmissionRuntime::SubmissionRunnable : public Runnable {
public:
    explicit SubmissionRunnable(VulkanSubmissionRuntime& in_owner) : m_owner(in_owner) {}

    uint32_t Run() override {
        m_owner.RunSubmissionThread();
        return 0;
    }

    void Init() override {}
    void Stop() override {
        m_owner.Shutdown();
    }
    void Exit() override {}
    ThreadIndex GetIndex() override {
        return EThread::UNKNOWN_THREAD;
    }

private:
    VulkanSubmissionRuntime& m_owner;
};

namespace {

const char* QueueTypeName(EQueueType queue) {
    switch (queue) {
        case EQueueType::Graphics: return "Graphics";
        case EQueueType::Compute:  return "Compute";
        case EQueueType::Copy:     return "Copy";
        case EQueueType::Ignore:   return "Ignore";
        case EQueueType::Num:
        default:                   return "Num";
    }
}

EPassType QueueToPassType(EQueueType queue) {
    switch (queue) {
        case EQueueType::Compute:
            return EPassType::Compute;
        case EQueueType::Graphics:
        case EQueueType::Copy:
        case EQueueType::Ignore:
        case EQueueType::Num:
        default:
            return EPassType::Graphics;
    }
}

const char* VkLayoutStr(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED:                        return "UNDEFINED";
        case VK_IMAGE_LAYOUT_GENERAL:                          return "GENERAL";
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:         return "COLOR_ATTACHMENT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT";
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:  return "DEPTH_STENCIL_READ_ONLY";
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:         return "SHADER_READ_ONLY";
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:             return "TRANSFER_SRC";
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:             return "TRANSFER_DST";
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:                  return "PRESENT_SRC";
        case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:                return "READ_ONLY";
        case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:               return "ATTACHMENT";
        default:                                               return "UNKNOWN";
    }
}

std::tuple<VkAccessFlagBits2, VkImageLayout, VkPipelineStageFlagBits2>
ResolveTextureSeedState(VulkanTexture* texture, const ResourceStateValue& state) {
    if (!state.known || state.texture_state == ETextureState::UNDEFINED) {
        return {VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE};
    }

    if (texture != nullptr && texture->b_present) {
        return {VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_NONE};
    }

    const EQueueType owner_queue =
        state.owner_queue == EQueueType::Ignore ? EQueueType::Graphics : state.owner_queue;
    VkTracker seed_tracker(owner_queue);
    auto      result = state.has_writer ?
                           seed_tracker.WriteTexture(texture, state.texture_state, QueueToPassType(owner_queue)) :
                           seed_tracker.ReadTexture(texture, state.texture_state, QueueToPassType(owner_queue));
    return {
        static_cast<VkAccessFlagBits2>(std::get<0>(result)),
        std::get<1>(result),
        static_cast<VkPipelineStageFlagBits2>(std::get<2>(result)),
    };
}

bool RootRhiBoundaryHasGpuWaits(const RootRhiBoundary& boundary) {
    return !boundary.gpu_waits.empty();
}

bool RootRhiBoundaryHasHostEvent(const RootRhiBoundary& boundary) {
    return boundary.host_event != nullptr;
}

GraphEventRef ChainCompletionBoundary(
    const GraphEventRef& previous_boundary,
    const GraphEventRef& completion_event
) {
    if (!completion_event) {
        return previous_boundary;
    }
    if (!previous_boundary) {
        return completion_event;
    }

    GraphEventRef chained_boundary = GraphEvent::CreateGraphEvent();
    chained_boundary->WaitUntil(previous_boundary);
    chained_boundary->WaitUntil(completion_event);
    chained_boundary->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return chained_boundary;
}

bool BatchKindRequiresCompletionEvent(SubmissionBatch::EKind kind) {
    return kind == SubmissionBatch::EKind::Drain || kind == SubmissionBatch::EKind::Sync ||
           kind == SubmissionBatch::EKind::Flush;
}

ERHISyncDepth ResolveBatchSyncDepth(const SubmissionBatch& batch) {
    return batch.kind == SubmissionBatch::EKind::Flush ? ERHISyncDepth::Present : batch.sync_depth;
}

size_t QueueTypeIndex(EQueueType queue) {
    const size_t index = static_cast<size_t>(queue);
    assert(index < static_cast<size_t>(EQueueType::Num) && "invalid queue type index");
    return index;
}

Array<WaitEvent> CollapseWaitEventsByTimelineMax(std::span<const WaitEvent> wait_events) {
    UnorderedMap<uint64, uint64> max_value_by_handle{};
    max_value_by_handle.reserve(wait_events.size());
    for (const WaitEvent& wait : wait_events) {
        auto& max_value = max_value_by_handle[wait.timeline_handle];
        max_value       = std::max(max_value, wait.value);
    }

    Array<WaitEvent> final_waits{};
    final_waits.reserve(max_value_by_handle.size());
    for (const auto& [timeline_handle, value] : max_value_by_handle) {
        final_waits.emplace_back(WaitEvent{timeline_handle, value});
    }
    return final_waits;
}

GraphEventRef CreateCompletedEvent() {
    GraphEventRef completed = GraphEvent::CreateGraphEvent();
    completed->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return completed;
}

GraphEventRef CreateCompletionWaitEvent(const GraphEventRef& completion_event) {
    GraphEventRef wait_event = GraphEvent::CreateGraphEvent();
    if (completion_event) {
        wait_event->WaitUntil(completion_event);
    }
    wait_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return wait_event;
}

void FinishBatchCompletion(SubmissionBatch& batch) {
    if (!batch.completion_event) {
        assert(!BatchKindRequiresCompletionEvent(batch.kind) && "blocking batch must have a completion event");
        return;
    }
    batch.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
}

} // namespace

struct SubmissionPresentContext::Impl {
    explicit Impl(EQueueType in_queue_type) :
        queue_type(in_queue_type),
        command_queue(static_cast<VkCommandQueue&>(RenderDevice::Get().GetCommandQueue(in_queue_type))),
        native_queue(in_queue_type, command_queue.vk_device),
        timeline(MoerNew(VulkanFence(command_queue.vk_device))) {}

    ~Impl() {
        Array<VulkanPresentor*> cached_presentors{};
        presentors.PopAll(cached_presentors);
        for (auto* presentor : cached_presentors) {
            MoerDelete(presentor);
        }
    }

    UniquePtr<VulkanPresentor> AcquirePresentor() {
        auto presentor = UniquePtr<VulkanPresentor>(presentors.Pop());
        if (presentor) {
            return presentor;
        }
        return MakeUnique<VulkanPresentor>(&command_queue.vk_device, queue_type);
    }

    void WaitForReusablePresentSlot(VkSwapchain& swapchain) {
        const uint64 present_index = swapchain.image_idx;
        if (present_index < swapchain.max_frames_in_flight) {
            return;
        }

        // Wait on our user-space timeline instead of busy-polling VkFence.
        // This avoids racing with the InterruptThread which also polls the same
        // fence.  The timeline is signaled by the InterruptThread via
        // ResolvePresentCompletion once the fence is ready.
        const uint64 wait_value = present_index - swapchain.max_frames_in_flight + 1;
        if (timeline) {
            timeline->Wait(wait_value);
        }

        // Reset the VkFence so it can be reused for the next present submission.
        // This runs on the SubmissionThread (NOT the main thread), so it does not
        // violate the thread model.
        VkFence fence = swapchain.GetInFlightFence(present_index);
        if (fence != VK_NULL_HANDLE) {
            vkResetFences(command_queue.vk_device.GetDevice(), 1, &fence);
        }
    }

    EQueueType                         queue_type{EQueueType::Graphics};
    VkCommandQueue&                    command_queue;
    VkNativeQueue                      native_queue;
    VulkanFenceRef                     timeline = nullptr;
    std::atomic_uint64_t               last_submitted_timeline{0};
    GraphEventRef                      completion_boundary{nullptr};
    UnorderedMap<Swapchain*, GraphEventRef> completion_boundary_by_swapchain{};
    LockFreeQueueBase<VulkanPresentor, false> presentors{};
};

SubmissionPresentContext::SubmissionPresentContext(EQueueType in_queue_type) :
    impl(MakeUnique<Impl>(in_queue_type)) {}

SubmissionPresentContext::~SubmissionPresentContext() = default;

SubmissionPresentResult SubmissionPresentContext::Present(
    const ExecutorPresentOp&   present_op,
    std::span<const WaitEvent> wait_events,
    const ResourceStateValue*  source_texture_state,
    SubmissionHostFence&       out_host_fence
) {
    SubmissionPresentResult present_result{};
    out_host_fence.Reset();
    if (!present_op.swapchain || !present_op.target.texture) {
        return present_result;
    }

    auto* swapchain = ResourceCast(present_op.swapchain.Get());
    if (swapchain == nullptr) {
        return present_result;
    }

    const bool use_present_fence =
        impl->command_queue.vk_device.GetOptionalExtensions().m_has_khr_swapchain_maintenance1;

    impl->WaitForReusablePresentSlot(*swapchain);

    auto  presentor = impl->AcquirePresentor();
    auto& cmd_list  = presentor->GetCmdList();
    auto& tracker   = presentor->GetTracker();

    auto [ready_semaphore, image_index, present_index] = swapchain->AquireNextImage();
    if (image_index == UINT32_MAX) {
        presentor->Reset();
        impl->presentors.Push(presentor.release());
        return present_result;
    }

    auto* src_texture       = static_cast<VulkanTexture*>(present_op.target.texture);
    auto* swapchain_texture = ResourceCast(swapchain->GetSwapchainImage(image_index).texture);

    cmd_list.Begin();
    cmd_list.BeginLabel(MOER_TEXT("Submission Present"), {0.0f, 1.0f, 1.0f, 1.0f});
    tracker.SetPassType(EPassType::Graphics);

    TrackerSeed        present_seed{};
    TrackerSeedTextureEntry source_seed{};
    source_seed.texture     = src_texture;
    source_seed.mip_level   = static_cast<uint8_t>(present_op.target.mip_level);
    source_seed.mip_count   = 1;
    source_seed.array_layer = static_cast<uint8_t>(present_op.target.array_layer);
    source_seed.array_count = 1;
    if (source_texture_state != nullptr) {
        source_seed.known         = source_texture_state->known;
        source_seed.has_writer    = source_texture_state->has_writer;
        source_seed.owner_queue   = source_texture_state->owner_queue;
        source_seed.texture_state = source_texture_state->texture_state;
    }
    present_seed.textures.emplace_back(source_seed);
    tracker.InitFromSeed(present_seed);

    if (source_texture_state != nullptr && source_texture_state->known) {
        auto [src_access, src_layout, _src_stage] =
            ResolveTextureSeedState(src_texture, *source_texture_state);
        RHITRACE_RESOURCE_LOG(
            src_texture->GetName(),
            "[ResourceTrace][Present][SeedSrc] {} : known={} owner_queue={} state={} layout={} access=0x{:x}",
            src_texture->GetName(),
            source_texture_state->known,
            QueueTypeName(source_texture_state->owner_queue),
            int(source_texture_state->texture_state),
            VkLayoutStr(src_layout),
            uint64(src_access)
        );
    } else {
        RHITRACE_RESOURCE_LOG(
            src_texture->GetName(),
            "[ResourceTrace][Present][SeedSrc] {} : persistent state UNKNOWN",
            src_texture->GetName()
        );
    }
    auto src_to_transfer = tracker.ReadTexture(src_texture, ETextureState::TRANSFER_SRC);
    tracker.EmitLocalTransition(
        src_texture,
        static_cast<VkAccessFlagBits2>(std::get<0>(src_to_transfer)),
        std::get<1>(src_to_transfer),
        static_cast<VkPipelineStageFlagBits2>(std::get<2>(src_to_transfer)),
        static_cast<uint8_t>(present_op.target.mip_level),
        1,
        static_cast<uint8_t>(present_op.target.array_layer),
        1
    );
    tracker.EmitLocalTransition(
        swapchain_texture,
        tracker.WriteTexture(swapchain_texture, ETextureState::TRANSFER_DST)
    );
    tracker.ResolveBarriers();
    tracker.DispatchBarriers(cmd_list);

    cmd_list.InsertLabel(MOER_TEXT("Copy Present Image"), {0.0f, 0.0f, 0.0f, 1.0f});
    cmd_list.CopyTexture(
        src_texture,
        swapchain_texture,
        present_op.target.extent,
        {0, 0, 0},
        {0, 0, 0},
        0,
        0
    );
    tracker.EmitLocalTransition(
        swapchain_texture,
        VK_ACCESS_2_NONE,
        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        VK_PIPELINE_STAGE_2_COPY_BIT
    );
    tracker.ResolveBarriers();
    tracker.DispatchBarriers(cmd_list);
    cmd_list.EndLabel();
    cmd_list.End();
    tracker.Reset();

    const uint64 completion_value = ++impl->last_submitted_timeline;
    for (const auto& wait_event : wait_events) {
        auto* fence = reinterpret_cast<VulkanFence*>(wait_event.timeline_handle);
        if (fence == nullptr) {
            continue;
        }
        impl->native_queue.Wait(fence, wait_event.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }
    impl->native_queue.Wait(ready_semaphore, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    impl->native_queue.Signal(swapchain->GetRenderFinishedFence(), VK_PIPELINE_STAGE_2_COPY_BIT);
    impl->native_queue.Submit(cmd_list);

    VkSemaphore render_finished_semaphore = swapchain->GetRenderFinishedFence();
    VkFence     in_flight_fence           = VK_NULL_HANDLE;
    VkSwapchainPresentFenceInfoEXT present_fence_info{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
    if (use_present_fence) {
        in_flight_fence                  = swapchain->GetInFlightFence(present_index);
        present_fence_info.swapchainCount = 1;
        present_fence_info.pFences        = &in_flight_fence;
    }

    VkPresentInfoKHR present_info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present_info.pNext              = use_present_fence ? &present_fence_info : nullptr;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores    = &render_finished_semaphore;
    present_info.swapchainCount     = 1;
    present_info.pSwapchains        = &swapchain->handle;
    present_info.pImageIndices      = &image_index;

    VulkanQueueAccessScope present_queue_access_scope(
        impl->command_queue.vk_device.GetPresentQueue(),
        MOER_TEXT("vkQueuePresentKHR")
    );
    VkResult present_vk_result =
        vkQueuePresentKHR(impl->command_queue.vk_device.GetPresentQueue(), &present_info);
    if (present_vk_result != VK_SUCCESS && present_vk_result != VK_SUBOPTIMAL_KHR &&
        present_vk_result != VK_ERROR_OUT_OF_DATE_KHR) {
        LOG_ERROR(MOER_TEXT("vkQueuePresentKHR failed with result {}"), int(present_vk_result));
    }
    VkFence queue_completion_fence = VK_NULL_HANDLE;
    if (!use_present_fence) {
        queue_completion_fence = swapchain->GetInFlightFence(present_index);
    }
    impl->native_queue.Signal(impl->timeline.Get(), completion_value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    impl->native_queue.SubmitEmpty(queue_completion_fence);
    const VkFence host_completion_fence = use_present_fence ? in_flight_fence : queue_completion_fence;
    if (host_completion_fence != VK_NULL_HANDLE) {
        out_host_fence.handle = host_completion_fence;
        out_host_fence.owned  = false;
    }
    ++swapchain->image_idx;

    present_result.submitted         = true;
    present_result.completion        = WaitEvent{uint64(impl->timeline.Get()), completion_value};
    present_result.timeline_value    = completion_value;
    present_result.presentor         = std::move(presentor);
    return present_result;
}

void SubmissionPresentContext::Flush() {}

void SubmissionPresentContext::Shutdown() {}

void SubmissionPresentContext::ResolvePresentCompletion(
    UniquePtr<VulkanPresentor>&& presentor,
    uint64                       timeline_value
) {
    if (!presentor) {
        return;
    }
    presentor->VulkanAllocatorBase::Complete(impl->timeline.Get(), timeline_value);
    presentor->Reset();
    impl->presentors.Push(presentor.release());
}

void SubmissionPresentContext::AppendCompletionBoundary(Swapchain* swapchain, const GraphEventRef& completion_event) {
    impl->completion_boundary = ChainCompletionBoundary(impl->completion_boundary, completion_event);
    if (swapchain != nullptr) {
        impl->completion_boundary_by_swapchain[swapchain] = ChainCompletionBoundary(
            impl->completion_boundary_by_swapchain[swapchain],
            completion_event
        );
    }
}

void SubmissionPresentContext::ResetCompletionBoundary() {
    impl->completion_boundary = nullptr;
    impl->completion_boundary_by_swapchain.clear();
}

const GraphEventRef& SubmissionPresentContext::GetCompletionBoundary() const {
    return impl->completion_boundary;
}

const GraphEventRef& SubmissionPresentContext::GetCompletionBoundary(Swapchain* swapchain) const {
    static const GraphEventRef empty_boundary{nullptr};
    const auto iter = impl->completion_boundary_by_swapchain.find(swapchain);
    return iter == impl->completion_boundary_by_swapchain.end() ? empty_boundary : iter->second;
}

QueueRuntimeState& SubmissionQueueStateSet::Get(EQueueType queue) {
    return states[QueueTypeIndex(queue)];
}

const QueueRuntimeState& SubmissionQueueStateSet::Get(EQueueType queue) const {
    return states[QueueTypeIndex(queue)];
}

VulkanSubmissionRuntime::VulkanSubmissionRuntime(VulkanInterruptRuntime& in_interrupt_runtime) :
    interrupt_runtime(in_interrupt_runtime),
    graphics_queue_owner(
        static_cast<VkCommandQueue&>(RenderDevice::Get().GetCommandQueue(EQueueType::Graphics))
    ),
    compute_queue_owner(
        static_cast<VkCommandQueue&>(RenderDevice::Get().GetCommandQueue(EQueueType::Compute))
    ),
    copy_queue_owner(static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue())) {
    scheduler_state.queue_states.Get(EQueueType::Graphics).timeline_handle =
        graphics_queue_owner.GetTimelineHandle();
    scheduler_state.queue_states.Get(EQueueType::Compute).timeline_handle =
        compute_queue_owner.GetTimelineHandle();
    scheduler_state.queue_states.Get(EQueueType::Copy).timeline_handle =
        copy_queue_owner.GetTimelineHandle();

    submission_runnable = MoerNew(SubmissionRunnable)(*this);
    submission_thread   = RunnableThread::Create(
        submission_runnable,
        ThreadAttributes{.affinity = Affinity{}, .name = MOER_ASCII_TEXT("SubmissionThread")}
    );
}

VulkanSubmissionRuntime::~VulkanSubmissionRuntime() {
    Shutdown();
}

void VulkanSubmissionRuntime::Enqueue(Array<SubmitInfo>&& submits, Array<SubmitPresentStage>&& present_ops) {
    if (submits.empty() && present_ops.empty()) {
        return;
    }
    assert(b_enable.load(std::memory_order_acquire) && "Enqueue is not allowed after shutdown begins");
    if (!b_enable.load(std::memory_order_acquire)) {
        return;
    }
    SubmissionBatch batch{};
    batch.kind             = SubmissionBatch::EKind::Submit;
    batch.submits          = std::move(submits);
    batch.present_ops      = std::move(present_ops);
    batch.root_rhi_boundary.host_event = GraphEvent::CreateGraphEvent();
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
}

void VulkanSubmissionRuntime::Enqueue(
    Array<std::shared_ptr<TranslateBatch>>&& translate_batches,
    Array<SubmitPresentStage>&&              present_ops
) {
    if (translate_batches.empty() && present_ops.empty()) {
        return;
    }
    assert(b_enable.load(std::memory_order_acquire) && "Enqueue is not allowed after shutdown begins");
    if (!b_enable.load(std::memory_order_acquire)) {
        return;
    }
    SubmissionBatch batch{};
    batch.kind = SubmissionBatch::EKind::Submit;
    batch.translate_batches = std::move(translate_batches);
    batch.present_ops = std::move(present_ops);
    batch.root_rhi_boundary.host_event = GraphEvent::CreateGraphEvent();
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
}

GraphEventRef VulkanSubmissionRuntime::Sync(ERHISyncDepth depth) {
    return EnqueueOrderedSyncRequest(depth, SubmissionBatch::EKind::Sync);
}

GraphEventRef VulkanSubmissionRuntime::Sync(Swapchain* swapchain) {
    if (swapchain == nullptr) {
        return CreateCompletedEvent();
    }
    return EnqueueOrderedSwapchainSyncRequest(swapchain);
}

void VulkanSubmissionRuntime::Flush() {
    GraphEventRef completion =
        EnqueueOrderedSyncRequest(ERHISyncDepth::Present, SubmissionBatch::EKind::Flush);
    if (completion) {
        completion->Wait();
    }
    FlushPresentContexts();
}

void VulkanSubmissionRuntime::Drain() {
    assert(b_enable.load(std::memory_order_acquire) && "Drain is not allowed after shutdown begins");
    if (!b_enable.load(std::memory_order_acquire)) {
        return;
    }

    GraphEventRef wait_event = EnqueueDrainRequestUnchecked();
    wait_event->Wait();
}

GraphEventRef VulkanSubmissionRuntime::EnqueueOrderedSyncRequest(
    ERHISyncDepth           depth,
    SubmissionBatch::EKind  kind
) {
    assert(
        b_enable.load(std::memory_order_acquire) &&
        "Sync/Flush request is not allowed after shutdown begins"
    );
    if (!b_enable.load(std::memory_order_acquire)) {
        return CreateCompletedEvent();
    }
    return EnqueueOrderedSyncRequestUnchecked(depth, kind);
}

GraphEventRef VulkanSubmissionRuntime::EnqueueOrderedSyncRequestUnchecked(
    ERHISyncDepth           depth,
    SubmissionBatch::EKind  kind
) {
    assert(
        (kind == SubmissionBatch::EKind::Sync || kind == SubmissionBatch::EKind::Flush) &&
        "ordered sync request only supports Sync or Flush batches"
    );

    GraphEventRef batch_completion = GraphEvent::CreateGraphEvent();
    GraphEventRef wait_event       = CreateCompletionWaitEvent(batch_completion);
    SubmissionBatch batch{};
    batch.kind             = kind;
    batch.sync_depth       = depth;
    batch.completion_event = batch_completion;
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
    return wait_event;
}

GraphEventRef VulkanSubmissionRuntime::EnqueueOrderedSwapchainSyncRequest(Swapchain* swapchain) {
    assert(b_enable.load(std::memory_order_acquire) && "Swapchain sync request is not allowed after shutdown begins");
    if (!b_enable.load(std::memory_order_acquire)) {
        return CreateCompletedEvent();
    }

    GraphEventRef batch_completion = GraphEvent::CreateGraphEvent();
    GraphEventRef wait_event       = CreateCompletionWaitEvent(batch_completion);
    SubmissionBatch batch{};
    batch.kind             = SubmissionBatch::EKind::Sync;
    batch.sync_depth       = ERHISyncDepth::Present;
    batch.sync_swapchain   = swapchain;
    batch.completion_event = batch_completion;
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
    return wait_event;
}

GraphEventRef VulkanSubmissionRuntime::EnqueueDrainRequestUnchecked() {
    GraphEventRef batch_completion = GraphEvent::CreateGraphEvent();
    SubmissionBatch batch{};
    batch.kind             = SubmissionBatch::EKind::Drain;
    batch.completion_event = batch_completion;
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
    return batch_completion;
}

void VulkanSubmissionRuntime::Shutdown() {
    bool expected_enabled = true;
    if (!b_enable.compare_exchange_strong(expected_enabled, false, std::memory_order_acq_rel)) {
        JoinSubmissionThread();
        return;
    }

    GraphEventRef completion =
        EnqueueOrderedSyncRequestUnchecked(ERHISyncDepth::Present, SubmissionBatch::EKind::Flush);
    if (completion) {
        completion->Wait();
    }
    FlushPresentContexts();
    JoinSubmissionThread();
    ShutdownPresentContexts();
    ResetOwnerCompletionBoundaries();
}

void VulkanSubmissionRuntime::JoinSubmissionThread() {
    submission_cv.notify_all();
    if (submission_thread != nullptr) {
        MoerDelete(submission_thread);
        submission_thread = nullptr;
    }
    if (submission_runnable != nullptr) {
        MoerDelete(submission_runnable);
        submission_runnable = nullptr;
    }
}

void VulkanSubmissionRuntime::BindSubmitBatchRootBoundary(SubmissionBatch& batch) {
    assert(batch.kind == SubmissionBatch::EKind::Submit && "root boundary binding only applies to submit batches");

    if (!batch.root_rhi_boundary.host_event) {
        batch.root_rhi_boundary.host_event = GraphEvent::CreateGraphEvent();
    }

    if (pending_rhi_boundary.host_event) {
        batch.root_rhi_boundary.host_event->WaitUntil(pending_rhi_boundary.host_event);
    }
    batch.root_rhi_boundary.gpu_waits = pending_rhi_boundary.gpu_waits;
    batch.root_rhi_boundary.host_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
}

OrderedBatchRuntimeState VulkanSubmissionRuntime::InitOrderedBatchRuntimeState(SubmissionBatch& batch) {
    OrderedBatchRuntimeState state{};
    state.submits            = std::move(batch.submits);
    state.root_rhi_boundary  = std::move(batch.root_rhi_boundary);
    state.cache.resize(state.submits.size());
    return state;
}

void VulkanSubmissionRuntime::MaterializeTranslateBatches(SubmissionBatch& batch) {
    if (batch.translate_batches.empty()) {
        return;
    }

    Array<SubmitInfo> submit_infos{};
    for (const std::shared_ptr<TranslateBatch>& translate_batch : batch.translate_batches) {
        if (!translate_batch) {
            continue;
        }

        if (GraphEventRef boundary = translate_batch->translate_pipe.Close(); boundary) {
            boundary->Wait(EThread::UNKNOWN_THREAD);
        }

        std::lock_guard<std::mutex> batch_lock(translate_batch->mutex);
        std::optional<size_t> last_submit_info_index{};
        submit_infos.reserve(submit_infos.size() + translate_batch->entries.size());
        for (TranslateBatchEntry& entry : translate_batch->entries) {
            if (!entry.submit.has_value()) {
                continue;
            }

            SubmitInfo submit_info = std::move(entry.submit.value());
            if (!submit_info.translate_result.translate_complete) {
                submit_info.translate_result.translate_complete = entry.translate_event;
            }
            submit_info.submit_seq = next_submit_seq++;
            submit_infos.emplace_back(std::move(submit_info));
            last_submit_info_index = submit_infos.size() - 1;
        }

        if (last_submit_info_index.has_value() && translate_batch->allocator_cache != nullptr) {
            SubmitInfo& last_submit = submit_infos[last_submit_info_index.value()];
            if (last_submit.translate_result.recorded_submit.has_value()) {
                last_submit.translate_result.recorded_submit->allocator_owner = std::move(translate_batch->allocator_cache);
            }
        }
    }

    batch.submits = std::move(submit_infos);
    batch.translate_batches.clear();
}

void VulkanSubmissionRuntime::RunOrderedSubmitBatch(SubmissionBatch& batch) {
    auto& thread_heartbeat = VulkanThreadHeartbeat::Get();
    thread_heartbeat.PulseCurrent(MOER_TEXT("RunOrderedSubmitBatch.MaterializeTranslateBatches"));
    MaterializeTranslateBatches(batch);
    OrderedBatchRuntimeState state = InitOrderedBatchRuntimeState(batch);
    while (state.next_submit_index < state.submits.size()) {
        thread_heartbeat.PulseCurrent(MOER_TEXT("RunOrderedSubmitBatch.ScanBatchReadiness"));
        ScanBatchReadiness(state);
        const uint32 old_submit_index = state.next_submit_index;
        thread_heartbeat.PulseCurrent(MOER_TEXT("RunOrderedSubmitBatch.FlushPendingReady"));
        const uint32 flushed_count    = FlushPendingReady(state);
        if (state.next_submit_index == old_submit_index && flushed_count == 0) {
            LOG_ERROR(
                MOER_TEXT("Submission runtime made no ordered progress at submit index {} of {}"),
                state.next_submit_index,
                state.submits.size()
            );
            assert(false && "ordered submission batch must make forward progress");
            return;
        }
    }

    RootRhiBoundary batch_tail_boundary = state.root_rhi_boundary;
    if (!state.submits.empty()) {
        thread_heartbeat.PulseCurrent(MOER_TEXT("RunOrderedSubmitBatch.BuildBatchTailBoundary"));
        batch_tail_boundary = BuildBatchTailBoundary(state);
        batch_tail_boundary.host_event = state.root_rhi_boundary.host_event;
        pending_rhi_boundary           = batch_tail_boundary;
    }

    if (!batch.present_ops.empty()) {
        thread_heartbeat.PulseCurrent(MOER_TEXT("RunOrderedSubmitBatch.ExecutePresentStages"));
        ExecutePresentStages(batch.present_ops, batch_tail_boundary.gpu_waits);
    }
}

void VulkanSubmissionRuntime::ScanBatchReadiness(OrderedBatchRuntimeState& state) {
    for (uint32 submit_index = state.next_submit_index; submit_index < state.submits.size(); ++submit_index) {
        auto& cache = state.cache[submit_index];
        if (cache.submitted || cache.ready) {
            continue;
        }

        auto& submit = state.submits[submit_index];
        if (!submit.translate_result.valid) {
            LOG_ERROR(
                MOER_TEXT("Translate failed for ordered submit seq={} ({}, {}): {}"),
                submit.submit_seq,
                submit.key.op_seq,
                submit.key.submit_idx,
                submit.translate_result.error
            );
            assert(false && "ordered submit must have a valid translate result");
            return;
        }
        if (submit.translate_result.translate_complete &&
            !submit.translate_result.translate_complete->IsComplete()) {
            LOG_ERROR(
                MOER_TEXT("Translate completion is not resolved before ordered submit seq={} ({}, {})"),
                submit.submit_seq,
                submit.key.op_seq,
                submit.key.submit_idx
            );
            assert(false && "translate work must complete before submission runtime");
            return;
        }
        if (!submit.translate_result.recorded_submit.has_value()) {
            LOG_ERROR(
                MOER_TEXT("Missing recorded submit packet for ordered submit seq={} ({}, {})"),
                submit.submit_seq,
                submit.key.op_seq,
                submit.key.submit_idx
            );
            assert(false && "ordered submit must carry recorded submit payload");
            return;
        }

        if (submit.wait_syncpoints.empty() &&
            state.root_rhi_boundary.host_event != nullptr &&
            !state.root_rhi_boundary.host_event->IsComplete()) {
            continue;
        }

        Array<WaitEvent> resolved_waits{};
        if (!TryResolveWaitSyncPoints(submit.wait_syncpoints, resolved_waits)) {
            continue;
        }

        cache.resolved_waits = BuildResolvedWaits(state, submit, resolved_waits);
        cache.ready          = true;
    }
}

uint32 VulkanSubmissionRuntime::FlushPendingReady(OrderedBatchRuntimeState& state) {
    uint32 flushed_count = 0;
    while (state.next_submit_index < state.submits.size()) {
        const uint32 submit_index = state.next_submit_index;
        auto&        cache        = state.cache[submit_index];
        if (!cache.ready) {
            break;
        }

        auto& submit = state.submits[submit_index];
        auto& queue_state = scheduler_state.queue_states.Get(submit.queue);
        if (queue_state.timeline_handle == 0) {
            LOG_ERROR(
                MOER_TEXT("Queue runtime state is missing timeline handle for queue {}"),
                QueueTypeName(submit.queue)
            );
            assert(false && "queue runtime state must be initialized before submit");
            return flushed_count;
        }

        const uint64 signal_value = queue_state.next_signal_value;
        GraphEventRef task_completion_event = GraphEvent::CreateGraphEvent();
        for (const auto& event : submit.interrupt_completion_events) {
            if (event) {
                event->WaitUntil(task_completion_event);
            }
        }

        bool submitted = false;
        switch (submit.queue) {
            case EQueueType::Graphics:
            case EQueueType::Compute: {
                VulkanThreadHeartbeat::Get().PulseCurrent(MOER_TEXT("FlushPendingReady.SubmitRecorded.Graphics"));
                auto& queue = static_cast<VkCommandQueue&>(
                    RenderDevice::Get().GetCommandQueue(submit.queue)
                );
                auto& recorded_submit = submit.translate_result.recorded_submit.value();
                if (recorded_submit.payloads.empty()) {
                    if (task_completion_event) {
                        task_completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                    }
                    submitted = true;
                    break;
                }

                uint64 current_signal_value = signal_value;
                for (size_t payload_index = 0; payload_index < recorded_submit.payloads.size(); ++payload_index) {
                    auto runtime_payload = queue.SubmitPayloadForRuntime(
                        std::move(recorded_submit.payloads[payload_index]),
                        payload_index == 0 ? std::span<const WaitEvent>(cache.resolved_waits) : std::span<const WaitEvent>{},
                        current_signal_value
                    );
                    if (!runtime_payload->gpu_events.empty()) {
                        GPUEventStream::Get().EnqueueSubmit(
                            std::move(runtime_payload->gpu_events),
                            submit.queue,
                            runtime_payload->completion
                        );
                        if (runtime_payload->has_frame_boundary_event) {
                            GPUEventStream::Get().EndFrame();
                        }
                    }
                    runtime_payload->op_seq = submit.key.op_seq;
                    runtime_payload->pending_since = VulkanSubmitPayload::Clock::now();
                    runtime_payload->pending_warn_count = 0;
                    runtime_payload->completion_event =
                        payload_index + 1 == recorded_submit.payloads.size() ? task_completion_event : nullptr;
                    interrupt_runtime.EnqueuePayload(std::move(runtime_payload));
                    ++current_signal_value;
                }
                queue.AppendCompletionBoundary(task_completion_event);
                queue_state.next_signal_value = current_signal_value;
                submitted = true;
                break;
            }
            case EQueueType::Copy: {
                VulkanThreadHeartbeat::Get().PulseCurrent(MOER_TEXT("FlushPendingReady.SubmitRecorded.Copy"));
                auto& copy_queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
                auto& recorded_submit = submit.translate_result.recorded_submit.value();
                if (recorded_submit.payloads.empty()) {
                    if (task_completion_event) {
                        task_completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                    }
                    submitted = true;
                    break;
                }

                uint64 current_signal_value = signal_value;
                for (size_t payload_index = 0; payload_index < recorded_submit.payloads.size(); ++payload_index) {
                    auto runtime_payload = copy_queue.SubmitPayloadForRuntime(
                        std::move(recorded_submit.payloads[payload_index]),
                        payload_index == 0 ? std::span<const WaitEvent>(cache.resolved_waits) : std::span<const WaitEvent>{},
                        current_signal_value
                    );
                    if (!runtime_payload->gpu_events.empty()) {
                        GPUEventStream::Get().EnqueueSubmit(
                            std::move(runtime_payload->gpu_events),
                            submit.queue,
                            runtime_payload->completion
                        );
                        if (runtime_payload->has_frame_boundary_event) {
                            GPUEventStream::Get().EndFrame();
                        }
                    }
                    runtime_payload->op_seq = submit.key.op_seq;
                    runtime_payload->pending_since = VulkanSubmitPayload::Clock::now();
                    runtime_payload->pending_warn_count = 0;
                    runtime_payload->completion_event =
                        payload_index + 1 == recorded_submit.payloads.size() ? task_completion_event : nullptr;
                    interrupt_runtime.EnqueuePayload(std::move(runtime_payload));
                    ++current_signal_value;
                }
                copy_queue.AppendCompletionBoundary(task_completion_event);
                queue_state.next_signal_value = current_signal_value;
                submitted = true;
                break;
            }
            case EQueueType::Ignore:
            case EQueueType::Num:
            default:
                LOG_ERROR(
                    MOER_TEXT("Invalid queue type in ordered submit runtime: {}"),
                    QueueTypeName(submit.queue)
                );
                assert(false && "ordered submit must target a concrete queue");
                return flushed_count;
        }

        if (!submitted) {
            break;
        }

        PublishResolvedSyncPoint(
            submit.signal_syncpoint,
            WaitEvent{queue_state.timeline_handle, queue_state.next_signal_value - 1}
        );

        cache.submitted = true;
        state.next_submit_index += 1;
        ++flushed_count;
    }
    return flushed_count;
}

void VulkanSubmissionRuntime::ExecutePresentStages(
    const Array<SubmitPresentStage>& present_ops,
    std::span<const WaitEvent>       initial_waits
) {
    Array<WaitEvent> current_waits(initial_waits.begin(), initial_waits.end());
    for (const SubmitPresentStage& present_stage : present_ops) {
        const std::optional<WaitEvent> present_completion =
            ExecutePresentStage(present_stage, current_waits);
        if (present_completion.has_value()) {
            current_waits.clear();
            current_waits.emplace_back(present_completion.value());
        }
    }
}

std::optional<WaitEvent> VulkanSubmissionRuntime::ExecutePresentStage(
    const SubmitPresentStage& present_stage,
    std::span<const WaitEvent> wait_events
) {
    if (!present_stage.valid) {
        if (!present_stage.error.empty()) {
            LOG_ERROR(MOER_TEXT("{}"), present_stage.error);
        }
        assert(false && "present stage must be valid before runtime execution");
        return std::nullopt;
    }
    if (!present_stage.present.swapchain || !present_stage.present.target.texture) {
        return std::nullopt;
    }
    if (present_stage.present.queue == EQueueType::Copy ||
        present_stage.present.queue == EQueueType::Ignore) {
        LOG_ERROR(MOER_TEXT("Invalid present queue type: {}"), QueueTypeName(present_stage.present.queue));
        assert(false && "present must execute on a graphics-capable queue");
        return std::nullopt;
    }

    SubmissionHostFence host_fence{};
    host_fence.Reset();
    SubmissionPresentContext& context = GetOrCreatePresentContext(present_stage.present.queue);
    SubmissionPresentResult present_result = context.Present(
        present_stage.present,
        wait_events,
        present_stage.has_source_texture_state ? &present_stage.source_texture_state : nullptr,
        host_fence
    );
    if (!present_result.submitted) {
        return std::nullopt;
    }

    const WaitEvent present_completion = present_result.completion;
    GraphEventRef present_completion_event = GraphEvent::CreateGraphEvent();
    auto payload = MakeUnique<VulkanSubmitPayload>();
    payload->type = EVulkanSubmitPayloadType::Present;
    payload->queue_type = present_stage.present.queue;
    payload->present_context = &context;
    payload->presentor = std::move(present_result.presentor);
    payload->completion = present_completion;
    payload->timeline_value = present_result.timeline_value;
    payload->host_fence = host_fence.handle;
    payload->owns_host_fence = host_fence.owned;
    payload->op_seq = present_stage.op_seq;
    payload->pending_since = VulkanSubmitPayload::Clock::now();
    payload->completion_event = present_completion_event;
    interrupt_runtime.EnqueuePayload(std::move(payload));
    context.AppendCompletionBoundary(present_stage.present.swapchain.Get(), present_completion_event);
    return present_completion;
}

bool VulkanSubmissionRuntime::TryResolveWaitSyncPoints(
    std::span<const SyncPointRef> wait_syncpoints,
    Array<WaitEvent>&             out_resolved_waits
) const {
    out_resolved_waits.clear();
    out_resolved_waits.reserve(wait_syncpoints.size());
    for (const SyncPointRef& syncpoint : wait_syncpoints) {
        if (!syncpoint) {
            LOG_ERROR(MOER_TEXT("Ordered submit carries a null SyncPoint wait"));
            assert(false && "wait syncpoint must be valid");
            return false;
        }

        WaitEvent resolved_wait{};
        if (!syncpoint->TryGetResolvedWaitEvent(resolved_wait)) {
            return false;
        }
        out_resolved_waits.emplace_back(resolved_wait);
    }
    return true;
}

Array<WaitEvent> VulkanSubmissionRuntime::BuildResolvedWaits(
    const OrderedBatchRuntimeState&  state,
    const SubmitInfo&                submit,
    std::span<const WaitEvent>      resolved_waits
) const {
    Array<WaitEvent> wait_events = CollapseWaitEventsByTimelineMax(resolved_waits);
    if (submit.wait_syncpoints.empty() && RootRhiBoundaryHasGpuWaits(state.root_rhi_boundary)) {
        wait_events.insert(
            wait_events.end(),
            state.root_rhi_boundary.gpu_waits.begin(),
            state.root_rhi_boundary.gpu_waits.end()
        );
        wait_events = CollapseWaitEventsByTimelineMax(wait_events);
    }
    return wait_events;
}

void VulkanSubmissionRuntime::PublishResolvedSyncPoint(
    const SyncPointRef& syncpoint,
    WaitEvent           resolved_wait
) {
    if (!syncpoint) {
        LOG_ERROR(MOER_TEXT("Ordered submit carries a null SyncPoint signal"));
        assert(false && "signal syncpoint must be valid");
        return;
    }

    syncpoint->Publish(resolved_wait);
}

RootRhiBoundary VulkanSubmissionRuntime::BuildBatchTailBoundary(
    const OrderedBatchRuntimeState& state
) const {
    Array<WaitEvent> batch_tail_waits{};
    batch_tail_waits.reserve(state.submits.size());
    for (const SubmitInfo& submit : state.submits) {
        if (!submit.signal_syncpoint) {
            LOG_ERROR(
                MOER_TEXT("Batch tail is missing signal SyncPoint for ordered submit seq={} ({}, {})"),
                submit.submit_seq,
                submit.key.op_seq,
                submit.key.submit_idx
            );
            assert(false && "submitted work must carry a valid signal syncpoint");
            continue;
        }

        WaitEvent resolved_wait{};
        if (!submit.signal_syncpoint->TryGetResolvedWaitEvent(resolved_wait)) {
            LOG_ERROR(
                MOER_TEXT("Batch tail is missing published syncpoint for ordered submit seq={} ({}, {})"),
                submit.submit_seq,
                submit.key.op_seq,
                submit.key.submit_idx
            );
            assert(false && "submitted work must publish its exported syncpoint");
            continue;
        }
        batch_tail_waits.emplace_back(resolved_wait);
    }

    RootRhiBoundary boundary{};
    boundary.gpu_waits = CollapseWaitEventsByTimelineMax(batch_tail_waits);
    return boundary;
}

void VulkanSubmissionRuntime::RunSubmissionThread() {
    auto& thread_heartbeat = VulkanThreadHeartbeat::Get();
    auto  heartbeat_handle =
        thread_heartbeat.Register(MOER_TEXT("SubmissionThread"), MOER_TEXT("WaitBatch"));
    while (true) {
        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("WaitBatch"));
        SubmissionBatch batch{};
        {
            std::unique_lock<std::mutex> lock(submission_mutex);
            submission_cv.wait(lock, [this]() {
                return !b_enable.load(std::memory_order_acquire) || !submission_queue.empty();
            });
            if (!b_enable.load(std::memory_order_acquire) && submission_queue.empty()) {
                thread_heartbeat.Unregister(heartbeat_handle);
                return;
            }
            batch = std::move(submission_queue.front());
            submission_queue.pop_front();
        }
        assert(!BatchKindRequiresCompletionEvent(batch.kind) || batch.completion_event != nullptr);
        if (batch.kind == SubmissionBatch::EKind::Submit) {
            thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("RunOrderedSubmitBatch"));
            BindSubmitBatchRootBoundary(batch);
            RunOrderedSubmitBatch(batch);
        }

        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("AttachSyncDependencies"));
        AttachSyncDependencies(batch);
        thread_heartbeat.Pulse(heartbeat_handle, MOER_TEXT("FinishBatchCompletion"));
        FinishBatchCompletion(batch);
    }
}

SubmissionPresentContext& VulkanSubmissionRuntime::GetOrCreatePresentContext(EQueueType queue_type) {
    std::lock_guard<std::mutex> lock(present_context_mutex);
    auto& context = present_contexts[static_cast<size_t>(queue_type)];
    if (!context) {
        context = std::make_unique<SubmissionPresentContext>(queue_type);
    }
    return *context;
}

void VulkanSubmissionRuntime::FlushPresentContexts() {
    std::lock_guard<std::mutex> lock(present_context_mutex);
    for (auto& context : present_contexts) {
        if (context) {
            context->Flush();
        }
    }
}

void VulkanSubmissionRuntime::ShutdownPresentContexts() {
    std::lock_guard<std::mutex> lock(present_context_mutex);
    for (auto& context : present_contexts) {
        if (context) {
            context->Shutdown();
            context.reset();
        }
    }
}

void VulkanSubmissionRuntime::ResetOwnerCompletionBoundaries() {
    graphics_queue_owner.ResetCompletionBoundary();
    compute_queue_owner.ResetCompletionBoundary();
    copy_queue_owner.ResetCompletionBoundary();
}

void VulkanSubmissionRuntime::AttachSyncDependencies(SubmissionBatch& batch) {
    if (batch.kind != SubmissionBatch::EKind::Sync && batch.kind != SubmissionBatch::EKind::Flush) {
        return;
    }
    assert(batch.completion_event != nullptr && "sync-like batch must have a completion event");

    if (batch.sync_swapchain != nullptr) {
        std::lock_guard<std::mutex> lock(present_context_mutex);
        for (const auto& context : present_contexts) {
            if (context == nullptr) {
                continue;
            }
            if (const GraphEventRef& boundary = context->GetCompletionBoundary(batch.sync_swapchain); boundary) {
                batch.completion_event->WaitUntil(boundary);
            }
        }
        return;
    }

    if (const GraphEventRef& boundary = graphics_queue_owner.GetCompletionBoundary(); boundary) {
        batch.completion_event->WaitUntil(boundary);
    }
    if (const GraphEventRef& boundary = compute_queue_owner.GetCompletionBoundary(); boundary) {
        batch.completion_event->WaitUntil(boundary);
    }
    if (const GraphEventRef& boundary = copy_queue_owner.GetCompletionBoundary(); boundary) {
        batch.completion_event->WaitUntil(boundary);
    }

    if (ResolveBatchSyncDepth(batch) != ERHISyncDepth::Present) {
        return;
    }

    std::lock_guard<std::mutex> lock(present_context_mutex);
    for (const auto& context : present_contexts) {
        if (context == nullptr) {
            continue;
        }
        if (const GraphEventRef& boundary = context->GetCompletionBoundary(); boundary) {
            batch.completion_event->WaitUntil(boundary);
        }
    }
}

} // namespace Moer::Render
