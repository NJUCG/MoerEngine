#include "rhi/backends/vulkan/VulkanMacroUtils.h"

#include <iostream>

int main() {
    int calls = 0;
    bool continued = false;
    try {
        VK_CHECK_RESULT((++calls, VK_SUCCESS));
        VK_CHECK_RESULT((++calls, VK_ERROR_DEVICE_LOST));
        continued = true;
    } catch (const Moer::Render::VulkanCallError& error) {
        if (error.GetResult() != VK_ERROR_DEVICE_LOST || calls != 2 || continued) {
            return 1;
        }
        std::cout << "Vulkan failures stop execution and retain VkResult with assertions disabled\n";
        return 0;
    }
    return 1;
}
