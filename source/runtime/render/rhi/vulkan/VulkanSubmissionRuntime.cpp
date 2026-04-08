#include "VulkanSubmissionRuntime.h"

#include "VulkanInterruptRuntime.h"
#include "VulkanDevice.h"
#include "VulkanRHITrace.h"
#include "log/LogSystem.h"
#include "platform/Platform.h"
#include "rhi/RHIImpl.h"

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

VulkanSubmissionRuntime::VulkanSubmissionRuntime(VulkanInterruptRuntime& in_interrupt_runtime) :
    interrupt_runtime(in_interrupt_runtime),
    submission_thread([this]() { RunSubmissionThread(); }) {}

VulkanSubmissionRuntime::~VulkanSubmissionRuntime() {
    Stop();
}

void VulkanSubmissionRuntime::Enqueue(Array<SubmitInfo>&& submits, bool frame_end) {
    if (submits.empty() && !frame_end) {
        return;
    }
    SubmissionBatch batch{};
    batch.kind                  = SubmissionBatch::EKind::Submit;
    batch.submits                = std::move(submits);
    batch.root_rhi_prerequisites = SnapshotPendingRHITails();
    batch.frame_end              = frame_end;
    batch.batch_id               = next_batch_id.fetch_add(1, std::memory_order_relaxed);
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
    GraphEventRef completion = EnqueueOrderedSyncRequest(ERHISyncDepth::Present, SubmissionBatch::EKind::Flush);
    if (completion) {
        completion->Wait();
    }
    FlushPresentContexts();
}

void VulkanSubmissionRuntime::Drain() {
    if (!running.load(std::memory_order_acquire)) {
        return;
    }

    auto completion = std::make_shared<std::promise<void>>();
    auto future     = completion->get_future();

    SubmissionBatch batch{};
    batch.kind       = SubmissionBatch::EKind::Drain;
    batch.batch_id   = next_batch_id.fetch_add(1, std::memory_order_relaxed);
    batch.completion = completion;
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
    future.wait();
}

GraphEventRef VulkanSubmissionRuntime::EnqueueOrderedSyncRequest(
    ERHISyncDepth           depth,
    SubmissionBatch::EKind  kind
) {
    if (!running.load(std::memory_order_acquire)) {
        return CreateCompletedEvent();
    }

    auto completion = std::make_shared<std::promise<GraphEventRef>>();
    auto future     = completion->get_future();

    SubmissionBatch batch{};
    batch.kind            = kind;
    batch.batch_id        = next_batch_id.fetch_add(1, std::memory_order_relaxed);
    batch.sync_depth      = depth;
    batch.sync_completion = completion;
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        submission_queue.emplace_back(std::move(batch));
    }
    submission_cv.notify_one();
    GraphEventRef snapshot = future.get();
    return snapshot ? snapshot : CreateCompletedEvent();
}

void VulkanSubmissionRuntime::Shutdown() {
    Flush();
    Stop();
    if (submission_thread.joinable()) {
        submission_thread.join();
    }
    ShutdownPresentContexts();
}

void VulkanSubmissionRuntime::Stop() {
    bool expected_running = true;
    if (!running.compare_exchange_strong(expected_running, false)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(submission_mutex);
        for (auto& batch : submission_queue) {
            if (batch.sync_completion) {
                try {
                    batch.sync_completion->set_value(CreateCompletedEvent());
                } catch (const std::future_error&) {
                }
            }
            if (batch.completion) {
                try {
                    batch.completion->set_value();
                } catch (const std::future_error&) {
                }
            }
        }
        submission_queue.clear();
    }
    submission_cv.notify_all();
}

