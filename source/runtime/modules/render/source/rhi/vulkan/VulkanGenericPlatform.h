//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_GENERIC_PLATFORM_H
#define VULKAN_GENERIC_PLATFORM_H
//
//#include <string>
//#include <vector>
//#include <memory>
//#include <span>
//
//#include "vulkan.h"
//
//#include "RHI.h"
//
//struct OptionalVulkanDeviceExtensions;
//class VulkanDevice;
//class VulkanRenderTargetLayout;
//struct GfxPipelineDesc;
//class VulkanPhysicalDeviceFeatures;
//
//using FVulkanDeviceExtensionArray = std::vector<std::unique_ptr<class FVulkanDeviceExtension>>;
//using FVulkanInstanceExtensionArray = std::vector<std::unique_ptr<class FVulkanInstanceExtension>>;
//
//// the platform interface, and empty implementations for platforms that don't need em
//class VulkanGenericPlatform {
//public:
//    static bool LoadVulkanLibrary() { return true; }
//    static bool LoadVulkanInstanceFunctions(VkInstance inInstance);
//    static void ClearVulkanInstanceFunctions();
//    static void FreeVulkanLibrary() {}
//
//    // Array of required extensions for the platform (Required!)
//    static void GetInstanceExtensions(FVulkanInstanceExtensionArray& OutExtensions);
//    static void GetInstanceLayers(std::vector<const char*>& OutLayers) {}
//    static void GetDeviceExtensions(FVulkanDevice* Device, FVulkanDeviceExtensionArray& OutExtensions);
//    static void GetDeviceLayers(std::vector<const char*>& OutLayers) {}
//
//    // create the platform-specific surface object - required
//    static void CreateSurface(VkSurfaceKHR* OutSurface);
//
//    static void WriteCrashMarker(const FOptionalVulkanDeviceExtensions& OptionalExtensions, VkCommandBuffer CmdBuffer, VkBuffer DestBuffer, const std::span<uint32_t>& Entries, bool bAdding) {}
//};

#endif// VULKAN_GENERIC_PLATFORM_H
