#include "rhi/backends/vulkan/VulkanDescriptorMode.h"

#include <iostream>
#include <stdexcept>

using namespace Moer::Render;

int main() {
    try {
        const auto require = [](bool value) {
            if (!value)
                throw std::runtime_error("Descriptor selection contract failed");
        };
        using Mode = VulkanDescriptorMode;
        require(ResolveVulkanDescriptorMode({}) == Mode::Sets);
        require(ResolveVulkanDescriptorMode({.buffer = true}) == Mode::Buffer);
        require(ResolveVulkanDescriptorMode({.heap = true}) == Mode::Heap);
        require(ResolveVulkanDescriptorMode({.heap = true, .buffer = true}) == Mode::Heap);
        // Push support changes the binding mechanism, never the storage capability.
        require(ResolveVulkanDescriptorMode({.push = true}) == Mode::Sets);

        // Explicit requests take precedence even when a higher-priority backend is available.
        for (auto mode : {Mode::Sets, Mode::Buffer, Mode::Heap}) {
            require(ResolveVulkanDescriptorMode({.heap = true, .buffer = true}, mode) == mode);
        }
        require(ResolveVulkanDescriptorMode({}, Mode::Sets) == Mode::Sets);
        require(ResolveVulkanDescriptorMode({.buffer = true}, Mode::Buffer) == Mode::Buffer);
        require(ResolveVulkanDescriptorMode({.heap = true}, Mode::Heap) == Mode::Heap);

        const auto rejects_request = [](const VulkanDescriptorCapabilities& caps, Mode requested) {
            try {
                (void)ResolveVulkanDescriptorMode(caps, requested);
            } catch (const std::runtime_error&) {
                return true;
            }
            return false;
        };
        // An unavailable explicit request must not silently select another supported backend.
        require(rejects_request({}, Mode::Buffer));
        require(rejects_request({}, Mode::Heap));
        require(rejects_request({.heap = true}, Mode::Buffer));
        require(rejects_request({.buffer = true}, Mode::Heap));

        for (auto mode : {Mode::Sets, Mode::Buffer, Mode::Heap}) {
            require(SelectVulkanPipelineDescriptorMode(mode, true) == Mode::Sets);
            require(SelectVulkanPipelineDescriptorMode(mode, false) == mode);
        }
        std::cout << "[PASS] descriptor capability and pipeline fallback contracts\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
