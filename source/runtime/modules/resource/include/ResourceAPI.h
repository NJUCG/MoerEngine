#ifndef MOER_ENGINE_RESOURCE_API_H
#define MOER_ENGINE_RESOURCE_API_H
#include "API_Macro.h"
#if defined(RESOURCE_MODULE_SHARED_LIB)

#if defined(_WIN32)
#define RESOURCE_API DLLEXPORT
#else// !defined(_WIN32)
#define RESOURCE_API __attribute__((visibility("default")))
#endif

#else// !defined(RESOURCE_MODULE_SHARED_LIB)
#define RESOURCE_API DLLIMPORT
#endif
#endif