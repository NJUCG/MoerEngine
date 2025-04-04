#ifndef MOER_TYPE_TRAITS_H
#define MOER_TYPE_TRAITS_H

#include "math/Base.h"
#include "math/Matrix.h"
#include <type_traits>
#include "STL.h"
#include "misc/CountableRef.h"
#include <vector>
namespace Moer {
    using uint     = uint32_t;
    using int32    = int32_t;
    using uint32   = uint32_t;
    using uint64   = uint64_t;
    using int64    = int64_t;
    using uint8    = uint8_t;
    using int8     = int8_t;
    using uint16   = uint16_t;
    using int16    = int16_t;
    using float4   = Vector4f;
    using float3   = Vector3f;
    using float2   = Vector2f;
    using uint2    = Vector2ui;
    using uint3    = Vector3ui;
    using uint4    = Vector4ui;
    using int2     = Vector2i;
    using int3     = Vector3i;
    using int4     = Vector4i;
    using byte     = std::byte;
    using ubyte    = uint8;
    using float3x4 = Matrix3x4f;
    using float4x4 = Matrix4x4f;
    using float3x3 = Matrix3x3f;
    using float2x2 = Matrix2x2f;

    // Primary template for is_template_of
    template<template<auto...> class Template, typename T>
    struct IsTemplateOf : std::false_type {};

    // Specialization for when T is an instantiation of Template
    template<template<auto...> class Template, auto... Args>
    struct IsTemplateOf<Template, Template<Args...>> : std::true_type {};

    // Helper variable template
    template<typename T, template<auto...> class TargetTemp>
    inline static constexpr bool is_template_of_v = IsTemplateOf<TargetTemp, T>::value;

    template<typename T>
    struct IsSharedPtr : std::false_type {};

    template<typename T>
    struct IsSharedPtr<SharedPtr<T>> : std::true_type {};

    template<typename T>
    static constexpr bool is_shared_ptr_v = IsSharedPtr<T>::value;

    template<typename T>
    struct IsUniquePtr : std::false_type {};

    template<typename T>
    struct IsUniquePtr<UniquePtr<T>> : std::true_type {};

    template<typename T>
    static constexpr bool is_unique_ptr_v = IsUniquePtr<T>::value;

    template<typename T>
    struct IsCountable : std::false_type {};

    template<typename T>
    struct IsCountable<CountableRef<T>> : std::true_type {};

    template<typename T>
    static constexpr bool is_countable_v = IsCountable<T>::value;

#define MARK_USER_TRIVIAL_TYPE() \
    static constexpr bool user_trival_type = true

};// namespace Moer
#endif