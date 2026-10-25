#ifndef VULKAN_CUSTOM_COMMAND_H
#define VULKAN_CUSTOM_COMMAND_H

#include "../RHIImpl.h"

#include "VulkanPipelineArgument.h"
#include "VulkanPlatform.h"

namespace Moer::Render {

struct VkCustomDispatchCmd : public CustomDispatchCmd {
public:
    struct ResourceUsage {
        TArg               resource;
        VulkanArgumentInfo info;

        template<typename Arg>
            requires(std::is_constructible_v<TArg, Arg &&>)
        ResourceUsage(Arg&& argument, const VulkanArgumentInfo& argument_info) :
            resource(std::forward<Arg>(argument)),
            info(argument_info) {}
    };

    virtual std::span<const ResourceUsage> GetResourceUsages() const = 0;

    // Opt in only when the caller declares every public resource in RDG and
    // Execute synchronizes private work, returning public resources in their
    // declared pass states. Compiled recording never infers GetResourceUsages.
    virtual bool PreservesCompiledResourceStates() const { return false; }

    void IterateArgs(const std::function<void(const TArg&, ERHIAccessFlags)>& visit) const final {
        for (const auto& usage : GetResourceUsages()) {
            visit(usage.resource, usage.info.GetAccess());
        }
    }

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
