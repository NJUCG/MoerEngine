#ifndef ENUM_BIT_OPERATION_H_
#define ENUM_BIT_OPERATION_H_
#ifndef MOER_FORCE_INLINE
#define MOER_FORCE_INLINE __forceinline
#endif// !FORCE_INLINE
#include <type_traits>

#define PARENS ()

#define EXPAND(...) EXPAND4(EXPAND4(EXPAND4(EXPAND4(__VA_ARGS__))))
#define EXPAND4(...) EXPAND3(EXPAND3(EXPAND3(EXPAND3(__VA_ARGS__))))
#define EXPAND3(...) EXPAND2(EXPAND2(EXPAND2(EXPAND2(__VA_ARGS__))))
#define EXPAND2(...) EXPAND1(EXPAND1(EXPAND1(EXPAND1(__VA_ARGS__))))
#define EXPAND1(...) __VA_ARGS__



#define ENUM_BIT_OP_IMPL_1(TEnum)                                                                                                                                      \
    MOER_FORCE_INLINE constexpr TEnum operator^(TEnum lhs, TEnum rhs) { return (TEnum)(((std::underlying_type_t<TEnum>)lhs) ^ ((std::underlying_type_t<TEnum>)rhs)); } \
    MOER_FORCE_INLINE constexpr TEnum operator|(TEnum lhs, TEnum rhs) { return (TEnum)(((std::underlying_type_t<TEnum>)lhs) | ((std::underlying_type_t<TEnum>)rhs)); } \
    MOER_FORCE_INLINE constexpr TEnum operator&(TEnum lhs, TEnum rhs) { return (TEnum)(((std::underlying_type_t<TEnum>)lhs) & ((std::underlying_type_t<TEnum>)rhs)); } \
    MOER_FORCE_INLINE constexpr TEnum operator~(TEnum e) { return (TEnum)(~(std::underlying_type_t<TEnum>)e); }                                                        \
    MOER_FORCE_INLINE constexpr bool  operator!(TEnum e) { return !((std::underlying_type_t<TEnum>)e); }                                                               \
    MOER_FORCE_INLINE TEnum&          operator&=(TEnum& lhs, TEnum rhs) {                                                                                              \
        lhs = lhs & rhs;                                                                                                                                      \
        return lhs;                                                                                                                                           \
    }                                                                                                                                                                  \
    MOER_FORCE_INLINE const TEnum operator^=(TEnum& lhs, TEnum rhs) {                                                                                                  \
        lhs = lhs ^ rhs;                                                                                                                                               \
        return lhs;                                                                                                                                                    \
    }                                                                                                                                                                  \
    MOER_FORCE_INLINE const TEnum operator|=(TEnum& lhs, TEnum rhs) {                                                                                                  \
        lhs = lhs | rhs;                                                                                                                                               \
        return lhs;                                                                                                                                                    \
    }

#define ENUM_BIT_OP_FLAG(TEnum)                                                                     \
    MOER_FORCE_INLINE bool EnumHasAllFlag(TEnum lhs, TEnum rhs) { return (lhs & rhs) == rhs; }      \
    MOER_FORCE_INLINE bool EnumHasAnyFlag(TEnum lhs, TEnum rhs) { return (lhs | rhs) != (TEnum)0; } \
    MOER_FORCE_INLINE void EnumAddFlags(TEnum& lhs, TEnum rhs) { lhs |= rhs; }                      \
    MOER_FORCE_INLINE void EnumRemoveFlags(TEnum& lhs, TEnum rhs) { lhs &= ~rhs; }

#define _EXPAND_ARGS_(...) __VA_ARGS__
#define ENUM_BIT_OP_(TEnum)

#define ENUM_BIT_OP_2(TEnum, OPERATION)    ENUM_BIT_OP_##OPERATION(TEnum)
//#define ENUM_BIT_OP(TEnum, OPERATION, ...) ENUM_BIT_OP_2(TEnum, OPERATION) _EXPAND_ARGS_(ENUM_BIT_OP_2(TEnum, ##__VA_ARGS__))

/*implement enum bit operation*/
//#define ENUM_BIT_OP_IMPL(TEnum, ...) ENUM_BIT_OP_IMPL_1(TEnum) _EXPAND_ARGS_(ENUM_BIT_OP(TEnum, ##__VA_ARGS__))

#define ENUM_BIT_OP(ENUM, ...)                                    \
  __VA_OPT__(EXPAND(FOR_EACH_HELPER(ENUM, __VA_ARGS__)))

#define FOR_EACH_HELPER(ENUM, a1, ...)                         \
  ENUM_BIT_OP_2(ENUM, a1)                                                     \
  __VA_OPT__(FOR_EACH_AGAIN PARENS (macro, __VA_ARGS__))

#define FOR_EACH_AGAIN() FOR_EACH_HELPER

#define ENUM_BIT_OP_IMPL(TEnum, ...) ENUM_BIT_OP_IMPL_1(TEnum)

#define ENUM_FLAG_OP_IMPL(TEnum) ENUM_BIT_OP_FLAG(TEnum)

#define BEGIN_ENUM_STR_DEFINITION(TEnum) \
    const char* ToString(TEnum target) { \
        using TThisEnum = TEnum;         \
        switch (target) {                \
            default: return "";

#define ENUM_STR_ELEMENT(enum) \
    case TThisEnum::enum: return #enum;

#define END_ENUM_STR_DEFINITION(TEnum) \
    }}

//#undef FORCE_INLINE
#endif// !ENUM_BIT_OPERATION_H_
