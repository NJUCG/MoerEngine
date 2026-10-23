#pragma once

namespace Moer::Render {

class VulkanDevice;

class VulkanDeviceObject {
public:
    VulkanDeviceObject(VulkanDevice* _device = nullptr) : m_device(_device) {}

protected:
    VulkanDevice* m_device;
    unsigned int  b_deferred_delete : 1 = true;
};

} // namespace Moer::Render
