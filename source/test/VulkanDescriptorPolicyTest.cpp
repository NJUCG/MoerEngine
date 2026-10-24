#include "rhi/vulkan/VulkanDescriptorMode.h"

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
        require(SelectVulkanDescriptorMode({}) == Mode::Sets);
        require(SelectVulkanDescriptorMode({.buffer = true}) == Mode::Buffer);
        require(SelectVulkanDescriptorMode({.heap = true}) == Mode::Heap);
        require(SelectVulkanDescriptorMode({.heap = true, .buffer = true}) == Mode::Heap);
        // Push support changes the binding mechanism, never the storage capability.
        require(SelectVulkanDescriptorMode({.push = true}) == Mode::Sets);
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
