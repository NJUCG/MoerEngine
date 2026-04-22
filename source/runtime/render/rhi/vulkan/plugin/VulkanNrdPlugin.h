#ifndef VULKAN_NRD_PLUGIN_H
#define VULKAN_NRD_PLUGIN_H

#include "rhi/plugin/NrdPlugin.h"

#include "../VulkanRHIResource.h"

namespace Moer::Render::Ext {

class VkNRDPlugin : public NRDPlugin, public VulkanDeviceObject {
public:
    VkNRDPlugin(VulkanDevice* _device) : VulkanDeviceObject(_device) {}
    ~VkNRDPlugin() = default;

    UniquePtr<NRDInterface> CreateInterface(
        uint8  _max_frame_in_flight = 0,
        uint16 _frame_width         = 0,
        uint16 _frame_height        = 0
    ) override;

    UniquePtr<NRDInterface> RecreateInterface(
        UniquePtr<NRDInterface> _interface,
        uint16                  _frame_width  = 0,
        uint16                  _frame_height = 0
    ) override;
};

}; // namespace Moer::Render::Ext
#endif // VULKAN_NRD_PLUGIN_H
