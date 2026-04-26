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
    static TranslateResult MakeFailed(EQueueType queue, std::string error);
};

} // namespace Moer::Render
