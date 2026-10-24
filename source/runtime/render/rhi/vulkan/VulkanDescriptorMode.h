#pragma once

#include <cstdint>

namespace Moer::Render {

// Device preference and pipeline binding mode are deliberately separate:
// ordinary shaders can use sets while bindless shaders use the device backend.
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

constexpr VulkanDescriptorMode SelectVulkanPipelineDescriptorMode(
    VulkanDescriptorMode preferred,
    bool                 bindless,
    bool                 requires_sets,
    uint32_t             heap_push_bytes,
    uint32_t             max_push_data
) {
    if (!bindless || requires_sets ||
        (preferred == VulkanDescriptorMode::Heap && heap_push_bytes > max_push_data)) {
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
