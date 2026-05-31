#include "rhi/RHICommand.h"
#include "rhi/RHI.h"
#include "rhi/RHIExecutorBackend.h"
#include "rhi/RHIImpl.h"
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

bool IsFrameTickCommand(const Command* command) {
    if (command == nullptr || command->Type() != Command::EType::Custom) {
        return false;
    }
    const auto* custom_cmd = static_cast<const CustomCmd*>(command);
    return custom_cmd->CustomId() == CustomCmd::CustomCmdId::CUSTOM_FRAME_TICK;
}

bool BatchContainsFrameTick(const RHIBackendSubmissionBatch& batch) {
    for (const RHIBackendSubmissionBatchEntry& entry : batch.submits) {
        for (const UniquePtr<Command>& command : entry.submit.cmds) {
            if (IsFrameTickCommand(command.get())) {
                return true;
            }
        }
    }
    return false;
}

void EnableFrameProfiling(RHIBackendSubmissionBatch& batch) {
    if (!BatchContainsFrameTick(batch)) {
        return;
    }
    for (RHIBackendSubmissionBatchEntry& entry : batch.submits) {
        entry.submit.b_tick_profiling = true;
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
            pending_command_lists.emplace_back(PendingCommandListEntry(std::move(command_list)));
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
            pending_command_lists.emplace_back(PendingCommandListEntry(std::move(command_list)));
        }
    }

    if (EnumHasAnyFlag(flags, ERHIExecSubmitFlags::FlushGPU)) {
        Flush(ERHIFlushDepth::SubmitGPU);
    }
}

void RHIExecutor::Flush(ERHIFlushDepth depth) {
    std::shared_ptr<RHIBackendExecutor> backend;
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        EnqueuePendingLocked();
        backend = backend_executor;
    }
    if (backend) {
        backend->Flush(depth);
    }
}

void RHIExecutor::Sync(ERHISyncDepth depth) {
    GraphEventRef event = nullptr;
    std::shared_ptr<RHIBackendExecutor> backend;
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        EnqueuePendingLocked();
        backend = backend_executor;
    }
    if (backend) {
        event = backend->Sync(depth);
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
    std::shared_ptr<RHIBackendExecutor> backend;
    {
        std::lock_guard<std::mutex> lock(submit_mutex);
        EnqueuePendingLocked();
        backend = backend_executor;
    }
    if (backend) {
        event = backend->Sync(swapchain);
    }
    if (event) {
        event->Wait();
    }
}

void RHIExecutor::ShutDown() {
    auto& executor = Get();
    std::shared_ptr<RHIBackendExecutor> backend;
    {
        std::lock_guard<std::mutex> lock(executor.submit_mutex);
        executor.EnqueuePendingLocked();
        backend = std::move(executor.backend_executor);
        executor.backend_executor.reset();
    }
    if (backend) {
        backend->ShutDown();
    }
}

void RHIExecutor::EnqueuePendingLocked() {
    RHIBackendSubmissionBatch batch{};
    batch.submits.reserve(pending_command_lists.size());

    std::optional<EQueueType> last_non_empty_queue{};
    for (PendingCommandListEntry& entry : pending_command_lists) {
        CommandList* command_list = nullptr;
        if (entry.kind == PendingCommandListEntry::EKind::Recorded) {
            command_list = &entry.recorded;
        } else {
            if (!entry.recording) {
                continue;
            }
            if (GraphEventRef record_complete_event = entry.recording->GetRecordCompleteEvent();
                record_complete_event && !record_complete_event->IsComplete()) {
                record_complete_event->Wait(EThread::UNKNOWN_THREAD);
            }
            command_list = entry.recording.get();
        }

        if (command_list->IsEmpty()) {
            continue;
        }
        const EQueueType queue = command_list->GetQueueType();
        last_non_empty_queue   = queue;
        CmdSubmit submit = command_list->Submit();
        batch.submits.emplace_back(queue, std::move(submit));
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

    EnableFrameProfiling(batch);

    GetBackendExecutorLocked()->Enqueue(std::move(batch));
}

} // namespace Moer::Render
