#pragma once

#include "VulkanDescriptorBufferAllocator.h"
#include <unordered_map>

namespace Moer::Render {
class Command;

// Commands are assigned to exactly one context at a time. Attaching immutable
// reservations to commands lets serial/parallel scheduling and discarded-work
// fallback use the same budget. GPU lifetime belongs to the submission lease.
struct VulkanDescriptorRecordingPlan {
    std::unordered_map<const Command*, VulkanDescriptorBufferAllocator::Recording> commands;
};
} // namespace Moer::Render
