//
// Created by 74535 on 2023/10/2.
//

#ifndef VULKAN_MACRO_UTILS_H
#define VULKAN_MACRO_UTILS_H

#include "log/LogSystem.h"

#define VK_CHECK_RESULT(f)                 \
    {                                      \
        VkResult res = (f);                \
                                           \
        if (res != VK_SUCCESS) {           \
            std::stringstream ss;          \
            ss << "Fatal : VkResult is \"" \
               << res                      \
               << "\" in " << __FILE__     \
               << " at line " << __LINE__  \
               << "\n";                    \
            LOG_CRITICAL(ss.str());        \
            assert(false);                 \
        }                                  \
    }

#define VK_CHECK_NULLPTR(ptr, msg, ...) \
    {                                   \
        if (ptr == nullptr) {           \
            LOG_CRITICAL(msg);          \
            __VA_ARGS__;                \
        }                               \
    }

#if defined(_DEBUG) || defined(DEBUG)
#include <vulkan/vk_enum_string_helper.h>
#define VK_TYPE_TO_STRING(type, value)  string_##type(value)
#define VK_FLAGS_TO_STRING(type, value) string_##type(value).c_str()
#else
#define VK_TYPE_TO_STRING(type, value)  std::to_string(static_cast<uint32_t>(value)).c_str()
#define VK_FLAGS_TO_STRING(type, value) std::to_string(static_cast<uint32_t>(value)).c_str()
#endif

#endif// VULKAN_MACRO_UTILS_H
