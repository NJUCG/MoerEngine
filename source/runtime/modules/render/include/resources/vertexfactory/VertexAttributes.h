#pragma once

#include "misc/Traits.h"
#include "PixelFormat.h"

namespace Moer {

    // TODO: Array<Array<uint16>> joint_data;
    //       Array<float4>        joint_weights;

    // *************************************************
    // * Vertex Attributes Table
    // *   Name, Type, PixelFormat
#define VERTEX_ATTRIBUTES_TABLE                 \
    X(VA_POSITION, float3, PF_R32G32B32_SFLOAT) \
    X(VA_NORMAL, uint, PF_R32_UINT)             \
    X(VA_TANGENT, uint, PF_R32_UINT)            \
    X(VA_TEXCOORD0, float2, PF_R32G32_SFLOAT)   \
    X(VA_TEXCOORD1, float2, PF_R32G32_SFLOAT)
    // *
    // *************************************************

    // EVertexAttributes
    enum class EVertexAttributes : size_t {
#define X(E, T, PF) E,
        VERTEX_ATTRIBUTES_TABLE
#undef X
            VA_NUM
    };

    static constexpr size_t VA_NUM = static_cast<size_t>(EVertexAttributes::VA_NUM);

    // VertexAttributes Tool of Run Time

    class VertexAttributesTool {
    public:
        static size_t GetSize(EVertexAttributes attr) {
            switch (attr) {
#define X(E, T, PF) \
    case EVertexAttributes::E: return sizeof(T);
                VERTEX_ATTRIBUTES_TABLE
#undef X
            }
            assert(false && "Invalid EVertexAttributes");
            return 0;
        }

        static EPixelFormat GetPixelFormat(EVertexAttributes attr) {
            switch (attr) {
#define X(E, T, PF) \
    case EVertexAttributes::E: return PF;
                VERTEX_ATTRIBUTES_TABLE
#undef X
            }
            assert(false && "Invalid EVertexAttributes");
            return EPixelFormat::PF_UNDEFINED;
        }
    };

    // VertexAttributes Tool of Compile Time

    template<EVertexAttributes E>
    struct VertexAttributesType;

    template<EVertexAttributes E>
    struct VertexAttributesPixelFormat;

#define X(E, T, PF)                                     \
    template<>                                          \
    struct VertexAttributesType<EVertexAttributes::E> { \
        using type = T;                                 \
    };
    VERTEX_ATTRIBUTES_TABLE
#undef X

// PF is a PixelFormat enum
#define X(E, T, PF_)                                           \
    template<>                                                 \
    struct VertexAttributesPixelFormat<EVertexAttributes::E> { \
        constexpr static EPixelFormat PF = PF_;                \
    };
    VERTEX_ATTRIBUTES_TABLE
#undef X

#undef VERTEX_ATTRIBUTES_TABLE

}// namespace Moer