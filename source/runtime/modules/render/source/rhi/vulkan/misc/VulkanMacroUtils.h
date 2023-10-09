//
// Created by 74535 on 2023/10/2.
//

#ifndef VULKAN_MACRO_UTILS_H
#define VULKAN_MACRO_UTILS_H

#include "misc/MacroUtils.h"

#include <spdlog/spdlog.h>

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
            MOER_LOG_CRITICAL(ss.str());   \
            assert(res == VK_SUCCESS);     \
        }                                  \
    }

#endif// VULKAN_MACRO_UTILS_H
