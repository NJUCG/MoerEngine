#ifndef VULKAN_NRD_PLUGIN_H
#define VULKAN_NRD_PLUGIN_H

#include "rhi/plugin/NrdPlugin.h"

namespace Moer::Render {
class VulkanDevice;
}

namespace Moer::Render::Ext {

class VkNRDPlugin : public NRDPlugin {
public:
    VkNRDPlugin(VulkanDevice* _device) : m_device(_device) {}
    ~VkNRDPlugin() = default;

    SharedPtr<NRDInterface> CreateInterface(
        uint8  _max_frame_in_flight = 0,
        uint16 _frame_width         = 0,
        uint16 _frame_height        = 0
    ) override;

    SharedPtr<NRDInterface> RecreateInterface(
        SharedPtr<NRDInterface> _interface,
        uint16                  _frame_width  = 0,
        uint16                  _frame_height = 0
    ) override;

private:
    VulkanDevice* m_device;
};

}; // namespace Moer::Render::Ext
#endif // VULKAN_NRD_PLUGIN_H
