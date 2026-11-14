#pragma once

#include "misc/Types.h"

// Vertex storage and fetch interpretation, independent of texture pixel formats.
enum class EVertexFormat : Moer::uint8 {
    Undefined,
    Float1,
    Float2,
    Float3,
    Float4,
    UInt,
    UByte4Normalized
};

[[nodiscard]] constexpr Moer::uint32 GetVertexFormatByteSize(EVertexFormat format) noexcept {
    switch (format) {
        case EVertexFormat::Float1:
        case EVertexFormat::UInt:
        case EVertexFormat::UByte4Normalized:
            return 4;
        case EVertexFormat::Float2:
            return 8;
        case EVertexFormat::Float3:
            return 12;
        case EVertexFormat::Float4:
            return 16;
        default:
            return 0;
    }
}
