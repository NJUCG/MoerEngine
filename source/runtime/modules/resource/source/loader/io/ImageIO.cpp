#include "ImageIO.h"
#include "KtxImageHelper.h"

#include "contrib/stb/stb_image.h"

#include <cassert>
namespace Moer {

    ImageReadDesc ImageIO::ReadFromFile(const std::filesystem::path& path, uint32_t desired_channal) {
        ImageReadDesc desc;
        const auto&   path_str = path.string();
        if (path_str.ends_with(".png") || path_str.ends_with("jpg")) {
            desc.data          = stbi_load(path_str.c_str(), &desc.width, &desc.height, &desc.channal, desired_channal);
            desc.data_callback = stbi_image_free;
        } else if (path_str.ends_with("ktx")) {
            ktxTexture* ktx_texture;
            ktxResult   result = ktxTexture_CreateFromNamedFile(path_str.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx_texture);
            assert(result == KTX_SUCCESS);
            //   this->extent3D = VkExtent3D{ktxTexture->baseWidth, ktxTexture->baseHeight, 1};
            desc.width  = ktx_texture->baseWidth;
            desc.height = ktx_texture->baseHeight;

            ktx_uint8_t* ktxTextureData = ktxTexture_GetData(ktx_texture);
            ktx_size_t   ktxTextureSize = ktxTexture_GetSize(ktx_texture);

            uint8_t* data = new uint8_t[ktxTextureSize];
            memcpy(data, ktxTextureData, ktxTextureSize);

            desc.data_callback = free;
            desc.data          = data;
            assert(result == KTX_SUCCESS);

            desc.format = KtxImageHelper::GetFormatFromOpenGLInternalFormat(ktx_texture->glInternalformat);

            if (KtxImageHelper::IsAstc(desc.format)) {
                //todo Get this from rhi
                bool astc_supported = false;
                if (!astc_supported) {
                    KtxImageHelper::DecodeAstcImage(desc);
                }
            }
        }
        return desc;
    }

}