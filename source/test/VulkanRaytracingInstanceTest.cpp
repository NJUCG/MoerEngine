#include "rhi/vulkan/VulkanRaytracingInstance.h"

#include <iostream>
#include <stdexcept>

int main() {
    using namespace Moer;
    using namespace Moer::Render;
    using namespace Moer::Render::VulkanRaytracingDetail;
    Array<RaytracingInstance> instances;
    Array<uint> free_slots;
    auto check = [](bool value) { if (!value) throw std::runtime_error("instance slot contract failed"); };
    for (uint i = 0; i < 3000; ++i) {
        check(AddInstance(instances, free_slots).array_idx == i);
    }
    instances[5].custom_index = 99;
    FreeInstance(instances, free_slots, 5);
    check(instances[5].visible_mask == RTVM_NONE && instances[5].geom == nullptr);
    bool rejected = false;
    try { FreeInstance(instances, free_slots, 5); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
    auto& reused = AddInstance(instances, free_slots);
    check(&reused == &instances[5] && reused.array_idx == 5 && reused.instance_id == 5);
    check(reused.custom_index == 0 && reused.visible_mask == RTVM_ALL);
    check(instances.size() == 3000);
    check(AddInstance(instances, free_slots).array_idx == 3000);
    rejected = false;
    try { FreeInstance(instances, free_slots, 4000); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && free_slots.empty());
    std::cout << "Ray tracing instance growth, reuse and invalid free contracts passed\n";
}
