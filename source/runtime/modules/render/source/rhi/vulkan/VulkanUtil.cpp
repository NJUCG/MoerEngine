//
// Created by 74535 on 2023/10/1.
//

#include "rhi/vulkan/misc/VulkanMacroUtils.h"
#include "VulkanUtil.h"

#if defined(_WIN32)
#include <windows.h>
#endif

#include <vector>
#include <fstream>

namespace MoerEngine {
namespace RHI {
namespace Vulkan {

namespace Util {
    bool error_mode_silent = false;

    std::string ErrorString(VkResult error_code) {
        switch (error_code) {
#define STR(r) \
case VK_##r: return #r
            STR(NOT_READY);
            STR(TIMEOUT);
            STR(EVENT_SET);
            STR(EVENT_RESET);
            STR(INCOMPLETE);
            STR(ERROR_OUT_OF_HOST_MEMORY);
            STR(ERROR_OUT_OF_DEVICE_MEMORY);
            STR(ERROR_INITIALIZATION_FAILED);
            STR(ERROR_DEVICE_LOST);
            STR(ERROR_MEMORY_MAP_FAILED);
            STR(ERROR_LAYER_NOT_PRESENT);
            STR(ERROR_EXTENSION_NOT_PRESENT);
            STR(ERROR_FEATURE_NOT_PRESENT);
            STR(ERROR_INCOMPATIBLE_DRIVER);
            STR(ERROR_TOO_MANY_OBJECTS);
            STR(ERROR_FORMAT_NOT_SUPPORTED);
            STR(ERROR_SURFACE_LOST_KHR);
            STR(ERROR_NATIVE_WINDOW_IN_USE_KHR);
            STR(SUBOPTIMAL_KHR);
            STR(ERROR_OUT_OF_DATE_KHR);
            STR(ERROR_INCOMPATIBLE_DISPLAY_KHR);
            STR(ERROR_VALIDATION_FAILED_EXT);
            STR(ERROR_INVALID_SHADER_NV);
            // STR(ERROR_INCOMPATIBLE_SHADER_BINARY_EXT);
#undef STR
            default:
                return "UNKNOWN_ERROR";
        }
    }

    std::string PhysicalDeviceTypeString(VkPhysicalDeviceType type) {
        switch (type) {
#define STR(r) \
case VK_PHYSICAL_DEVICE_TYPE_##r: return #r
            STR(OTHER);
            STR(INTEGRATED_GPU);
            STR(DISCRETE_GPU);
            STR(VIRTUAL_GPU);
            STR(CPU);
#undef STR
            default: return "UNKNOWN_DEVICE_TYPE";
        }
    }

    SwapChainSupportDetails QuerySwapChainSupport(VkPhysicalDevice _gpu, VkSurfaceKHR _surface) {
        SwapChainSupportDetails details;

        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_gpu, _surface, &details.capabilities));

        uint32_t format_count = 0;
        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, nullptr));
        if (format_count > 0) {
            details.formats.resize(format_count);
            VK_CHECK_RESULT(vkGetPhysicalDeviceSurfaceFormatsKHR(_gpu, _surface, &format_count, details.formats.data()));
        }

