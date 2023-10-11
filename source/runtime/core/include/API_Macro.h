#ifndef API_MACRO_H
#define API_MACRO_H
#pragma region MODULE_API
#if defined(_WIN32) || defined(_WIN64)
#define DLLEXPORT __declspec(dllexport)
#elif defined(linux) || defined(__linux) || defined(__linux__)
#define DLLEXPORT
#endif
#define RHI_API         DLLEXPORT
#define RENDER_CORE_API DLLEXPORT
#define CORE_API        DLLEXPORT

#pragma endregion
#endif// !API_MACRO_H
