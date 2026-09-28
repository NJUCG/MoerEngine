#include "rhi/metal/MetalFormat.h"

#include <stdexcept>
#include <string>

namespace Moer::Render {
[[noreturn]] void Unsupported(const char* operation) {
    throw std::runtime_error(std::string("Metal RHI has not implemented ") + operation);
}

MTLPixelFormat ToMetalFormat(EPixelFormat format) {
    switch (format) {
        case PF_R8_UNORM:
            return MTLPixelFormatR8Unorm;
        case PF_R8G8_UNORM:
            return MTLPixelFormatRG8Unorm;
        case PF_R8G8B8A8_UNORM:
            return MTLPixelFormatRGBA8Unorm;
        case PF_R8G8B8A8_SRGB:
            return MTLPixelFormatRGBA8Unorm_sRGB;
        case PF_B8G8R8A8_UNORM:
            return MTLPixelFormatBGRA8Unorm;
        case PF_B8G8R8A8_SRGB:
            return MTLPixelFormatBGRA8Unorm_sRGB;
        case PF_A2R10G10B10_UNORM_PACK32:
            return MTLPixelFormatBGR10A2Unorm;
        case PF_B10G11R11_UFLOAT_PACK32:
            return MTLPixelFormatRG11B10Float;
        case PF_R16_SFLOAT:
            return MTLPixelFormatR16Float;
        case PF_R16G16_SFLOAT:
            return MTLPixelFormatRG16Float;
        case PF_R16G16B16A16_SFLOAT:
            return MTLPixelFormatRGBA16Float;
        case PF_R32_SFLOAT:
            return MTLPixelFormatR32Float;
        case PF_R32G32B32A32_SFLOAT:
            return MTLPixelFormatRGBA32Float;
        case PF_D16_UNORM:
            return MTLPixelFormatDepth16Unorm;
        case PF_D32_SFLOAT:
            return MTLPixelFormatDepth32Float;
        case PF_D32_SFLOAT_S8_UINT:
            return MTLPixelFormatDepth32Float_Stencil8;
        default:
            throw std::runtime_error("Metal RHI has not implemented texture format " +
                                     std::to_string(static_cast<uint>(format)));
    }
}

NSUInteger PixelStride(EPixelFormat format) {
    switch (format) {
        case PF_R8_UNORM:
            return 1;
        case PF_R8G8_UNORM:
        case PF_R16_SFLOAT:
        case PF_D16_UNORM:
            return 2;
        case PF_R16G16B16A16_SFLOAT:
        case PF_D32_SFLOAT_S8_UINT:
            return 8;
        case PF_R8G8B8A8_UNORM:
        case PF_R8G8B8A8_SRGB:
        case PF_B8G8R8A8_UNORM:
        case PF_B8G8R8A8_SRGB:
        case PF_A2R10G10B10_UNORM_PACK32:
        case PF_B10G11R11_UFLOAT_PACK32:
        case PF_R16G16_SFLOAT:
        case PF_R32_SFLOAT:
        case PF_D32_SFLOAT:
            return 4;
        case PF_R32G32B32A32_SFLOAT:
            return 16;
        default:
            Unsupported("this texture format");
    }
}

MTLVertexFormat ToMetalVertexFormat(EPixelFormat format) {
    switch (format) {
        case PF_R32_SFLOAT: return MTLVertexFormatFloat;
        case PF_R32G32_SFLOAT: return MTLVertexFormatFloat2;
        case PF_R32G32B32_SFLOAT: return MTLVertexFormatFloat3;
        case PF_R32G32B32A32_SFLOAT: return MTLVertexFormatFloat4;
        case PF_R8G8B8A8_UNORM: return MTLVertexFormatUChar4Normalized;
        default: Unsupported("this vertex attribute format");
    }
}

MTLBlendOperation ToMetalBlendOperation(EBlendOperation operation) {
    switch (operation) {
        case BO_ADD: return MTLBlendOperationAdd;
        case BO_SUBTRACT: return MTLBlendOperationSubtract;
        case BO_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
        case BO_MIN: return MTLBlendOperationMin;
        case BO_MAX: return MTLBlendOperationMax;
        default: Unsupported("this blend operation");
    }
}

MTLBlendFactor ToMetalBlendFactor(EBlendFactor factor) {
    switch (factor) {
        case BF_ZERO: return MTLBlendFactorZero;
        case BF_ONE: return MTLBlendFactorOne;
        case BF_SRC_COLOR: return MTLBlendFactorSourceColor;
        case BF_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
        case BF_DST_COLOR: return MTLBlendFactorDestinationColor;
        case BF_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
        case BF_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
        case BF_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
        case BF_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
        case BF_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
        case BF_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
        case BF_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
        case BF_SRC1_COLOR: return MTLBlendFactorSource1Color;
        case BF_ONE_MINUS_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
        case BF_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
        case BF_ONE_MINUS_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
        default: Unsupported("this blend factor");
    }
}

MTLCompareFunction ToMetalCompare(ECompareOption compare) {
    switch (compare) {
        case CO_NEVER: return MTLCompareFunctionNever;
        case CO_LESS: return MTLCompareFunctionLess;
        case CO_EQUAL: return MTLCompareFunctionEqual;
        case CO_LESS_OR_EQUAL: return MTLCompareFunctionLessEqual;
        case CO_GREATER: return MTLCompareFunctionGreater;
        case CO_NOT_EQUAL: return MTLCompareFunctionNotEqual;
        case CO_GREATER_OR_EQUAL: return MTLCompareFunctionGreaterEqual;
        case CO_ALWAYS: return MTLCompareFunctionAlways;
        default: Unsupported("this depth comparison");
    }
}

} // namespace Moer::Render
