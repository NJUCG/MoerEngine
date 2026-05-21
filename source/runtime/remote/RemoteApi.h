#ifndef MOER_ENGINE_REMOTE_API_H
#define MOER_ENGINE_REMOTE_API_H

#include "API_Macro.h"

#if defined(REMOTE_MODULE_SHARED_LIB)

#if defined(_WIN32)
#define REMOTE_API DLLEXPORT
#else
#define REMOTE_API __attribute__((visibility("default")))
#endif

#else
#define REMOTE_API DLLIMPORT
#endif

#endif