void VulkanSubmissionRuntime::RunSubmissionThread() {
    Platform::SetCurrentThreadName("SubmissionThread");
    while (true) {
        SubmissionBatch batch{};
        {
            std::unique_lock<std::mutex> lock(submission_mutex);
            submission_cv.wait(lock, [this]() {
                return !running.load(std::memory_order_acquire) || !submission_queue.empty();
            });
            if (!running.load(std::memory_order_acquire) && submission_queue.empty()) {
                return;
            }
            batch = std::move(submission_queue.front());
            submission_queue.pop_front();
        }

        UnorderedMap<SubmissionKey, WaitEvent, SubmissionKeyHash> completion_by_submit{};
        completion_by_submit.reserve(static_cast<uint32>(batch.submits.size()));
        uint32 submission_count = 0;
        uint32 present_count    = 0;
        for (auto& submit_info : batch.submits) {
            if (submit_info.is_root_submit && !batch.root_rhi_prerequisites.empty()) {
                submit_info.root_prerequisite_waits = batch.root_rhi_prerequisites;
            }

            bool submitted = false;
            if (!submit_info.translate_result.valid) {
                if (!submit_info.translate_result.error.empty()) {
                    LOG_ERROR(
                        "Translate failed for submit ({}, {}): {}",
                        submit_info.key.op_seq,
                        submit_info.key.submit_idx,
                        submit_info.translate_result.error
                    );
                }
            } else if (submit_info.translate_result.translate_complete &&
                       !submit_info.translate_result.translate_complete->IsComplete()) {
                LOG_ERROR(
                    "Translate completion is not resolved before submit ({}, {})",
                    submit_info.key.op_seq,
                    submit_info.key.submit_idx
                );
                submit_info.translate_result.valid = false;
            } else {
                if (!submit_info.root_prerequisite_waits.empty() &&
                    submit_info.translate_result.recorded_submit.has_value() &&
                    submit_info.translate_result.recorded_submit->submit.has_value()) {
                    auto& wait_events = submit_info.translate_result.recorded_submit->submit->wait_events;
                    wait_events.insert(
                        wait_events.end(),
                        submit_info.root_prerequisite_waits.begin(),
                        submit_info.root_prerequisite_waits.end()
                    );
                }

                for (const auto& dependency : submit_info.wait_submission_keys) {
                    const auto completion_it = completion_by_submit.find(dependency);
                    if (completion_it == completion_by_submit.end()) {
                        LOG_ERROR(
                            "Missing dependency completion for submit ({}, {})",
                            dependency.op_seq,
                            dependency.submit_idx
                        );
                        continue;
                    }
                    if (submit_info.translate_result.recorded_submit.has_value() &&
                        submit_info.translate_result.recorded_submit->submit.has_value()) {
                        submit_info.translate_result.recorded_submit->submit->wait_events.emplace_back(
                            completion_it->second
                        );
                    }
                }

                if (!submit_info.translate_result.recorded_submit.has_value()) {
                    LOG_ERROR(
                        "Missing recorded submit packet for ({}, {})",
                        submit_info.key.op_seq,
                        submit_info.key.submit_idx
                    );
                } else {
                    WaitEvent completion_event{};
                    switch (submit_info.translate_result.queue) {
                        case EQueueType::Graphics:
                        case EQueueType::Compute: {
                            auto& queue = static_cast<VkCommandQueue&>(
                                RenderDevice::Get().GetCommandQueue(submit_info.translate_result.queue)
                            );
                            GraphEventRef task_completion_event = GraphEvent::CreateGraphEvent();
                            for (const auto& event : submit_info.interrupt_completion_events) {
                                if (event) {
                                    event->WaitUntil(task_completion_event);
                                }
                            }
                            auto result = queue.SubmitRecordedForRuntime(
                                std::move(submit_info.translate_result.recorded_submit.value())
                            );
                            completion_event = result.completion;
                            if (result.scheduled_completion) {
                                interrupt_runtime.EnqueueQueueCompletion(
                                    submit_info.key.op_seq,
                                    &queue,
                                    task_completion_event,
                                    result.completion,
                                    result.timeline_value,
                                    std::move(result.allocator),
                                    std::move(result.callbacks),
                                    std::move(result.signal_events)
                                );
                                FoldSemanticTail(rhi_tail, task_completion_event);
                            } else if (task_completion_event) {
                                task_completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                            }
                            submitted = true;
                            break;
                        }
                        case EQueueType::Copy: {
                            auto& copy_queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
                            GraphEventRef task_completion_event = GraphEvent::CreateGraphEvent();
                            for (const auto& event : submit_info.interrupt_completion_events) {
                                if (event) {
                                    event->WaitUntil(task_completion_event);
                                }
                            }
                            auto result = copy_queue.SubmitRecordedForRuntime(
                                std::move(submit_info.translate_result.recorded_submit.value())
                            );
                            completion_event = WaitEvent{result.completion.handle, result.completion.timeline};
                            if (result.scheduled_completion) {
                                interrupt_runtime.EnqueueCopyQueueCompletion(
                                    submit_info.key.op_seq,
                                    &copy_queue,
                                    task_completion_event,
                                    result.completion,
                                    result.timeline_value,
                                    std::move(result.allocator),
                                    std::move(result.callbacks),
                                    std::move(result.signal_events)
                                );
                                FoldSemanticTail(rhi_tail, task_completion_event);
                            } else if (task_completion_event) {
                                task_completion_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
                            }
                            submitted = true;
                            break;
                        }
                        default:
                            LOG_ERROR(
                                "Invalid queue type in submission plan: {}",
                                QueueTypeName(submit_info.translate_result.queue)
                            );
                            break;
                    }

                    if (submitted) {
                        completion_by_submit.emplace(submit_info.key, completion_event);
                        ++submission_count;
                    }
                }
            }

            if (!submitted || !submit_info.present_stage.has_value()) {
                continue;
            }

            const SubmitPresentStage& present_stage = submit_info.present_stage.value();
            if (!present_stage.valid || !present_stage.present.swapchain || !present_stage.present.target.texture) {
                continue;
            }
            const auto submit_completion_it = completion_by_submit.find(submit_info.key);
            if (submit_completion_it == completion_by_submit.end()) {
                LOG_ERROR(
                    "Missing completion for present-attached submit ({}, {})",
                    submit_info.key.op_seq,
                    submit_info.key.submit_idx
                );
                continue;
            }
            if (present_stage.present.queue == EQueueType::Copy ||
                present_stage.present.queue == EQueueType::Ignore) {
                LOG_ERROR("Invalid present queue type: {}", QueueTypeName(present_stage.present.queue));
                continue;
            }

            Array<WaitEvent> wait_events{};
            wait_events.emplace_back(submit_completion_it->second);
            submit_info.host_fence.Reset();
            SubmissionPresentContext& context = GetOrCreatePresentContext(present_stage.present.queue);
            SubmissionPresentResult present_result = context.Present(
                    present_stage.present,
                    wait_events,
                    present_stage.has_source_texture_state ? &present_stage.source_texture_state : nullptr,
                    submit_info.host_fence
                );
            if (present_result.submitted) {
                GraphEventRef task_completion_event = GraphEvent::CreateGraphEvent();
                interrupt_runtime.EnqueuePresentCompletion(
                    present_stage.op_seq,
                    &context,
                    task_completion_event,
                    submit_info.host_fence.device,
                    submit_info.host_fence.handle,
                    submit_info.host_fence.owned,
                    present_result.timeline_value,
                    std::move(present_result.presentor)
                );
                FoldSemanticTail(present_tail, task_completion_event);
                ++present_count;
            }
        }

        if (!completion_by_submit.empty()) {
            Array<WaitEvent> batch_rhi_tails{};
            batch_rhi_tails.reserve(completion_by_submit.size());
            for (const auto& [_, completion] : completion_by_submit) {
                batch_rhi_tails.emplace_back(completion);
            }
            StorePendingRHITails(std::move(batch_rhi_tails));
        }

        if (batch.frame_end) {
            interrupt_runtime.EnqueueFrameEndMarker(batch.batch_id, submission_count, present_count);
        }
        if (batch.sync_completion) {
            GraphEventRef sync_event = CreateFrozenSyncEvent(
                batch.kind == SubmissionBatch::EKind::Flush ? ERHISyncDepth::Present : batch.sync_depth,
                rhi_tail,
                present_tail
            );
            try {
                batch.sync_completion->set_value(std::move(sync_event));
            } catch (const std::future_error&) {
            }
        }
        if (batch.completion) {
            try {
                batch.completion->set_value();
            } catch (const std::future_error&) {
            }
        }
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

GraphEventRef VulkanSubmissionRuntime::CreateCompletedEvent() {
    GraphEventRef completed = GraphEvent::CreateGraphEvent();
    completed->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return completed;
}

GraphEventRef VulkanSubmissionRuntime::CreateFrozenSyncEvent(
    ERHISyncDepth       depth,
    const GraphEventRef& rhi_tail_event,
    const GraphEventRef& present_tail_event
) {
    GraphEventRef frozen_sync_event = GraphEvent::CreateGraphEvent();
    if (rhi_tail_event) {
        frozen_sync_event->WaitUntil(rhi_tail_event);
    }
    if (depth == ERHISyncDepth::Present && present_tail_event) {
        frozen_sync_event->WaitUntil(present_tail_event);
    }
    frozen_sync_event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return frozen_sync_event;
}

void VulkanSubmissionRuntime::FoldSemanticTail(
    GraphEventRef&       tail,
    const GraphEventRef& completion_event
) {
    if (!completion_event) {
        return;
    }
    if (!tail) {
        tail = completion_event;
        return;
    }

    GraphEventRef folded = GraphEvent::CreateGraphEvent();
    folded->WaitUntil(tail);
    folded->WaitUntil(completion_event);
    folded->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    tail = folded;
}

Array<WaitEvent> VulkanSubmissionRuntime::SnapshotPendingRHITails() {
    std::lock_guard<std::mutex> lock(tail_mutex);
    return pending_rhi_tails;
}

void VulkanSubmissionRuntime::StorePendingRHITails(Array<WaitEvent>&& tails) {
    std::lock_guard<std::mutex> lock(tail_mutex);
    pending_rhi_tails = std::move(tails);
}

} // namespace Moer::Render
