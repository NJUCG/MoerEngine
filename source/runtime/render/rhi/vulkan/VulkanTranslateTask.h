#pragma once

#include "VulkanQueue.h"
#include "taskgraph/GraphTask.h"

#include <optional>
#include <string>
#include <utility>

namespace Moer::Render {

struct TranslateTaskInput {
    EQueueType queue{EQueueType::Ignore};
    CmdSubmit  submit;
    TrackerSeed initial_seed{};

    TranslateTaskInput(EQueueType in_queue, CmdSubmit&& in_submit, TrackerSeed&& in_seed) :
        queue(in_queue),
        submit(std::move(in_submit)),
        initial_seed(std::move(in_seed)) {}

    TranslateTaskInput(TranslateTaskInput&&) noexcept            = default;
    TranslateTaskInput& operator=(TranslateTaskInput&&) noexcept = default;
    TranslateTaskInput(const TranslateTaskInput&)                = delete;
    TranslateTaskInput& operator=(const TranslateTaskInput&)     = delete;
};

struct TranslateResult {
    EQueueType queue{EQueueType::Ignore};
    std::optional<VulkanRecordedSubmit> recorded_submit{};
    GraphEventRef translate_complete{nullptr};
    bool          valid{true};
    std::string   error{};
};

class VulkanTranslateTask {
public:
    static TranslateResult Dispatch(TranslateTaskInput&& input);
    static TranslateResult MakeFailed(EQueueType queue, std::string error);
};

} // namespace Moer::Render
