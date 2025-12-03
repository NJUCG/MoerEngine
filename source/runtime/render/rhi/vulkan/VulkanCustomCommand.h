#ifndef VULKAN_CUSTOM_COMMAND_H
#define VULKAN_CUSTOM_COMMAND_H

#include "../RHIImpl.h"

#include "VulkanPlatform.h"

namespace Moer::Render {

struct VkCustomDispatchCmd : public CustomDispatchCmd {
public:
    struct VkDispatchContext {
        VkInstance       instance;
        VkPhysicalDevice gpu;
        VkDevice         device;
        VkCommandBuffer  cmd_list;
        void*            user_data;
    };

public:
    VkCustomDispatchCmd()  = default;
    ~VkCustomDispatchCmd() = default;

    virtual void Execute(const VkDispatchContext& _context) const = 0;
};

} // namespace Moer::Render

#endif