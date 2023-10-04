//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_WINDOWS_PLATFORM_H
#define VULKAN_WINDOWS_PLATFORM_H


#define VK_USE_PLATFORM_WIN32_KHR					1
#define VK_USE_PLATFORM_WIN32_KHX					1

#define	VULKAN_SHOULD_ENABLE_DRAW_MARKERS			(BUILD_DEBUG || BUILD_DEVELOPMENT)
#define VULKAN_USE_CREATE_WIN32_SURFACE				1
#define VULKAN_DYNAMICALLYLOADED					1
#define VULKAN_SHOULD_ENABLE_DESKTOP_HMD_SUPPORT	1
#define VULKAN_SIGNAL_UNIMPLEMENTED()				checkf(false, TEXT("Unimplemented vulkan functionality: %s"), StringCast<TCHAR>(__FUNCTION__).Get())
#define VULKAN_SUPPORTS_AMD_BUFFER_MARKER			1

#define VULKAN_RHI_RAYTRACING 						(RHI_RAYTRACING)
#define VULKAN_SUPPORTS_SCALAR_BLOCK_LAYOUT			(VULKAN_RHI_RAYTRACING)
#define VULKAN_SUPPORTS_MULTIVIEW					1	// needed for VULKAN_PCES31

#if VULKAN_RHI_RAYTRACING
#	define VK_API_VERSION						VK_API_VERSION_1_2
#else
#	define VK_API_VERSION						VK_API_VERSION_1_1
#endif // VULKAN_RHI_RAYTRACING

#if BUILD_DEBUG || BUILD_DEVELOPMENT
#	include "vk_enum_string_helper.h"
#	define VK_TYPE_TO_STRING(Type, Value) ANSI_TO_TCHAR(string_##Type(Value))
#	define VK_FLAGS_TO_STRING(Type, Value) ANSI_TO_TCHAR(string_##Type(Value).c_str())
#endif

// 32-bit windows has warnings on custom mem mgr callbacks
#define VULKAN_SHOULD_USE_LLM					(BUILD_DEBUG || BUILD_DEVELOPMENT) && !PLATFORM_32BITS

#define ENUM_VK_ENTRYPOINTS_PLATFORM_BASE(EnumMacro)

#define ENUM_VK_ENTRYPOINTS_PLATFORM_INSTANCE(EnumMacro)	\
	EnumMacro(PFN_vkCreateWin32SurfaceKHR, vkCreateWin32SurfaceKHR)

#define ENUM_VK_ENTRYPOINTS_OPTIONAL_PLATFORM_INSTANCE(EnumMacro)

// // and now, include the GenericPlatform class
// #include "../VulkanGenericPlatform.h"

// class FVulkanWindowsPlatform : public VulkanGenericPlatform {
// public:
//     static bool LoadVulkanLibrary();
//     static bool LoadVulkanInstanceFunctions(VkInstance inInstance);
//     static void FreeVulkanLibrary();

//     static void GetInstanceExtensions(FVulkanInstanceExtensionArray& OutExtensions);
//     static void GetInstanceLayers(std::vector<const char*>& OutLayers) {}
//     static void GetDeviceExtensions(FVulkanDevice* Device, FVulkanDeviceExtensionArray& OutExtensions);
//     static void GetDeviceLayers(std::vector<const char*>& OutLayers) {}

//     static void CreateSurface(void* WindowHandle, VkInstance Instance, VkSurfaceKHR* OutSurface);

//     // static bool SupportsDeviceLocalHostVisibleWithNoPenalty(EGpuVendorId VendorId);

//     static void WriteCrashMarker(const FOptionalVulkanDeviceExtensions& OptionalExtensions, VkCommandBuffer CmdBuffer, VkBuffer DestBuffer, const std::span<uint32_t>& Entries, bool bAdding);

// private:
//     static bool bAttemptedLoad;
// };

// typedef FVulkanWindowsPlatform FVulkanPlatform;



#endif // VULKAN_WINDOWS_PLATFORM_H
