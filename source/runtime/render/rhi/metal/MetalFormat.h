#pragma once

#include "rhi/metal/MetalDevice.h"
#import <Metal/Metal.h>

namespace Moer::Render {
[[noreturn]] void Unsupported(const char* operation);
MTLPixelFormat ToMetalFormat(EPixelFormat format);
NSUInteger PixelStride(EPixelFormat format);
MTLVertexFormat ToMetalVertexFormat(EVertexFormat format);
MTLBlendOperation ToMetalBlendOperation(EBlendOperation operation);
MTLBlendFactor ToMetalBlendFactor(EBlendFactor factor);
MTLCompareFunction ToMetalCompare(ECompareOption compare);
} // namespace Moer::Render
