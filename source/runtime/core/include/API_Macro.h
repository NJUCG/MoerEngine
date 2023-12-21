#ifndef API_MACRO_H
#define API_MACRO_H
#pragma region MODULE_API

#if defined(_WIN32) || defined(_WIN64)
#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)
#elif defined(linux) || defined(__linux) || defined(__linux__)
#define DLLEXPORT
#define DLLIMPORT
#endif

#if defined(MOER_CORE_SHARED_LIB)
#if defined(_WIN32) || defined(_WIN64)
#define CORE_API DLLEXPORT
#else// !defined(_WIN32)
#define CORE_API __attribute__((visibility("default")))
#endif
#else// !defined(MOER_CORE_SHARED_LIB)

#define CORE_API DLLIMPORT

#endif

#define RHI_API         DLLEXPORT
#define RENDER_CORE_API DLLEXPORT

#pragma endregion
#endif// !API_MACRO_H
