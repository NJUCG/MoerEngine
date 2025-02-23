#include "ImageIO.h"
#include "KtxImageHelper.h"
#define STB_IMAGE_IMPLEMENTATION
#include "contrib/stb/stb_image.h"
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"

#include <cassert>
#include <numeric>
#include <stb/stb_image_resize.h>
#include <dds.hpp>
#include <gl_format.h>

namespace Moer {

    struct MipmapDesc {
        uint32_t level = 0;

        /// Byte offset used for uploading
        // uint32_t offset = 0;

        /// Width depth and height of the mipmap
        Extent3D extent;
    };

    EPixelFormat ConvertFormatFromDxgiFormat(DXGI_FORMAT format, bool alpha_flag) {
        switch (format) {
            case DXGI_FORMAT_BC1_UNORM: {
                if (alpha_flag)
                    return PF_BC1_RGBA_UNORM_BLOCK;
                else
                    return PF_BC1_RGB_UNORM_BLOCK;
            }
            case DXGI_FORMAT_BC1_UNORM_SRGB: {
                if (alpha_flag)
                    return PF_BC1_RGBA_SRGB_BLOCK;
                else
                    return PF_BC1_RGB_SRGB_BLOCK;
            }

            case DXGI_FORMAT_BC2_UNORM:
                return PF_BC2_UNORM_BLOCK;
            case DXGI_FORMAT_BC2_UNORM_SRGB:
                return PF_BC2_SRGB_BLOCK;
            case DXGI_FORMAT_BC3_UNORM:
                return PF_BC3_UNORM_BLOCK;
            case DXGI_FORMAT_BC3_UNORM_SRGB:
                return PF_BC3_SRGB_BLOCK;
            case DXGI_FORMAT_BC4_UNORM:
                return PF_BC4_UNORM_BLOCK;
            case DXGI_FORMAT_BC4_SNORM:
                return PF_BC4_SNORM_BLOCK;
            case DXGI_FORMAT_BC5_UNORM:
                return PF_BC5_UNORM_BLOCK;
            case DXGI_FORMAT_BC5_SNORM:
                return PF_BC5_SNORM_BLOCK;
            case DXGI_FORMAT_BC7_UNORM:
                return PF_BC7_UNORM_BLOCK;
            case DXGI_FORMAT_BC7_UNORM_SRGB:
                return PF_BC7_SRGB_BLOCK;

            // 8-bit wide formats
            case DXGI_FORMAT_R8_UNORM:
                return PF_R8_UNORM;
            case DXGI_FORMAT_R8_UINT:
                return PF_R8_UINT;
            case DXGI_FORMAT_R8_SNORM:
                return PF_R8_SNORM;
            case DXGI_FORMAT_R8_SINT:
                return PF_R8_SINT;

            // 16-bit wide formats
            case DXGI_FORMAT_R8G8_UNORM:
                return PF_R8G8_UNORM;
            case DXGI_FORMAT_R8G8_UINT:
                return PF_R8G8_SINT;
            case DXGI_FORMAT_R8G8_SNORM:
                return PF_R8G8_SNORM;
            case DXGI_FORMAT_R8G8_SINT:
                return PF_R8G8_SINT;

            case DXGI_FORMAT_R16_FLOAT:
                return PF_R16_SFLOAT;
            case DXGI_FORMAT_R16_UNORM:
                return PF_R16_UNORM;
            case DXGI_FORMAT_R16_UINT:
                return PF_R16_UINT;
            case DXGI_FORMAT_R16_SNORM:
                return PF_R16_SNORM;
            case DXGI_FORMAT_R16_SINT:
                return PF_R16_SINT;

            case DXGI_FORMAT_B5G5R5A1_UNORM:
                return PF_B5G5R5A1_UNORM_PACK16;
            case DXGI_FORMAT_B5G6R5_UNORM:
                return PF_B5G6R5_UNORM_PACK16;
            case DXGI_FORMAT_B4G4R4A4_UNORM:
                return PF_B4G4R4A4_UNORM_PACK16;

            // 32-bit wide formats
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                return PF_R8G8B8A8_UNORM;
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                return PF_R8G8B8A8_SRGB;
            case DXGI_FORMAT_R8G8B8A8_UINT:
                return PF_R8G8B8A8_UINT;
            case DXGI_FORMAT_R8G8B8A8_SNORM:
                return PF_R8G8B8A8_SNORM;
            case DXGI_FORMAT_R8G8B8A8_SINT:
                return PF_R8G8B8A8_SINT;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return PF_B8G8R8A8_UNORM;
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                return PF_B8G8R8A8_SRGB;

            case DXGI_FORMAT_R16G16_FLOAT:
                return PF_R16G16_SFLOAT;
            case DXGI_FORMAT_R16G16_UNORM:
                return PF_R16G16_UNORM;
            case DXGI_FORMAT_R16G16_UINT:
                return PF_R16G16_UINT;
            case DXGI_FORMAT_R16G16_SNORM:
                return PF_R16G16_SNORM;
            case DXGI_FORMAT_R16G16_SINT:
                return PF_R16G16_SINT;

            case DXGI_FORMAT_R32_FLOAT:
                return PF_R32_SFLOAT;
            case DXGI_FORMAT_R32_UINT:
                return PF_R32_UINT;
            case DXGI_FORMAT_R32_SINT:
                return PF_R32_SINT;

            case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
                return PF_E5B9G9R9_UFLOAT_PACK32;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return PF_A2B10G10R10_UNORM_PACK32;
            case DXGI_FORMAT_R10G10B10A2_UINT:
                return PF_A2B10G10R10_UINT_PACK32;
            case DXGI_FORMAT_R11G11B10_FLOAT:
                return PF_B10G11R11_UFLOAT_PACK32;

            // 64-bit wide formats
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return PF_R16G16B16A16_SFLOAT;
            case DXGI_FORMAT_R16G16B16A16_SINT:
                return PF_R16G16B16A16_SINT;
            case DXGI_FORMAT_R16G16B16A16_UINT:
                return PF_R16G16B16A16_UINT;
            case DXGI_FORMAT_R16G16B16A16_UNORM:
                return PF_R16G16B16A16_UNORM;
            case DXGI_FORMAT_R16G16B16A16_SNORM:
                return PF_R16G16B16A16_SNORM;

            case DXGI_FORMAT_R32G32_FLOAT:
                return PF_R32G32_SFLOAT;
            case DXGI_FORMAT_R32G32_UINT:
                return PF_R32G32_UINT;
            case DXGI_FORMAT_R32G32_SINT:
                return PF_R32G32_SINT;

            // 96-bit wide formats
            case DXGI_FORMAT_R32G32B32_FLOAT:
                return PF_R32G32B32_SFLOAT;
            case DXGI_FORMAT_R32G32B32_UINT:
                return PF_R32G32B32_UINT;
            case DXGI_FORMAT_R32G32B32_SINT:
                return PF_R32G32B32_SINT;

            // 128-bit wide formats
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                return PF_R32G32B32A32_SFLOAT;
            case DXGI_FORMAT_R32G32B32A32_UINT:
                return PF_R32G32B32A32_UINT;
            case DXGI_FORMAT_R32G32B32A32_SINT:
                return PF_R32G32B32A32_SINT;

            case DXGI_FORMAT_R8G8_B8G8_UNORM:
            case DXGI_FORMAT_G8R8_G8B8_UNORM:
            case DXGI_FORMAT_YUY2:
            default:
                return PF_UNDEFINED;
        }
    }

