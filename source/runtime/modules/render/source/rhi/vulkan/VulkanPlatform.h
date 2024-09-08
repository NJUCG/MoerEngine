//
// Created by 74535 on 2023/10/11.
//

#ifndef VULKAN_PLATFORM_H
#define VULKAN_PLATFORM_H

#include "misc/MacroUtils.h"
#include "platform/Platform.h"

#if PLATFORM_WINDOWS
#include "platform/windows/WindowsVulkanPlatformDefines.h"
#else if PLATFORM_LINUX

#endif
#endif//VULKAN_PLATFORM_H
