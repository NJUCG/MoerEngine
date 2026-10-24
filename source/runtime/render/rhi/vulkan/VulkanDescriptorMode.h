#pragma once

#include <cstdint>

namespace Moer::Render {

// Pipelines use the device backend unless a concrete binding constraint requires
// a fallback. Bindless resource access does not determine descriptor storage.
enum class VulkanDescriptorMode : uint8_t {
    Sets,
    Buffer,
    Heap
};

struct VulkanDescriptorCapabilities {
    bool     heap                 = false;
    bool     buffer               = false;
    bool     push                 = false;
    bool     buffer_push          = false;
    uint32_t max_push_descriptors = 0;
    uint32_t max_push_data        = 0;
};

constexpr VulkanDescriptorMode SelectVulkanDescriptorMode(const VulkanDescriptorCapabilities& caps) {
    return caps.heap   ? VulkanDescriptorMode::Heap :
           caps.buffer ? VulkanDescriptorMode::Buffer :
                         VulkanDescriptorMode::Sets;
}

// Select a candidate from the shader interface; check heap push-data capacity
// after constructing the complete heap mapping.
constexpr VulkanDescriptorMode SelectVulkanPipelineDescriptorMode(
    VulkanDescriptorMode preferred,
    bool                 requires_sets
) {
    if (requires_sets) {
        return VulkanDescriptorMode::Sets;
    }
    return preferred;
}

constexpr const char* VulkanDescriptorModeName(VulkanDescriptorMode mode) {
    switch (mode) {
        case VulkanDescriptorMode::Heap:
            return "heap";
        case VulkanDescriptorMode::Buffer:
            return "buffer";
        default:
            return "sets";
    }
}

} // namespace Moer::Render