    EPixelFormat ConvertFormatFromDDSFormat(uint32_t format) {
        switch (format) {
            case GL_RGBA8:
                return EPixelFormat::PF_R8G8B8A8_UNORM;
            case GL_RGBA:
                return EPixelFormat::PF_R8G8B8_UNORM;
            case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT:
                return EPixelFormat::PF_BC1_RGB_UNORM_BLOCK;
            case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT:
                return EPixelFormat::PF_BC2_UNORM_BLOCK;
            case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT:
                return EPixelFormat::PF_BC3_UNORM_BLOCK;
        }
        assert(false && "Unsupported format");
        return EPixelFormat::PF_UNDEFINED;
    }

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

    bool ImageReadDesc::IsValid() {
        return (data != nullptr && width != 0 && height != 0 && layers != 0 && mips != 0 && channal != 0 && data_size != 0);
    }

    ImageReadDesc ImageIO::ReadFromFile(const std::filesystem::path& path, uint32_t desired_channal) {
        ImageReadDesc desc;
        const auto&   path_str = path.string();
        if (path_str.ends_with(".png") || path_str.ends_with("jpg") || path_str.ends_with("jpeg")) {
            desc.data = stbi_load(path_str.c_str(), reinterpret_cast<int*>(&desc.width), reinterpret_cast<int*>(&desc.height), reinterpret_cast<int*>(&desc.channal), desired_channal);
            if (!desc.data) {
                return desc;
            }
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
        } else if (path_str.ends_with("dds")) {
            dds::Image image;
            auto       result = dds::readFile(path_str, &image);
            assert(result == dds::Success);
            desc.width     = image.width;
            desc.height    = image.height;
            desc.layers    = image.depth;
            desc.data_size = image.data.size();
            desc.data      = new uint8_t[desc.data_size];
            desc.format    = ConvertFormatFromDxgiFormat(image.format, image.supportsAlpha);
            memcpy(desc.data, image.data.data(), desc.data_size);
            desc.data_callback = free;
            desc.mips          = std::max(static_cast<uint32_t>(image.mipmaps.size()), 1u);
            if (desc.mips > 1 && false) {
                uint32_t offset = 0;
                uint32_t width  = image.width;
                uint32_t height = image.height;
                for (uint32_t miplevel = 0; miplevel < desc.mips; ++miplevel) {
                    desc.mip_offsets.push_back(offset);
                    desc.mip_extents.push_back({width, height, 1});
                    offset += image.mipmaps[miplevel].size();
                    width  = std::max(1u, width / 2);
                    height = std::max(1u, height / 2);
                }
            }

            else {
                desc.mip_offsets = {0};
                desc.mip_extents = {{image.width, image.height, 1}};
                desc.mips        = 1;
            }
        } else {
            LOG_ERROR("Unsupported image format");
            throw std::runtime_error("Unsupported image format");
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