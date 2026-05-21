#ifndef MOER_ENGINE_SCRIPTING_API_H
#define MOER_ENGINE_SCRIPTING_API_H

#include "API_Macro.h"

#if defined(SCRIPTING_MODULE_SHARED_LIB)

#if defined(_WIN32)
#define SCRIPTING_API DLLEXPORT
#else
#define SCRIPTING_API __attribute__((visibility("default")))
#endif

#else
#define SCRIPTING_API DLLIMPORT
#endif

#endif