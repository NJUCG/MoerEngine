#pragma once

#include "VulkanTimeline.h"
#include "rhi/RHICommand.h"

#include <memory>
#include <optional>
#include <variant>

namespace Moer::Render {
class VulkanDevice;

// Accepted scheduled state, owned by the resource itself. Texture intervals
// address individual aspect/mip/layer cells; buffer intervals address bytes.
// It owns no resource references and disappears with the resource.
struct VulkanResourceStateSnapshot {
    struct Range {
        uint64                      first{}, end{};
        BarrierState                state{};
        uint32                      family{~uint32{0}};
        uint32                      released_to{~uint32{0}};
        std::optional<BarrierState> release_source{};
        bool                        external{false};
        VulkanQueueClock            uses{}, writes{};
    };
    Array<Range> ranges;
};

// Checks explicit access/transition contracts without inferring dependencies
// or generating barriers. Submission is the sole owner. Changes are prepared
// before native submission and published without allocation only on success.
class RENDER_API VulkanResourceStateValidator {
public:
    using Instruction = std::variant<RHIResourceAccess, BarrierCreateInfo>;
    using Program     = Array<Instruction>;

    static Program Compile(const CmdSubmit& submit);
    VulkanResourceStateValidator(VulkanDevice& device, EQueueType queue, const Program& program, const CmdSubmit* submit = nullptr, VulkanTimeline* timeline = nullptr, uint64 point = 0);
    ~VulkanResourceStateValidator();
    VulkanResourceStateValidator(const VulkanResourceStateValidator&)            = delete;
    VulkanResourceStateValidator& operator=(const VulkanResourceStateValidator&) = delete;
    void                          Commit() noexcept;
    static bool                   ValidateExternalAccess(VulkanDevice& device, EQueueType queue, const RHIResourceAccess& access) noexcept;

private:
    using Snapshot = VulkanResourceStateSnapshot;
    VulkanDevice&                             device;
    EQueueType                                queue;
    VulkanQueueClock                          clock{};
    uint64                                    point{};
    Array<std::pair<VulkanTimeline*, uint64>> signals;
    // Candidate snapshots borrow their publication destinations. Pending
    // submission lifetime owns every referenced resource until after Commit.
    Array<std::pair<std::shared_ptr<const Snapshot>*, std::shared_ptr<Snapshot>>> changes;

    void Apply(const RHIResourceAccess& access, const BarrierCreateInfo* transition = nullptr);
};
} // namespace Moer::Render
