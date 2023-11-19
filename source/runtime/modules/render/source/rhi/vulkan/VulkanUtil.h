//
// Created by 74535 on 2023/10/1.
//

#ifndef VULKAN_UTIL_H
#define VULKAN_UTIL_H

#include <vulkan/vulkan.h>

#include <vector>
#include <string>
namespace Moer {
namespace RHI {
namespace Vulkan {

namespace Util {
    struct SwapChainSupportDetails {
        VkSurfaceCapabilitiesKHR        capabilities;
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR>   present_modes;
    };
    /** @brief Disable message boxes on fatal errors */
    extern bool error_mode_silent;

    /** @brief Returns an error code as a string */
    std::string ErrorString(VkResult error_code);

    /** @brief Returns the device type as a string */
    std::string PhysicalDeviceTypeString(VkPhysicalDeviceType type);

    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface);

    // Selected a suitable supported depth format starting with 32 bit down to 16 bit
    // Returns false if none of the depth formats in the list is supported by the device
    VkBool32 GetSupportedDepthFormat(VkPhysicalDevice physical_device, VkFormat* depth_format);
    // Same as getSupportedDepthFormat but will only select formats that also have stencil
    VkBool32 GetSupportedDepthStencilFormat(VkPhysicalDevice physical_device, VkFormat* depth_stencil_format);

    // Returns tru a given format support LINEAR filtering
    VkBool32 FormatIsFilterable(VkPhysicalDevice physical_device, VkFormat format, VkImageTiling tiling);
    // Returns true if a given format has a stencil part
    VkBool32 FormatHasStencil(VkFormat format);

    // Put an image memory barrier for setting an image layout on the sub resource into the given command buffer
    void SetImageLayout(
        VkCommandBuffer         cmd_buffer,
        VkImage                 image,
        VkImageLayout           old_image_layout,
        VkImageLayout           new_image_layout,
        VkImageSubresourceRange subresource_range,
        VkPipelineStageFlags    src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VkPipelineStageFlags    dst_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    // Uses a fixed sub resource layout with first mip level and layer
    void SetImageLayout(
        VkCommandBuffer      cmd_buffer,
        VkImage              image,
        VkImageAspectFlags   aspect_mask,
        VkImageLayout        old_image_layout,
        VkImageLayout        new_image_layout,
        VkPipelineStageFlags src_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VkPipelineStageFlags dst_stage_mask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    /** @brief Insert an image memory barrier into the command buffer */
    void InsertImageMemoryBarrier(
        VkCommandBuffer         cmd_buffer,
        VkImage                 image,
        VkAccessFlags           src_access_mask,
        VkAccessFlags           dst_access_mask,
        VkImageLayout           old_image_layout,
        VkImageLayout           new_image_layout,
        VkPipelineStageFlags    src_stage_mask,
        VkPipelineStageFlags    dst_stage_mask,
        VkImageSubresourceRange subresource_range);

    // Display error message and exit on fatal error
    void ExitFatal(const std::string& message, int32_t exit_code);
    void ExitFatal(const std::string& message, VkResult result_code);

    // Load a SPIR-V shader (binary)
    VkShaderModule LoadShader(const char* file_name, VkDevice device);

    // Create a SPIR-V shader from code
    VkShaderModule CreateShaderModule(const std::vector<uint8_t>& _code, VkDevice device);

    /** @brief Checks if a file exists */
    bool FileExists(const std::string& filename);

    uint32_t AlignedSize(uint32_t value, uint32_t alignment);

    VkSurfaceFormatKHR SelectSurfaceFormat(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkFormat* request_formats, int request_formats_count, VkColorSpaceKHR request_color_space);
    VkPresentModeKHR   SelectPresentMode(VkPhysicalDevice physical_device, VkSurfaceKHR surface, const VkPresentModeKHR* request_modes, int request_modes_count);

    uint32_t MemCrc32(const void* data, size_t data_size);
}

}
}
}// namespace Moer::RHI::Vulkan::Util

#endif// !VULKAN_UTIL_H
