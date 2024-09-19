//
// Created by 74535 on 2023/10/1.
//

#include "misc/Crc32.h"

#include "VulkanUtil.h"
#include "vulkan/vulkan_core.h"
#include "VulkanMacroUtils.h"
#if defined(_WIN32)
#include <windows.h>
#endif

#include <fstream>

namespace Moer {
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

    uint32_t MemCrc32(const void* data, size_t data_size) {
        return crc32_8bytes(data, data_size);
    }
}

}
}
}// namespace Moer::RHI::Vulkan::Util