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


#define MOER_LOG_ERROR(...)    spdlog::error(__VA_ARGS__)
#define MOER_LOG_INFO(...)     spdlog::info(__VA_ARGS__)
#define MOER_LOG_WARN(...)     spdlog::warn(__VA_ARGS__)
#define MOER_LOG_TRACE(...)    spdlog::trace(__VA_ARGS__)
#define MOER_LOG_DEBUG(...)    spdlog::debug(__VA_ARGS__)
#define MOER_LOG_CRITICAL(...) spdlog::critical(__VA_ARGS__)

#endif// !MACRO_H