        uint32_t present_mode_count = 0;
        VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(_gpu, _surface, &present_mode_count, nullptr));
        if (present_mode_count > 0) {
            details.present_modes.resize(present_mode_count);
            VK_CHECK_RESULT(vkGetPhysicalDeviceSurfacePresentModesKHR(_gpu, _surface, &present_mode_count, details.present_modes.data()));
        }

        return details;
    }

    VkBool32 GetSupportedDepthFormat(VkPhysicalDevice physical_device, VkFormat* depth_format) {
        // Since all depth formats may be optional, we need to find a suitable depth format to use
        // Start with the highest precision packed format
        std::vector<VkFormat> format_list = {
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM,
        };

        for (auto& format : format_list) {
            VkFormatProperties format_props;
            vkGetPhysicalDeviceFormatProperties(physical_device, format, &format_props);
            if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                *depth_format = format;
                return true;
            }
        }

        return false;
    }

    VkBool32 GetSupportedDepthStencilFormat(VkPhysicalDevice physical_device, VkFormat* depth_stencil_format) {
        std::vector<VkFormat> format_list = {
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D16_UNORM_S8_UINT,
        };

        for (auto& format : format_list) {
            VkFormatProperties format_props;
            vkGetPhysicalDeviceFormatProperties(physical_device, format, &format_props);
            if (format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                *depth_stencil_format = format;
                return true;
            }
        }

        return false;
    }

    VkBool32 FormatIsFilterable(VkPhysicalDevice physical_device, VkFormat format, VkImageTiling tiling) {
        VkFormatProperties format_props;
        vkGetPhysicalDeviceFormatProperties(physical_device, format, &format_props);

        if (tiling == VK_IMAGE_TILING_OPTIMAL)
            return format_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

        if (tiling == VK_IMAGE_TILING_LINEAR)
            return format_props.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

        return false;
    }

    VkBool32 FormatHasStencil(VkFormat format) {
        std::vector<VkFormat> stencil_formats = {
            VK_FORMAT_S8_UINT,
            VK_FORMAT_D16_UNORM_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
        };
        return std::find(stencil_formats.begin(), stencil_formats.end(), format) != std::end(stencil_formats);
    }

    void SetImageLayout(
        VkCommandBuffer         cmd_buffer,
        VkImage                 image,
        VkImageLayout           old_image_layout,
        VkImageLayout           new_image_layout,
        VkImageSubresourceRange subresource_range,
        VkPipelineStageFlags    src_stage_mask,
        VkPipelineStageFlags    dst_stage_mask) {}

    void SetImageLayout(
        VkCommandBuffer      cmd_buffer,
        VkImage              image,
        VkImageAspectFlags   aspect_mask,
        VkImageLayout        old_image_layout,
        VkImageLayout        new_image_layout,
        VkPipelineStageFlags src_stage_mask,
        VkPipelineStageFlags dst_stage_mask) {}

    void InsertImageMemoryBarrier(
        VkCommandBuffer         cmd_buffer,
        VkImage                 image,
        VkAccessFlags           src_access_mask,
        VkAccessFlags           dst_access_mask,
        VkImageLayout           old_image_layout,
        VkImageLayout           new_image_layout,
        VkPipelineStageFlags    src_stage_mask,
        VkPipelineStageFlags    dst_stage_mask,
        VkImageSubresourceRange subresource_range) {}

    void ExitFatal(const std::string& message, int32_t exit_code) {
#if defined(_WIN32)
        if (!error_mode_silent) {
            MessageBox(nullptr, message.c_str(), nullptr, MB_OK | MB_ICONERROR);
        }
#endif
        LOG_CRITICAL(message);
        exit(exit_code);
    }

    void ExitFatal(const std::string& message, VkResult result_code) {
        ExitFatal(message, static_cast<int32_t>(result_code));
    }

    VkShaderModule LoadShader(const char* file_name, VkDevice device) {
        std::ifstream is(file_name, std::ios::binary | std::ios::in | std::ios::ate);

        if (is.is_open()) {
            size_t size = is.tellg();
            is.seekg(0, std::ios::beg);
            char* shader_code = new char[size];
            is.read(shader_code, size);
            is.close();

            assert(size > 0);

            VkShaderModule           shader_module;
            VkShaderModuleCreateInfo module_create_info{};
            module_create_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            module_create_info.codeSize = size;
            module_create_info.pCode    = (uint32_t*)shader_code;

            VK_CHECK_RESULT(vkCreateShaderModule(device, &module_create_info, nullptr, &shader_module))

            delete[] shader_code;

            return shader_module;
        }
        ExitFatal("Could not open shader file \"" + std::string(file_name) + "\"", -1);
    }

    bool FileExists(const std::string& filename) {
        std::ifstream f(filename.c_str());
        return !f.fail();
    }

    uint32_t AlignedSize(uint32_t value, uint32_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }
}

}
}
}// namespace MoerEngine::RHI::Vulkan::Util