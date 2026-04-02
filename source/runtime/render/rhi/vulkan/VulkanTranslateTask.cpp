#include "VulkanTranslateTask.h"

#include "VulkanDevice.h"
#include "log/LogSystem.h"

#include <format>

namespace Moer::Render {

TranslateResult VulkanTranslateTask::MakeFailed(EQueueType queue, std::string error) {
    TranslateResult result{};
    result.queue              = queue;
    result.translate_complete = GraphEvent::CreateGraphEvent();
    result.valid              = false;
    result.error              = std::move(error);
    result.translate_complete->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    return result;
}

TranslateResult VulkanTranslateTask::Dispatch(TranslateTaskInput&& input) {
    TranslateResult result{};
    result.queue              = input.queue;
    result.translate_complete = GraphEvent::CreateGraphEvent();

    switch (input.queue) {
        case EQueueType::Graphics:
        case EQueueType::Compute: {
            auto& queue = static_cast<VkCommandQueue&>(RenderDevice::Get().GetCommandQueue(input.queue));
            result.recorded_submit = queue.Translate(
                std::move(input.submit), nullptr, std::move(input.initial_seed)
            );
            break;
        }
        case EQueueType::Copy: {
            auto& queue = static_cast<VkCopyQueue&>(RenderDevice::Get().GetCopyQueue());
            result.recorded_submit = queue.Translate(std::move(input.submit), nullptr);
            break;
        }
        default: {
            result = MakeFailed(
                input.queue,
                std::format(
                    "VulkanTranslateTask::Dispatch invalid queue: {}",
                    static_cast<uint32>(input.queue)
                )
            );
            break;
        }
    }

    if (result.valid) {
        result.translate_complete->TryUnlockSubsequents(EThread::UNKNOWN_THREAD);
    } else if (!result.error.empty()) {
        LOG_ERROR("{}", result.error);
    }
    return result;
}

} // namespace Moer::Render
