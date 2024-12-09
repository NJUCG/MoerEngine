#ifndef MOER_TYPE_TRAITS_H
#define MOER_TYPE_TRAITS_H

#include "math/Base.h"
#include <type_traits>
namespace Moer {
    using uint   = uint32_t;
    using int32  = int32_t;
    using uint32 = uint32_t;
    using uint64 = uint64_t;
    using int64  = int64_t;
    using uint8  = uint8_t;
    using int8   = int8_t;
    using uint16 = uint16_t;
    using int16  = int16_t;
    using float4 = Vector4f;
    using float3 = Vector3f;
    using float2 = Vector2f;
    using uint2  = Vector2ui;
    using uint3  = Vector3ui;
    using uint4  = Vector4ui;
    using int2   = Vector2i;
    using int3   = Vector3i;
    using int4   = Vector4i;
    using byte   = std::byte;
    using ubyte  = uint8;

    // Primary template for is_template_of
    template<template<auto...> class Template, typename T>
    struct IsTemplateOf : std::false_type {};

    // Specialization for when T is an instantiation of Template
    template<template<auto...> class Template, auto... Args>
    struct IsTemplateOf<Template, Template<Args...>> : std::true_type {};

    // Helper variable template
    template<typename T, template<auto...> class TargetTemp>
    inline constexpr bool is_template_of_v = IsTemplateOf<TargetTemp, T>::value;

};// namespace Moer
#endif