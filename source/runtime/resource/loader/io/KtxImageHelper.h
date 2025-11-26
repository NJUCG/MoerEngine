#pragma once
#include "ImageIO.h"
#include "math/Base.h"

#include <ktx.h>

namespace Moer {
class KtxImageHelper {
public:
    static EPixelFormat GetFormatFromOpenGLInternalFormat(const GLenum intername_foramt);
    static void         DecodeAstcImage(ImageReadDesc& desc);
    static bool         IsAstc(EPixelFormat format);

protected:
    static void Decode(ImageReadDesc& desc, Vector3i block_dim);
};
} // namespace Moer
