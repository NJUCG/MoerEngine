#ifndef MACRO_H
#define MACRO_H
#ifndef FORCEINLINE
#define FORCEINLINE __forceinline
#endif// !FORCE_INLINE
#define MACRO_STR_T(X) #X
#define MACRO_STR(X)   MACRO_STR_T(X)
#endif// !MACRO_H
