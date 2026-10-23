//
// Created by 74535 on 2023/10/2.
//

#ifndef VULKAN_MACRO_UTILS_H
#define VULKAN_MACRO_UTILS_H

#include "VulkanCommon.h"
#include <format>
#include <source_location>
#include <stdexcept>

namespace Moer::Render {
class VulkanCallError : public std::runtime_error {
public:
    VulkanCallError(VkResult result, const char* operation, std::source_location location) :
        std::runtime_error(std::format(
            "{} failed with VkResult {} at {}:{}", operation, int(result), location.file_name(), location.line()
        )),
        m_result(result) {}

    VkResult GetResult() const noexcept { return m_result; }

private:
    VkResult m_result;
};

// For operations requiring VK_SUCCESS. Expected statuses (retry/recreate) are
// classified by their callers instead of passing through this check.
inline void CheckVulkanResult(
    VkResult result, const char* operation,
    std::source_location location = std::source_location::current()
) {
    if (result != VK_SUCCESS) {
        throw VulkanCallError(result, operation, location);
    }
}
} // namespace Moer::Render

#define VK_CHECK_RESULT(f) ::Moer::Render::CheckVulkanResult((f), #f)

#if defined(_DEBUG) || defined(DEBUG)
#include <vulkan/vk_enum_string_helper.h>
#define VK_TYPE_TO_STRING(type, value)  string_##type(value)
#else
#define VK_TYPE_TO_STRING(type, value)  std::to_string(static_cast<uint32_t>(value)).c_str()
#endif

#endif // VULKAN_MACRO_UTILS_H
