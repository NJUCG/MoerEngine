#pragma once

#include "VulkanCommon.h"
#include "rhi/RHIResource.h"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace Moer::Render::VulkanRaytracingDetail {

// The engine instance and its native build record share the same stable slot.
inline RaytracingInstance& AddInstance(
    Array<RaytracingInstance>& instances,
    Array<VkAccelerationStructureInstanceKHR>& native_instances,
    Array<uint>& free_slots
) {
    const uint slot = free_slots.empty() ? uint(instances.size()) : free_slots.back();
    RaytracingInstance instance{.array_idx = slot, .instance_id = slot};
    if (free_slots.empty()) {
        // Reserve both arrays before changing their logical sizes.
        if (instances.size() == instances.capacity()) {
            instances.reserve(std::max<size_t>(1, instances.size() * 2));
        }
        if (native_instances.size() == native_instances.capacity()) {
            native_instances.reserve(std::max<size_t>(1, native_instances.size() * 2));
        }
        instances.emplace_back(instance);
        native_instances.emplace_back();
    } else {
        // IDs are const members; reconstruct the reused slot with the same IDs.
        std::destroy_at(&instances[slot]);
        std::construct_at(&instances[slot], instance);
        native_instances[slot] = {};
        free_slots.pop_back();
    }
    return instances[slot];
}

inline void FreeInstance(Array<RaytracingInstance>& instances, Array<uint>& free_slots, uint slot) {
    if (slot >= instances.size() || std::find(free_slots.begin(), free_slots.end(), slot) != free_slots.end()) {
        throw std::invalid_argument("Ray tracing instance slot is invalid or already free");
    }
    free_slots.push_back(slot);
    instances[slot].visible_mask = RTVM_NONE;
    instances[slot].geom = nullptr;
}

} // namespace Moer::Render::VulkanRaytracingDetail
