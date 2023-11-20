#ifndef MOER_ENGINE_RENDER_API_H
#define MOER_ENGINE_RENDER_API_H
#include "API_Macro.h"
#if defined(RENDER_MODULE_SHARED_LIB)

#if defined(_WIN32)
#define RENDER_API DLLEXPORT
#else// !defined(_WIN32)
#define RENDER_API __attribute__((visibility("default")))
#endif

#else// !defined(RENDER_MODULE_SHARED_LIB)
#define RENDER_API DLLIMPORT
#endif
#endif