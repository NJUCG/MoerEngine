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
};// namespace Moer
#endif