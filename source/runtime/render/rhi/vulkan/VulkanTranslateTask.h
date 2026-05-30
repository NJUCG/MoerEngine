#pragma once

#include "VulkanSubmissionShared.h"

namespace Moer::Render {

class VulkanTranslateTask {
public:
    static TranslateResult DispatchSingle(
        EQueueType queue_type,
        CmdSubmit&& submit,
        TrackerSeed&& initial_seed,
        VulkanAllocator* allocator_override = nullptr
    );
    static Array<TranslateResult> DispatchBatch(Array<QueueTranslateInfo>&& inputs);
    static TranslateResult MakeFailed(EQueueType queue, std::string error);
    static void ResetSchedulerState();
};

} // namespace Moer::Render
