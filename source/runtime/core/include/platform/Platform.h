#ifndef PLATFORM_H
#define PLATFORM_H
#if defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
#include "PlatformWindows.h"
#define PLATFORM_WINDOWS 1 
typedef WindowsPlatform Platform;
#elif defined(ANDROID) || defined(_ANDROID_)
#define PLATFORM_ANDROID 1 
#elif defined(__linux__)
#define PLATFORM_LINUX	 1 
#elif defined(__APPLE__) || defined(TARGET_OS_IPHONE) || defined(TARGET_IPHONE_SIMULATOR) || defined(TARGET_OS_MAC)
#define PLATFORM_IOS	 1 
#else
#define PLATFORM_UNKNOWN 1
#endif

#endif // !PLATFORM_H



