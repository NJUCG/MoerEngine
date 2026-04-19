#include "VulkanSubmissionRuntime.h"

#include "VulkanInterruptRuntime.h"
#include "VulkanDevice.h"
#include "VulkanRHITrace.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHIImpl.h"

#include <cassert>
#include <algorithm>
#include <chrono>
#include <thread>

namespace Moer::Render {

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
        std::lock_guard<std::mutex> lock(presentor_mutex);
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

        VkFence fence = swapchain.GetInFlightFence(present_index);
        while (true) {
            const VkResult result = vkGetFenceStatus(command_queue.vk_device.GetDevice(), fence);
            if (result == VK_SUCCESS) {
                vkResetFences(command_queue.vk_device.GetDevice(), 1, &fence);
                return;
            }
            if (result != VK_NOT_READY) {
                LOG_ERROR(
                    "vkGetFenceStatus failed on present fence slot {} with result {}",
                    present_index % swapchain.max_frames_in_flight,
                    int(result)
                );
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    EQueueType                         queue_type{EQueueType::Graphics};
    VkCommandQueue&                    command_queue;
    VkNativeQueue                      native_queue;
    VulkanFenceRef                     timeline = nullptr;
    std::atomic_uint64_t               last_submitted_timeline{0};
    std::mutex                         submit_mutex{};
    std::mutex                         presentor_mutex{};
    GraphEventRef                      completion_boundary{nullptr};
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

    std::unique_lock<std::mutex> queue_submit_lock(impl->command_queue.GetSubmitMutex());
    std::unique_lock<std::mutex> lock(impl->submit_mutex);
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
    cmd_list.BeginLabel("Submission Present", {0.0f, 1.0f, 1.0f, 1.0f});
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
    auto src_to_transfer = tracker.ReadTexture(src_texture, ETextureState::TRANSFER);
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
        tracker.WriteTexture(swapchain_texture, ETextureState::TRANSFER)
    );
    tracker.ResolveBarriers();
    tracker.DispatchBarriers(cmd_list);

    cmd_list.InsertLabel("Copy Present Image", {0.0f, 0.0f, 0.0f, 1.0f});
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
    impl->native_queue.Signal(impl->timeline.Get(), completion_value, VK_PIPELINE_STAGE_2_COPY_BIT);
    for (const auto& wait_event : wait_events) {
        auto* fence = reinterpret_cast<VulkanFence*>(wait_event.timeline_handle);
        if (fence == nullptr) {
            continue;
        }
        impl->native_queue.Wait(fence, wait_event.value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    }
    impl->native_queue.Wait(ready_semaphore, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    impl->native_queue.Signal(swapchain->GetRenderFinishedFence(), VK_PIPELINE_STAGE_2_COPY_BIT);
    VkFence submit_fence = VK_NULL_HANDLE;
    if (!use_present_fence) {
        submit_fence = swapchain->GetInFlightFence(present_index);
    }
    impl->native_queue.Submit(cmd_list, submit_fence);

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

    VkResult present_vk_result =
        vkQueuePresentKHR(impl->command_queue.vk_device.GetPresentQueue(), &present_info);
    if (present_vk_result != VK_SUCCESS && present_vk_result != VK_SUBOPTIMAL_KHR &&
        present_vk_result != VK_ERROR_OUT_OF_DATE_KHR) {
        LOG_ERROR("vkQueuePresentKHR failed with result {}", int(present_vk_result));
    }
    ++swapchain->image_idx;

    present_result.submitted         = true;
    present_result.completion        = WaitEvent{uint64(impl->timeline.Get()), completion_value};
    present_result.timeline_value    = completion_value;
    present_result.presentor         = std::move(presentor);
    out_host_fence.device            = impl->command_queue.vk_device.GetDevice();
    out_host_fence.handle            = use_present_fence ? in_flight_fence : submit_fence;
    out_host_fence.owned             = false;
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
    std::lock_guard<std::mutex> lock(impl->presentor_mutex);
    impl->presentors.Push(presentor.release());
}

void SubmissionPresentContext::AppendCompletionBoundary(const GraphEventRef& completion_event) {
    impl->completion_boundary = ChainCompletionBoundary(impl->completion_boundary, completion_event);
}

void SubmissionPresentContext::ResetCompletionBoundary() {
    impl->completion_boundary = nullptr;
}

const GraphEventRef& SubmissionPresentContext::GetCompletionBoundary() const {
    return impl->completion_boundary;
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
    copy_queue_owner(static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue())),
    submission_thread([this]() { RunSubmissionThread(); }) {
    scheduler_state.queue_states.Get(EQueueType::Graphics).timeline_handle =
        graphics_queue_owner.GetTimelineHandle();
    scheduler_state.queue_states.Get(EQueueType::Compute).timeline_handle =
        compute_queue_owner.GetTimelineHandle();
    scheduler_state.queue_states.Get(EQueueType::Copy).timeline_handle =
        copy_queue_owner.GetTimelineHandle();
}

VulkanSubmissionRuntime::~VulkanSubmissionRuntime() {
    Shutdown();
}

void VulkanSubmissionRuntime::Enqueue(Array<SubmitInfo>&& submits) {
    if (submits.empty()) {
        return;
    }
    assert(b_enable.load(std::memory_order_acquire) && "Enqueue is not allowed after shutdown begins");
    if (!b_enable.load(std::memory_order_acquire)) {
        return;
    }
    SubmissionBatch batch{};
    batch.kind             = SubmissionBatch::EKind::Submit;
    batch.submits          = std::move(submits);
    batch.root_rhi_boundary = SnapshotPendingRhiBoundary();
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
}

GraphEventRef VulkanSubmissionRuntime::Sync(ERHISyncDepth depth) {
    return EnqueueOrderedSyncRequest(depth, SubmissionBatch::EKind::Sync);
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

GraphEventRef VulkanSubmissionRuntime::EnqueueDrainRequestUnchecked() {
    GraphEventRef batch_completion = GraphEvent::CreateGraphEvent();
    GraphEventRef wait_event       = CreateCompletionWaitEvent(batch_completion);
    SubmissionBatch batch{};
    batch.kind             = SubmissionBatch::EKind::Drain;
    batch.completion_event = batch_completion;
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
    return wait_event;
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
    if (submission_thread.joinable()) {
        submission_thread.join();
    }
}

OrderedBatchRuntimeState VulkanSubmissionRuntime::InitOrderedBatchRuntimeState(SubmissionBatch& batch) {
    OrderedBatchRuntimeState state{};
    state.submits            = std::move(batch.submits);
    state.root_rhi_boundary  = std::move(batch.root_rhi_boundary);
    state.cache.resize(state.submits.size());
    return state;
}

void VulkanSubmissionRuntime::RunOrderedSubmitBatch(SubmissionBatch& batch) {
    OrderedBatchRuntimeState state = InitOrderedBatchRuntimeState(batch);
    while (state.next_submit_index < state.submits.size()) {
        ScanBatchReadiness(state);
        const uint32 old_submit_index = state.next_submit_index;
        const uint32 flushed_count    = FlushPendingReady(state);
        if (state.next_submit_index == old_submit_index && flushed_count == 0) {
            LOG_ERROR(
                "Submission runtime made no ordered progress at submit index {} of {}",
                state.next_submit_index,
                state.submits.size()
            );
            assert(false && "ordered submission batch must make forward progress");
            return;
        }
    }

    if (!state.submits.empty()) {
        StorePendingRhiBoundary(BuildBatchTailBoundary(state));
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
                "Translate failed for ordered submit seq={} ({}, {}): {}",
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
                "Translate completion is not resolved before ordered submit seq={} ({}, {})",
                submit.submit_seq,
                submit.key.op_seq,
                submit.key.submit_idx
            );
            assert(false && "translate work must complete before submission runtime");
            return;
        }
        if (!submit.translate_result.recorded_submit.has_value()) {
            LOG_ERROR(
                "Missing recorded submit packet for ordered submit seq={} ({}, {})",
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

        Array<ResolvedSyncPoint> resolved_syncpoints{};
        if (!TryResolveWaitSyncPoints(submit.wait_syncpoints, resolved_syncpoints)) {
            continue;
        }

        cache.resolved_waits = BuildResolvedWaits(state, submit, resolved_syncpoints);
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
                "Queue runtime state is missing timeline handle for queue {}",
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
                auto& queue = static_cast<VkCommandQueue&>(
                    RenderDevice::Get().GetCommandQueue(submit.queue)
                );
                auto result = queue.SubmitRecordedForRuntime(
                    std::move(submit.translate_result.recorded_submit.value()),
                    cache.resolved_waits,
                    signal_value
                );
                if (result.scheduled_completion) {
                    interrupt_runtime.EnqueueTask(SubmissionCompletionTask::Create(
                        submit.key.op_seq,
                        task_completion_event,
                        SubmissionQueueCompletionPayload(queue, std::move(result))
                    ));
                    queue.AppendCompletionBoundary(task_completion_event);
                } else if (task_completion_event) {
                    task_completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                }
                submitted = true;
                break;
            }
            case EQueueType::Copy: {
                auto& copy_queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
                auto result = copy_queue.SubmitRecordedForRuntime(
                    std::move(submit.translate_result.recorded_submit.value()),
                    cache.resolved_waits,
                    signal_value
                );
                if (result.scheduled_completion) {
                    interrupt_runtime.EnqueueTask(SubmissionCompletionTask::Create(
                        submit.key.op_seq,
                        task_completion_event,
                        SubmissionCopyQueueCompletionPayload(copy_queue, std::move(result))
                    ));
                    copy_queue.AppendCompletionBoundary(task_completion_event);
                } else if (task_completion_event) {
                    task_completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                }
                submitted = true;
                break;
            }
            case EQueueType::Ignore:
            case EQueueType::Num:
            default:
                LOG_ERROR(
                    "Invalid queue type in ordered submit runtime: {}",
                    QueueTypeName(submit.queue)
                );
                assert(false && "ordered submit must target a concrete queue");
                return flushed_count;
        }

        if (!submitted) {
            break;
        }

        queue_state.next_signal_value += 1;
        PublishResolvedSyncPoint(
            submit.signal_syncpoint,
            ResolvedSyncPoint{
                .queue = submit.queue,
                .timeline_handle = queue_state.timeline_handle,
                .value = signal_value,
            }
        );

        if (submit.present_stage.has_value()) {
            const SubmitPresentStage& present_stage = submit.present_stage.value();
            if (present_stage.valid && present_stage.present.swapchain && present_stage.present.target.texture) {
                if (present_stage.present.queue == EQueueType::Copy ||
                    present_stage.present.queue == EQueueType::Ignore) {
                    LOG_ERROR("Invalid present queue type: {}", QueueTypeName(present_stage.present.queue));
                    assert(false && "present must execute on a graphics-capable queue");
                    return flushed_count;
                }

                Array<WaitEvent> wait_events{};
                wait_events.emplace_back(WaitEvent{queue_state.timeline_handle, signal_value});
                submit.host_fence.Reset();
                SubmissionPresentContext& context = GetOrCreatePresentContext(present_stage.present.queue);
                SubmissionPresentResult present_result = context.Present(
                    present_stage.present,
                    wait_events,
                    present_stage.has_source_texture_state ? &present_stage.source_texture_state : nullptr,
                    submit.host_fence
                );
                if (present_result.submitted) {
                    GraphEventRef present_completion_event = GraphEvent::CreateGraphEvent();
                    interrupt_runtime.EnqueueTask(SubmissionCompletionTask::Create(
                        present_stage.op_seq,
                        present_completion_event,
                        SubmissionPresentCompletionPayload(
                            context,
                            submit.host_fence,
                            std::move(present_result)
                        )
                    ));
                    context.AppendCompletionBoundary(present_completion_event);
                }
            }
        }

        cache.submitted = true;
        state.next_submit_index += 1;
        ++flushed_count;
    }
    return flushed_count;
}

bool VulkanSubmissionRuntime::TryResolveWaitSyncPoints(
    std::span<const SyncPointId> wait_syncpoints,
    Array<ResolvedSyncPoint>&    out_resolved_waits
) const {
    out_resolved_waits.clear();
    out_resolved_waits.reserve(wait_syncpoints.size());
    for (const SyncPointId syncpoint_id : wait_syncpoints) {
        const auto iter = scheduler_state.resolved_syncpoints.find(syncpoint_id);
        if (iter == scheduler_state.resolved_syncpoints.end()) {
            return false;
        }
        out_resolved_waits.emplace_back(iter->second);
    }
    return true;
}

Array<WaitEvent> VulkanSubmissionRuntime::CollapseWaitsByTimelineMax(
    std::span<const ResolvedSyncPoint> resolved_waits
) {
    UnorderedMap<uint64, uint64> max_value_by_handle{};
    max_value_by_handle.reserve(resolved_waits.size());
    for (const ResolvedSyncPoint& wait : resolved_waits) {
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

Array<WaitEvent> VulkanSubmissionRuntime::CollapseWaitEventsByTimelineMax(
    std::span<const WaitEvent> wait_events
) {
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

Array<WaitEvent> VulkanSubmissionRuntime::BuildResolvedWaits(
    const OrderedBatchRuntimeState&  state,
    const SubmitInfo&                submit,
    std::span<const ResolvedSyncPoint> resolved_waits
) const {
    Array<WaitEvent> wait_events = CollapseWaitsByTimelineMax(resolved_waits);
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
    SyncPointId              syncpoint_id,
    const ResolvedSyncPoint& resolved_syncpoint
) {
    const auto [_, inserted] =
        scheduler_state.resolved_syncpoints.emplace(syncpoint_id, resolved_syncpoint);
    if (!inserted) {
        LOG_ERROR("SyncPoint {} was published more than once", syncpoint_id);
        assert(false && "syncpoint must only be published once");
    }
}

RootRhiBoundary VulkanSubmissionRuntime::BuildBatchTailBoundary(
    const OrderedBatchRuntimeState& state
) const {
    Array<WaitEvent> batch_tail_waits{};
    batch_tail_waits.reserve(state.submits.size());
    for (const SubmitInfo& submit : state.submits) {
        const auto syncpoint_iter = scheduler_state.resolved_syncpoints.find(submit.signal_syncpoint);
        if (syncpoint_iter == scheduler_state.resolved_syncpoints.end()) {
            LOG_ERROR(
                "Batch tail is missing published syncpoint for ordered submit seq={} ({}, {})",
                submit.submit_seq,
                submit.key.op_seq,
                submit.key.submit_idx
            );
            assert(false && "submitted work must publish its exported syncpoint");
            continue;
        }
        const ResolvedSyncPoint& resolved = syncpoint_iter->second;
        batch_tail_waits.emplace_back(WaitEvent{resolved.timeline_handle, resolved.value});
    }

    RootRhiBoundary boundary{};
    boundary.gpu_waits = CollapseWaitEventsByTimelineMax(batch_tail_waits);
    return boundary;
}

void VulkanSubmissionRuntime::RunSubmissionThread() {
    Platform::SetCurrentThreadName("SubmissionThread");
    while (true) {
        SubmissionBatch batch{};
        {
            std::unique_lock<std::mutex> lock(submission_mutex);
            submission_cv.wait(lock, [this]() {
                return !b_enable.load(std::memory_order_acquire) || !submission_queue.empty();
            });
            if (!b_enable.load(std::memory_order_acquire) && submission_queue.empty()) {
                return;
            }
            batch = std::move(submission_queue.front());
            submission_queue.pop_front();
        }
        assert(!BatchKindRequiresCompletionEvent(batch.kind) || batch.completion_event != nullptr);
        if (batch.kind == SubmissionBatch::EKind::Submit) {
            RunOrderedSubmitBatch(batch);
        }

        AttachSyncDependencies(batch);
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

GraphEventRef VulkanSubmissionRuntime::CreateCompletedEvent() {
    GraphEventRef completed = GraphEvent::CreateGraphEvent();
    completed->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return completed;
}

GraphEventRef VulkanSubmissionRuntime::CreateCompletionWaitEvent(const GraphEventRef& completion_event) {
    GraphEventRef wait_event = GraphEvent::CreateGraphEvent();
    if (completion_event) {
        wait_event->WaitUntil(completion_event);
    }
    wait_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return wait_event;
}

void VulkanSubmissionRuntime::FinishBatchCompletion(SubmissionBatch& batch) {
    if (!batch.completion_event) {
        assert(!BatchKindRequiresCompletionEvent(batch.kind) && "blocking batch must have a completion event");
        return;
    }
    batch.completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
}

RootRhiBoundary VulkanSubmissionRuntime::SnapshotPendingRhiBoundary() {
    std::lock_guard<std::mutex> lock(tail_mutex);
    return pending_rhi_boundary;
}

void VulkanSubmissionRuntime::StorePendingRhiBoundary(RootRhiBoundary&& boundary) {
    std::lock_guard<std::mutex> lock(tail_mutex);
    pending_rhi_boundary = std::move(boundary);
}

} // namespace Moer::Render
