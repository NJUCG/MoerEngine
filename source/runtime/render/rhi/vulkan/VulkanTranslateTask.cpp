#include "VulkanTranslateTask.h"

#include "VulkanDevice.h"
#include "log/LogSystem.h"
#include "taskgraph/GraphTask.h"

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

} // namespace Moer::Render
