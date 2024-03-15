#ifndef VULKAN_BINDLESS_H
#define VULKAN_BINDLESS_H

#include "VulkanRHIResource.h"

enum class EVulkanBindlessSetType : uint16_t {
    Sampler = 0,
    StorageBuffer,
    UniformBuffer,
    StorageImage,
    SampledImage,
    StorageTexelBuffer,
    UniformTexelBuffer,
    AccelerationStructure,
    SingleUseUniformBuffer,
    Num,
    NumBits = 16
};

class VulkanBindlessManager : public VulkanDeviceObject {
public:
    VulkanBindlessManager(VulkanDevice* _device);
    ~VulkanBindlessManager();

    void Init();

    void Destroy();

private:

};

#endif
