#ifndef MACRO_H
#define MACRO_H
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif// !FORCE_INLINE
#define MACRO_STR_T(X) #X
#define MACRO_STR(X)   MACRO_STR_T(X)

#if defined(_MSC_VER)
#define ALIGNED_TYPE_DEF(Origin, Type, x) typedef __declspec(align(x)) Origin Type
#else
#if defined(__GNUC__)
#define ALIGNED_TYPE_DEF(Origin, Type, x) typedef Origin Type __attribute__((aligned(x)))
#endif
#endif

#endif// !MACRO_H
