#include "rhi/RHICommand.h"
#include "rhi/RHI.h"
#include "rhi/RHIExecutorBackend.h"
#include "Core.h"
#include "taskgraph/GraphTask.h"
#include "vulkan/VulkanSubmissionExecutor.h"

namespace Moer::Render {

namespace {

std::shared_ptr<RHIBackendExecutor> CreateBackendExecutor(ERHIType rhi_type) {
    switch (rhi_type) {
        case ERHIType::Vulkan:
            return std::make_shared<VulkanSubmissionExecutor>();
        case ERHIType::D3D12:
            LOG_ERROR(MOER_TEXT("RHIExecutor D3D12 backend executor is not implemented yet"));
            assert(false && "D3D12 backend executor is not implemented yet");
            return {};
        default:
            LOG_ERROR(MOER_TEXT("RHIExecutor got an unsupported RHI type"));
            assert(false && "Unsupported RHI type in RHIExecutor backend creation");
            return {};
    }
}

} // namespace

RHIExecutor& RHIExecutor::Get() {
    static RHIExecutor executor;
    return executor;
}

std::shared_ptr<RHIBackendExecutor> RHIExecutor::GetBackendExecutorLocked() {
    if (backend_executor) {
        return backend_executor;
    }

    backend_executor = CreateBackendExecutor(RenderDevice::Get().GetRHIType());
    return backend_executor;
}

std::shared_ptr<RHIBackendExecutor> RHIExecutor::TryGetBackendExecutorLocked() const {
    return backend_executor;
}

void RHIExecutor::Submit(
    Array<CommandList>&& command_lists,
    ERHIExecSubmitFlags  flags,
    RHIPresentRequest*   present
) {
    if (present != nullptr && !present->swapchain) {
        LOG_ERROR(MOER_TEXT("RHIExecutor::Submit got a null swapchain in RHIPresentRequest"));
        assert(false && "Present request swapchain must be valid");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(submit_mutex);

        for (size_t cmd_index = 0; cmd_index < command_lists.size(); ++cmd_index) {
            auto&            command_list = command_lists[cmd_index];
            const EQueueType queue_type   = command_list.GetQueueType();
            if (queue_type != EQueueType::Graphics && queue_type != EQueueType::Compute) {
                LOG_ERROR(
                    MOER_TEXT("RHIExecutor::Submit only accepts Graphics or Compute command lists, got={}"),
                    static_cast<uint32>(queue_type)
                );
                assert(false && "RHIExecutor only accepts Graphics or Compute command lists");
                return;
            }
            if (command_list.IsEmpty()) {
                continue;
            }
            pending_command_lists.emplace_back(std::move(command_list));
        }

        if (present != nullptr && present->source.texture != nullptr) {
            pending_present = *present;
        }
    }

    if (EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FlushGPU)) {
        Flush(ERHIFlushDepth::SubmitGPU);
    }
}

void RHIExecutor::SubmitRecording(
    Array<SharedPtr<CommandList>>&& command_lists,
    ERHIExecSubmitFlags             flags
) {
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        for (SharedPtr<CommandList>& command_list : command_lists) {
            if (!command_list) {
                continue;
            }
            const EQueueType queue_type = command_list->GetQueueType();
            if (queue_type != EQueueType::Graphics && queue_type != EQueueType::Compute) {
                LOG_ERROR(
                    MOER_TEXT("RHIExecutor::SubmitRecording only accepts Graphics or Compute command lists, got={}"),
                    static_cast<uint32>(queue_type)
                );
                assert(false && "RHIExecutor only accepts Graphics or Compute command lists");
                return;
            }
            pending_recording_command_lists.emplace_back(std::move(command_list));
        }
    }

    if (EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FlushGPU)) {
        Flush(ERHIFlushDepth::SubmitGPU);
    }
}

void RHIExecutor::Flush(ERHIFlushDepth depth) {
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        EnqueuePendingLocked();
        if (backend_executor) {
            backend_executor->Flush(depth);
        }
    }
}

void RHIExecutor::Sync(ERHISyncDepth depth) {
    GraphEventRef event = nullptr;
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        EnqueuePendingLocked();
        if (backend_executor) {
            event = backend_executor->Sync(depth);
        }
    }
    if (event) {
        event->Wait();
    }
}

void RHIExecutor::Sync(Swapchain* swapchain) {
    if (swapchain == nullptr) {
        LOG_ERROR(MOER_TEXT("RHIExecutor::Sync got a null swapchain"));
        assert(false && "Swapchain sync requires a valid swapchain");
        return;
    }

    GraphEventRef event = nullptr;
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        EnqueuePendingLocked();
        if (backend_executor) {
            event = backend_executor->Sync(swapchain);
        }
    }
    if (event) {
        event->Wait();
    }
}

void RHIExecutor::ShutDown() {
    auto& executor = Get();
    {
        std::lock_guard<std::mutex> lock(executor.submit_mutex);
        executor.EnqueuePendingLocked();
        if (executor.backend_executor) {
            executor.backend_executor->ShutDown();
            executor.backend_executor.reset();
        }
    }
}

void RHIExecutor::EnqueuePendingLocked() {
    for (SharedPtr<CommandList>& recording_command_list : pending_recording_command_lists) {
        if (!recording_command_list) {
            continue;
        }
        if (GraphEventRef record_complete_event = recording_command_list->GetRecordCompleteEvent();
            record_complete_event && !record_complete_event->IsComplete()) {
            record_complete_event->Wait(EThread::UNKNOWN_THREAD);
        }
        if (!recording_command_list->IsEmpty()) {
            pending_command_lists.emplace_back(std::move(*recording_command_list));
        }
    }
    pending_recording_command_lists.clear();

    RHIBackendSubmissionBatch batch{};
    batch.submits.reserve(pending_command_lists.size());

    std::optional<EQueueType> last_non_empty_queue{};
    for (auto& command_list : pending_command_lists) {
        if (command_list.IsEmpty()) {
            continue;
        }
        const EQueueType queue = command_list.GetQueueType();
        last_non_empty_queue   = queue;
        batch.submits.emplace_back(queue, command_list.Submit());
    }
    pending_command_lists.clear();

    if (pending_present.has_value()) {
        if (last_non_empty_queue.has_value() && last_non_empty_queue.value() != EQueueType::Graphics) {
            LOG_ERROR(MOER_TEXT("RHIExecutor::Submit requires the last command list before present to be Graphics"));
            assert(false && "Present requires the last command list to run on the Graphics queue");
            pending_present.reset();
            return;
        }
        batch.present = pending_present.value();
        pending_present.reset();
    }

    const bool has_work = !batch.submits.empty() || batch.present.has_value();
    if (!has_work) {
        return;
    }

    GetBackendExecutorLocked()->Enqueue(std::move(batch));
}

} // namespace Moer::Render
