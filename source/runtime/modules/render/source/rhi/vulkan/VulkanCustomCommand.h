#ifndef VULKAN_CUSTOM_COMMAND_H
#define VULKAN_CUSTOM_COMMAND_H

#include "../RHIImpl.h"

#include <volk.h>

namespace Moer::Render {

    struct VkCustomDispatchCmd : public CustomDispatchCmd {
    public:
        VkCustomDispatchCmd()  = default;
        ~VkCustomDispatchCmd() = default;

        virtual void Execute(VkCommandBuffer _cmd_list) const = 0;
    };

}// namespace Moer::Render

#endif