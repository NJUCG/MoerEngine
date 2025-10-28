#pragma once

#ifdef __cplusplus
#define EnumParam(Name, first, ...)          \
    enum class Name {                        \
        first = 0 __VA_OPT__(, __VA_ARGS__), \
        NUM                                  \
    };
#else

// HLSL side (DXC): some versions may not support __VA_OPT__. Use a dispatcher to
// differentiate between 2-arg (Name, first) and 3+ args (Name, first, ...).

// // Select helper (support up to 10 args; extend if needed)
// #define MOER_ENUMPARAM_SELECT(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, NAME, ...) NAME

// // Exact 2 args: Name, first
// #define EnumParam_2(Name, first) \
//     enum Name {                  \
//         first = 0,               \
//         NUM                      \
//     };

// // 3 or more args: Name, first, ...
// #define EnumParam_3PLUS(Name, first, ...) \
//     enum Name {                           \
//         first = 0,                        \
//         __VA_ARGS__,                      \
//         NUM                               \
//     };

// // Public macro
// #define EnumParam(...)                                                                                                                                                                                        \
//     MOER_ENUMPARAM_SELECT(__VA_ARGS__, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_3PLUS, EnumParam_2)( \
//         __VA_ARGS__
// Public macro
#define EnumParam(Name, ...) \
    namespace Name {         \
    enum {                   \
        __VA_ARGS__,         \
        NUM                  \
    };                       \
    }

#endif
