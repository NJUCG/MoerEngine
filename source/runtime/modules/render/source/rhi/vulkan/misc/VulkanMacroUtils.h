//
// Created by 74535 on 2023/10/2.
//

#ifndef VULKAN_MACRO_UTILS_H
#define VULKAN_MACRO_UTILS_H

#define CRITICAL_AND_THROW(MSG)        \
    {                                  \
        spdlog::critical(MSG);         \
        throw std::runtime_error(MSG); \
    }

#define CRITICAL_AND_RETURN(MSG, CODE) \
    {                                  \
        spdlog::critical(MSG);         \
        return CODE;                   \
    }

#define VK_CHECK_RESULT(f)                                        \
    {                                                             \
        VkResult res = (f);                                       \
        if (res != VK_SUCCESS) {                                  \
            std::stringstream ss;                                 \
            ss << "Fatal : VkResult is \""                        \
               << MoerEngine::RHI::Vulkan::Util::ErrorString(res) \
               << "\" in " << __FILE__                            \
               << " at line " << __LINE__                         \
               << "\n";                                           \
            spdlog::critical(ss.str());                           \
            assert(res == VK_SUCCESS);                            \
        }                                                         \
    }

#endif// VULKAN_MACRO_UTILS_H
