#ifndef MACRO_H
#define MACRO_H
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif// !FORCE_INLINE

// clang-format off
#if defined(_WIN32)
#define PLATFORM_NAME   Windows
#define PLATFORM_FOLDER platform/windows
#elif defined(__linux__)
#define PLATFORM_NAME   Linux
#define PLATFORM_FOLDER platform/linux
#endif
// clang-format on

#define MACRO_STR_T(X)         #X
#define MACRO_STR(X)           MACRO_STR_T(X)
#define MACRO_STR_JOIN_T(X, Y) X##Y
#define MACRO_STR_JOIN(X, Y)   MACRO_STR_JOIN_T(X, Y)

// clang-format off
#define COMPILED_PLATFORM_HEADER(Suffix) MACRO_STR(MACRO_STR_JOIN(PLATFORM_FOLDER/PLATFORM_NAME, Suffix))
// clang-format on

#if defined(_MSC_VER)
#define ALIGNED_TYPE_DEF(Origin, Type, x) typedef __declspec(align(x)) Origin Type
#else
#if defined(__GNUC__)
#define ALIGNED_TYPE_DEF(Origin, Type, x) typedef Origin Type __attribute__((aligned(x)))
#endif
#endif

#define GET_HIGH_BIT_UINT32(x) \
    {                          \
        x = x | (x >> 1);      \
        x = x | (x >> 2);      \
        x = x | (x >> 4);      \
        x = x | (x >> 8);      \
        x = x | (x >> 16);     \
        return (x >> 1) + 1;   \
    }

#define CHECK_AND_DELETE(ptr) \
    if (ptr != nullptr) {     \
        MoerDelete(ptr);      \
        ptr = nullptr;        \
    }

#endif// !MACRO_H
