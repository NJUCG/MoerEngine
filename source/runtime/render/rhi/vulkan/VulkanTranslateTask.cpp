#include "VulkanTranslateTask.h"

#include "VulkanDevice.h"
#include "log/LogSystem.h"
#include "taskgraph/TaskGraph.h"

#include <format>

namespace Moer::Render {

namespace {

GraphEventRef CreateCompletedTranslateEvent() {
    GraphEventRef event = GraphEvent::CreateGraphEvent();
    event->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return event;
}

TranslateResult DispatchSingleTranslate(
    EQueueType   queue_type,
    CmdSubmit&&  submit,
    TrackerSeed&& initial_seed
) {
    TranslateResult result{};
    result.queue              = queue_type;
    result.translate_complete = GraphEvent::CreateGraphEvent();

    switch (queue_type) {
        case EQueueType::Graphics:
        case EQueueType::Compute: {
            auto& queue = static_cast<VkCommandQueue&>(RenderDevice::Get().GetCommandQueue(queue_type));
            result.recorded_submit = queue.Translate(std::move(submit), nullptr, std::move(initial_seed));
            break;
        }
        case EQueueType::Copy: {
            auto& queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            result.recorded_submit = queue.Translate(std::move(submit), nullptr);
            break;
        }
        default:
            return VulkanTranslateTask::MakeFailed(
                queue_type,
                std::format(
                    "VulkanTranslateTask::DispatchBatch invalid queue: {}",
                    static_cast<uint32>(queue_type)
                )
            );
    }

    return result;
}

} // namespace

TranslateResult VulkanTranslateTask::DispatchSingle(
    EQueueType   queue_type,
    CmdSubmit&&  submit,
    TrackerSeed&& initial_seed
) {
    return DispatchSingleTranslate(queue_type, std::move(submit), std::move(initial_seed));
}

TranslateResult VulkanTranslateTask::MakeFailed(EQueueType queue, std::string error) {
    TranslateResult result{};
    result.queue              = queue;
    result.translate_complete = CreateCompletedTranslateEvent();
    result.valid              = false;
    result.error              = std::move(error);
    return result;
}

Array<TranslateResult> VulkanTranslateTask::DispatchBatch(Array<QueueTranslateInfo>&& inputs) {
    Array<TranslateResult> results{};
    results.reserve(inputs.size());

    if (inputs.empty()) {
        return results;
    }

    results.resize(inputs.size());
    GraphEventArray completion_events{};
    completion_events.reserve(inputs.size());

    for (size_t index = 0; index < inputs.size(); ++index) {
        auto& input = inputs[index];
        if (!input.completion_event) {
            input.completion_event = GraphEvent::CreateGraphEvent();
        }
        completion_events.emplace_back(input.completion_event);

        auto dispatch = LambdaTask::Create(
            [&, index]() mutable {
                QueueTranslateInfo& current = inputs[index];
                TranslateResult result = current.valid ?
                                             DispatchSingle(
                                                 current.queue,
                                                 std::move(current.submit),
                                                 std::move(current.initial_seed)
                                             ) :
                                             MakeFailed(current.queue, std::move(current.error));
                result.translate_complete = current.completion_event;
                if (!result.valid && !result.error.empty()) {
                    LOG_ERROR("{}", result.error);
                }
                results[index] = std::move(result);
            },
            EThread::AnyThread_NormalPri
        );

        if (!input.task_dependencies.empty()) {
            dispatch.Wait(std::move(input.task_dependencies));
        }
        dispatch.Next(input.completion_event).Dispatch();
    }

    TaskGraph::GetInterface().WaitUntilTasksComplete(completion_events, EThread::UNKNOWN_THREAD);
    return results;
}

void VulkanTranslateTask::ResetSchedulerState() {
    // No-op: translate ordering is now fully encoded by preprocess-generated graph events.
}

} // namespace Moer::Render
