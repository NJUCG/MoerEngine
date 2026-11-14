#pragma once

#include "RenderAPI.h"
#include "rhi/backends/vulkan/VulkanFault.h"

namespace Moer::Render {
class VulkanDevice;

// Process-local validation scenario. Configure before creating the device and
// starting workers; the trigger values stay immutable until all work is drained.
struct VulkanFaultInjection {
    RENDER_API static void Configure(uint64 _present_trigger);
    RENDER_API static void ValidateCommandLine(int argc, const char** argv);
    RENDER_API static void ConfigureFromCommandLine(int argc, const char** argv);

    static uint64 GetPresentTrigger();
    static bool ShouldInjectPresentSubmit();
    static VulkanOperationResult InjectPresentSubmitFault(
        VulkanDevice& _device, const VulkanOperationContext& _context
    );
};
} // namespace Moer::Render
