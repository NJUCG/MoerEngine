#ifndef VULKAN_NRD_EXTENSION_H
#define VULKAN_NRD_EXTENSION_H

#include "rhi/extension/NrdExtension.h"

#include "../VulkanRHIResource.h"

namespace Moer::Render::Ext {

    class VkNRDExtension : public NRDExtension, public VulkanDeviceObject {
    public:
        VkNRDExtension(VulkanDevice* _device) : VulkanDeviceObject(_device) {}
        ~VkNRDExtension() = default;

        UniquePtr<NRDInterface> CreateInterface(
            uint8  _max_frame_in_flight = 0,
            uint16 _frame_width         = 0,
            uint16 _frame_height        = 0) override;
    };

};// namespace Moer::Render::Ext
#endif//VULKAN_CUSTOM_COMMAND_H
