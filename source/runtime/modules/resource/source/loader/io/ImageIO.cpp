#include "ImageIO.h"
#include "KtxImageHelper.h"

#include "contrib/stb/stb_image.h"
#include "rhi/RHICommon.h"

#include <cassert>
#include <numeric>
#include <stb/stb_image_resize.h>
namespace Moer {

    struct MipmapDesc {
        uint32_t level = 0;

        /// Byte offset used for uploading
        // uint32_t offset = 0;

        /// Width depth and height of the mipmap
        Extent3D extent;
    };

    //
    static void Generatemipmaps(ImageReadDesc& desc) {
        if (desc.mip_offsets.size() > 1)
            return;

        Extent3D                extent         = {desc.width, desc.height, desc.layers};
        auto                    next_width     = desc.width;
        auto                    next_height    = desc.height;
        auto                    channels       = 4;
        auto                    next_size      = 0;
        uint32_t                one_image_size = desc.width * desc.height * channels;
        std::vector<MipmapDesc> mipmaps;

        //  auto& mipmaps = desc.mip_map_descs;
        uint32_t old_size = one_image_size;
        desc.mip_offsets  = {0};
        desc.mip_extents  = {extent};
        mipmaps           = {{0, extent}};
        while (true) {
            auto& prev_mipmap = mipmaps.back();

            next_width  = std::max<uint32_t>(1u, next_width / 2);
            next_height = std::max<uint32_t>(1u, next_height / 2);
            next_size   = next_width * next_height * channels;

            MipmapDesc next_mipmap{};
            next_mipmap.level  = prev_mipmap.level + 1;
            next_mipmap.extent = {next_width, next_height, 1u};
            mipmaps.emplace_back(next_mipmap);

            desc.mip_offsets.emplace_back(old_size);
            desc.mip_extents.push_back(next_mipmap.extent);

            old_size += next_size;
            if (next_width == 1 && next_height == 1) {
                break;
            }
        }
        uint32_t layer_offset = old_size;
        for (uint32_t layer = 1; layer < desc.layers; layer++) {
            for (int i = 0; i < mipmaps.size(); i++) {
                desc.mip_offsets.push_back(desc.mip_offsets[desc.mip_offsets.size() - desc.mips] + layer_offset);
            }
        }
        old_size             = desc.layers * old_size;
        desc.mips            = mipmaps.size();
        auto mip_mapped_data = new uint8_t[old_size];

        for (uint32_t layer = 0; layer < desc.layers; layer++) {
            memcpy(mip_mapped_data + desc.mip_offsets[layer * desc.mips], static_cast<uint8_t const*>(desc.data) + layer * one_image_size, one_image_size);
            // auto & cur_layer_mipmaps = mipmaps[layer];
            for (int mip = 1; mip < desc.mips; mip++) {
                auto&    prev_mipmap         = mipmaps[mip - 1];
                auto&    next_mipmap         = mipmaps[mip];
                uint32_t prev_mip_map_offset = desc.mip_offsets[layer * desc.mips + mip - 1];
                uint32_t next_mip_map_offset = desc.mip_offsets[layer * desc.mips + mip];
                stbir_resize_uint8(mip_mapped_data + prev_mip_map_offset, prev_mipmap.extent.width, prev_mipmap.extent.height, 0, mip_mapped_data + next_mip_map_offset, next_mipmap.extent.width, next_mipmap.extent.height, 0, channels);
            }
        }
        desc.data_size = old_size;
        desc.data_callback(desc.data);
        desc.data          = mip_mapped_data;
        desc.data_callback = free;
    }

    struct CallbackData final {
        ktxTexture*              texture;
        std::vector<MipmapDesc>* mipmaps;
        std::vector<uint32_t>*   offsets;
    };

    static KTX_error_code KTXAPIENTRY CallBack(int mipLevel, int face, int width, int height, int depth, ktx_uint32_t faceLodSize, void* pixels, void* userdata) {
        auto* callback_data = reinterpret_cast<CallbackData*>(userdata);
        assert(static_cast<size_t>(mipLevel) < callback_data->mipmaps->size() && "Not enough space in the mipmap vector");

        ktx_size_t mipmap_offset = 0;
        auto       result        = ktxTexture_GetImageOffset(callback_data->texture, mipLevel, 0, face, &mipmap_offset);
        if (result != KTX_SUCCESS) {
            return result;
        }

        auto& mipmap                         = callback_data->mipmaps->at(mipLevel);
        callback_data->offsets->at(mipLevel) = mipmap_offset;
        mipmap.level                         = mipLevel;
        mipmap.extent.width                  = width;
        mipmap.extent.height                 = height;
        mipmap.extent.depth                  = depth;

        return KTX_SUCCESS;
    }

    void ImageReadDesc::CheckValid() {
        assert(data != nullptr && width != 0 && height != 0 && layers != 0 && mips != 0 && channal != 0 && data_size != 0);
    }
    ImageReadDesc ImageIO::ReadFromFile(const std::filesystem::path& path, uint32_t desired_channal) {
        ImageReadDesc desc;
        const auto&   path_str = path.string();
        if (path_str.ends_with(".png") || path_str.ends_with("jpg")) {
            desc.data          = stbi_load(path_str.c_str(), reinterpret_cast<int*>(&desc.width), reinterpret_cast<int*>(&desc.height), reinterpret_cast<int*>(&desc.channal), desired_channal);
            desc.data_callback = stbi_image_free;
            desc.format        = EPixelFormat::PF_R8G8B8A8_UNORM;
            desc.data_size     = desc.width * desc.height * desc.channal;
            Generatemipmaps(desc);
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
            desc.layers = ktx_texture->numLayers;
            if (desc.layers > 1) {
                for (int i = 0; i < desc.layers; i++) {
                    ktx_size_t layer_offset = 0;
                    result                  = ktxTexture_GetImageOffset(ktx_texture, 0, i, 0, &layer_offset);
                    assert(result == KTX_SUCCESS);
                    desc.mip_offsets.push_back(layer_offset);
                }
            }

            if (KtxImageHelper::IsAstc(desc.format)) {
                //todo Get this from rhi
                bool astc_supported = false;
                if (!astc_supported) {
                    KtxImageHelper::DecodeAstcImage(desc);
                    Generatemipmaps(desc);
                }
            } else {
                desc.mips        = ktx_texture->numLevels;
                desc.mip_offsets = std::vector<uint32_t>(ktx_texture->numLevels);
                for (uint32_t layer = 0; layer < desc.layers; layer++) {
                    for (uint32_t miplevel = 0; miplevel < desc.mips; ++miplevel) {
                        ktx_size_t offset;
                        ktxTexture_GetImageOffset(ktx_texture, miplevel, layer, 0, &offset);
                        desc.mip_offsets.push_back(offset);
                    }
                }
            }
        }
        return desc;
    }

    ImageReadDesc ImageIO::ReadFromMemory(const unsigned char* memory_data, size_t len, uint32_t desired_channal) {
        ImageReadDesc desc;
        desc.data          = stbi_load_from_memory(memory_data, len, reinterpret_cast<int*>(&desc.width), reinterpret_cast<int*>(&desc.height), reinterpret_cast<int*>(&desc.channal), desired_channal);
        desc.data_callback = free;
        desc.data_size     = desc.width * desc.height * desc.channal;
        desc.format        = EPixelFormat::PF_R8G8B8A8_UNORM;
        Generatemipmaps(desc);
        return desc;
    }

}