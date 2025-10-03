#ifndef API_MACRO_H
#define API_MACRO_H
#pragma region MODULE_API

#if defined(_WIN32) || defined(_WIN64)
#define DLLEXPORT __declspec(dllexport)
#define DLLIMPORT __declspec(dllimport)
#elif defined(linux) || defined(__linux) || defined(__linux__) || defined(__GNUC__)
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLIMPORT
#else
#define DLLEXPORT
#define DLLIMPORT
#endif

#if defined(MOER_CORE_SHARED_LIB)
#if defined(_WIN32) || defined(_WIN64)
#define CORE_API DLLEXPORT
#else // !defined(_WIN32)
#define CORE_API __attribute__((visibility("default")))
#endif
#else // !defined(MOER_CORE_SHARED_LIB)

#define CORE_API DLLIMPORT

#endif

#if defined(__cplusplus) && (__cplusplus >= 201703)
#define MOER_NODISCARD [[nodiscard]]
#elif (defined(__GNUC__) && (__GNUC__ >= 4)) || defined(__clang__) // includes clang, icc, and clang-cl
#define MOER_NODISCARD __attribute__((warn_unused_result))
#elif defined(_HAS_NODISCARD)
#define MOER_NODISCARD _NODISCARD
#elif (_MSC_VER >= 1700)
#define MOER_NODISCARD _Check_return_
#else
#define MOER_NODISCARD
#endif

#pragma endregion
#endif // !API_MACRO_H
