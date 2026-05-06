#pragma once

#include "VulkanSubmissionShared.h"

namespace Moer::Render {

class VulkanTranslateTask {
public:
    static TranslateResult DispatchSingle(
        EQueueType queue_type,
        CmdSubmit&& submit,
        TrackerSeed&& initial_seed
    );
    static Array<TranslateResult> DispatchBatch(Array<QueueTranslateInfo>&& inputs);
    static TranslateResult MakeFailed(EQueueType queue, String error);
    static void ResetSchedulerState();
};

} // namespace Moer::Render
