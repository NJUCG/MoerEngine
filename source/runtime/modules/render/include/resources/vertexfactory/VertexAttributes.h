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
    X(VA_TEXCOORD1, float2, PF_R32G32_SFLOAT)   \
    // X(VA_INSTANCEID, uint, PF_R32_UINT)         \
    // *                                        \
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

    using VertexAttributesBitmask = uint64;

    class VertexAttributesTool {
    public:
        static size_t GetSize(EVertexAttributes _attr) {
            switch (_attr) {
#define X(E, T, PF) \
    case EVertexAttributes::E: return sizeof(T);
                VERTEX_ATTRIBUTES_TABLE
#undef X
                default:
                    break;
            }
            assert(false && "Invalid EVertexAttributes");
            return 0;
        }

        static EPixelFormat GetPixelFormat(EVertexAttributes _attr) {
            switch (_attr) {
#define X(E, T, PF) \
    case EVertexAttributes::E: return PF;
                VERTEX_ATTRIBUTES_TABLE
#undef X
                default:
                    break;
            }
            assert(false && "Invalid EVertexAttributes");
            return EPixelFormat::PF_UNDEFINED;
        }

        static VertexAttributesBitmask GetBitmaskFromArray(Moer::Array<EVertexAttributes> attrs) {
            VertexAttributesBitmask mask = 0;
            for (auto attr : attrs) {
                mask |= 1 << static_cast<size_t>(attr);
            }
            return mask;
        }

        // 注意，这里返回的Array顺序不能被随意改变，顺序应该按照EVertexAttributes枚举值的顺序（从小到大）
        static Moer::Array<EVertexAttributes> GetArrayFromBitmask(VertexAttributesBitmask mask) {
            Moer::Array<EVertexAttributes> attrs;
            for (size_t i = 0; i < VA_NUM; i++) {
                if (mask & (1 << i)) {
                    attrs.push_back(static_cast<EVertexAttributes>(i));
                }
            }
            return attrs;
        }

        static bool HasAttribute(VertexAttributesBitmask mask, EVertexAttributes attr) {
            return mask & (1 << static_cast<size_t>(attr));
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