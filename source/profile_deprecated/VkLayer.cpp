//仅仅作验证profiler的vkAllocateMemory用

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <iostream>
#include <string>
#include <mutex>
#include <map>

PFN_vkGetInstanceProcAddr g_NextGipa = nullptr;
PFN_vkGetDeviceProcAddr g_NextGdpa = nullptr;

std::atomic<VkDeviceSize> g_TotalAllocated{ 0 };
std::map<VkDeviceMemory, VkDeviceSize> g_MemoryMap;
std::mutex g_MemoryMutex;
std::atomic<VkDeviceSize> g_CurrentUsage{ 0 };

VKAPI_ATTR VkResult VKAPI_CALL Hook_AllocateMemory(
    VkDevice device,
    const VkMemoryAllocateInfo* pAllocateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDeviceMemory* pMemory)
{
    PFN_vkAllocateMemory pfn = (PFN_vkAllocateMemory)g_NextGdpa(device, "vkAllocateMemory");
    VkResult result = pfn(device, pAllocateInfo, pAllocator, pMemory);

    if (result == VK_SUCCESS && pMemory) {
        std::lock_guard<std::mutex> lock(g_MemoryMutex);
        g_MemoryMap[*pMemory] = pAllocateInfo->allocationSize;
        g_CurrentUsage += pAllocateInfo->allocationSize;

        std::cout << "[Moer Layer] + ALLOC: " << pAllocateInfo->allocationSize
            << " Bytes | Current Total: " << (g_CurrentUsage / 1024 / 1024) << " MB" << std::endl;
    }
    return result;
}

VKAPI_ATTR void VKAPI_CALL Hook_FreeMemory(
    VkDevice device,
    VkDeviceMemory memory,
    const VkAllocationCallbacks* pAllocator)
{
    if (memory != VK_NULL_HANDLE) {
        std::lock_guard<std::mutex> lock(g_MemoryMutex);

        auto it = g_MemoryMap.find(memory);
        if (it != g_MemoryMap.end()) {
            g_CurrentUsage -= it->second;
            std::cout << "[Moer Layer] - FREE: " << it->second
                << " Bytes | Current Total: " << (g_CurrentUsage / 1024 / 1024) << " MB" << std::endl;
            g_MemoryMap.erase(it);
        }
    }

    PFN_vkFreeMemory pfn = (PFN_vkFreeMemory)g_NextGdpa(device, "vkFreeMemory");
    pfn(device, memory, pAllocator);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Hook_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    std::string name(pName);

    if (name == "vkAllocateMemory") return (PFN_vkVoidFunction)Hook_AllocateMemory;
    if (name == "vkGetDeviceProcAddr") return (PFN_vkVoidFunction)Hook_GetDeviceProcAddr;
    if (name == "vkFreeMemory") return (PFN_vkVoidFunction)Hook_FreeMemory;

    return g_NextGdpa(device, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice)
{
    std::cout << "[Moer Layer] Hook_CreateDevice called" << std::endl;

    VkLayerDeviceCreateInfo* layerCreateInfo = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerDeviceCreateInfo*)layerCreateInfo->pNext;
    }

    if (!layerCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;

    g_NextGdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateDevice pfn = (PFN_vkCreateDevice)g_NextGipa(nullptr, "vkCreateDevice");
    return pfn(physicalDevice, pCreateInfo, pAllocator, pDevice);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance)
{
    std::cout << "[Moer Layer] Hook_CreateInstance called" << std::endl;

    VkLayerInstanceCreateInfo* layerCreateInfo = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO || layerCreateInfo->function != VK_LAYER_LINK_INFO)) {
        layerCreateInfo = (VkLayerInstanceCreateInfo*)layerCreateInfo->pNext;
    }

    if (!layerCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;

    g_NextGipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

    PFN_vkCreateInstance pfn = (PFN_vkCreateInstance)g_NextGipa(nullptr, "vkCreateInstance");
    return pfn(pCreateInfo, pAllocator, pInstance);
}

 VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    std::string name(pName);
    if (name == "vkCreateInstance") return (PFN_vkVoidFunction)Hook_CreateInstance;
    if (name == "vkCreateDevice") return (PFN_vkVoidFunction)Hook_CreateDevice;
    if (name == "vkGetInstanceProcAddr") return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (name == "vkGetDeviceProcAddr") return (PFN_vkVoidFunction)Hook_GetDeviceProcAddr;
    if (name == "vkFreeMemory") return (PFN_vkVoidFunction)Hook_FreeMemory;

    if (g_NextGipa) return g_NextGipa(instance, pName);
    return nullptr;
}

 VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return Hook_GetDeviceProcAddr(device, pName);
}