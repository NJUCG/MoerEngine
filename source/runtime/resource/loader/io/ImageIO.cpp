#include "ImageIO.h"
#include "KtxImageHelper.h"
#define STB_IMAGE_IMPLEMENTATION
#include "contrib/stb/stb_image.h"
#include "log/LogSystem.h"
#include "rhi/RHICommon.h"

#include <cassert>
#include <dds.hpp>
#include <gl_format.h>
#include <numeric>
#include <stb/stb_image_resize2.h>

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

/// Generate mipmaps for the given image data
/// @param _desc Image description containing the base image data
static void Generatemipmaps(ImageReadDesc& _desc) {
    if (_desc.mips > 1)
        return; // Mipmaps already present

    // Setup variables
    Extent3D extent          = {_desc.width, _desc.height, _desc.layers};
    auto     next_width      = _desc.width;
    auto     next_height     = _desc.height;
    auto     channel         = _desc.channel;
    auto     base_image_size = _desc.width * _desc.height * channel;
    // Calculate mipmap chain
    auto                             mipmaps_size   = base_image_size;
    Array<uint32>                    mip_offsets    = {0};
    Array<std::pair<uint32, uint32>> mipmap_extents = {{next_width, next_height}};

    while (next_width > 1 || next_height > 1) {
        next_width     = std::max<uint32>(1u, next_width >> 1);
        next_height    = std::max<uint32>(1u, next_height >> 1);
        auto next_size = next_width * next_height * channel;

        mip_offsets.emplace_back(mipmaps_size);
        mipmap_extents.emplace_back(next_width, next_height);

        mipmaps_size += next_size;
    }

    auto mip_level = mip_offsets.size();

    // Allocate total mipmap memory
    assert(channel == 4 && "Default support 4 channels. If you need more, please modify the code");
    auto channel_in_stbir = stbir_pixel_layout::STBIR_4CHANNEL;

    auto* mipmaps_mapped_data = new uint8[_desc.layers * mipmaps_size];
    // Generate mipmaps for each layer
    // TODO: #pragma omp parallel for
    for (uint32_t layer = 0; layer < _desc.layers; ++layer) {
        auto offset = layer * mipmaps_size;
        // Copy base mipmap (level 0)
        memcpy(mipmaps_mapped_data + offset, static_cast<uint8 const*>(_desc.data) + offset, base_image_size);

        // Generate lower mipmap levels
        for (uint32_t mip = 1; mip < mip_level; ++mip) {
            auto prev_mipmap_offset = offset + mip_offsets[mip - 1];
            auto next_mipmap_offset = offset + mip_offsets[mip];

            stbir_resize_uint8_linear(
                mipmaps_mapped_data + prev_mipmap_offset,
                mipmap_extents[mip - 1].first,
                mipmap_extents[mip - 1].second,
                0,
                mipmaps_mapped_data + next_mipmap_offset,
                mipmap_extents[mip].first,
                mipmap_extents[mip].second,
                0,
                channel_in_stbir
            );
        }
    }

    // Update descriptor with generated mipmaps
    _desc.mips          = mip_level;
    _desc.data_size     = mipmaps_size * _desc.layers;
    _desc.data          = mipmaps_mapped_data;
    _desc.data_callback = [](void* _ptr) {
        delete[] static_cast<uint8*>(_ptr);
    };
}

struct CallbackData final {
    ktxTexture*              texture;
    std::vector<MipmapDesc>* mipmaps;
    std::vector<uint32_t>*   offsets;
};

static KTX_error_code KTXAPIENTRY CallBack(
    int          mipLevel,
    int          face,
    int          width,
    int          height,
    int          depth,
    ktx_uint32_t faceLodSize,
    void*        pixels,
    void*        userdata
) {
    auto* callback_data = reinterpret_cast<CallbackData*>(userdata);
    assert(
        static_cast<size_t>(mipLevel) < callback_data->mipmaps->size() &&
        "Not enough space in the mipmap vector"
    );

    ktx_size_t mipmap_offset = 0;
    auto       result = ktxTexture_GetImageOffset(callback_data->texture, mipLevel, 0, face, &mipmap_offset);
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
    return (
        data != nullptr && width != 0 && height != 0 && layers != 0 && mips != 0 && channel != 0 &&
        data_size != 0
    );
}

ImageReadDesc
ImageIO::ReadFromFile(const std::filesystem::path& _path, uint32_t _desired_channal, EPixelFormat _fmt) {
    ImageReadDesc desc;
    auto          path_str = _path.string();
    // to lowercase
    std::transform(path_str.begin(), path_str.end(), path_str.begin(), ::tolower);
    if (path_str.ends_with("png") || path_str.ends_with("jpg") || path_str.ends_with("jpeg")) {
        desc.data = stbi_load(
            path_str.c_str(),
            reinterpret_cast<int*>(&desc.width),
            reinterpret_cast<int*>(&desc.height),
            reinterpret_cast<int*>(&desc.channel),
            _desired_channal
        );
        if (!desc.data) {
            return desc;
        }

        // stbi_load 返回的 desc.channel 是原图通道数
        // _desired_channal 是期望通道数 和 当前data通道数！
        desc.channel = _desired_channal;

        desc.data_callback = stbi_image_free;
        desc.format        = _fmt;
        desc.data_size     = desc.width * desc.height * desc.channel;
        Generatemipmaps(desc);
    } else if (path_str.ends_with("ktx")) {
        ktxTexture* ktx_texture = nullptr;
        ktxResult   result      = ktxTexture_CreateFromNamedFile(
            path_str.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx_texture
        );
        if (result != KTX_SUCCESS) {
            LOG_ERROR("Failed to load KTX texture: {}", ktxErrorString(result));
            throw std::runtime_error("Failed to load KTX texture");
        }

        // Set texture properties
        desc.width  = ktx_texture->baseWidth;
        desc.height = ktx_texture->baseHeight;
        desc.layers = ktx_texture->numLayers;
        desc.mips   = ktx_texture->numLevels;
        desc.faces  = ktx_texture->numFaces;
        desc.format = KtxImageHelper::GetFormatFromOpenGLInternalFormat(ktx_texture->glInternalformat);

        desc.data_size     = ktxTexture_GetSize(ktx_texture);
        desc.data          = new uint8[desc.data_size];
        desc.data_callback = [](void* _ptr) {
            delete[] static_cast<uint8*>(_ptr);
        };

        auto* ktx_texture_data = ktxTexture_GetData(ktx_texture);
        memcpy(desc.data, ktx_texture_data, desc.data_size);

        if (KtxImageHelper::IsAstc(desc.format)) {
            //todo Get this from rhi
            bool astc_supported = false;
            if (!astc_supported) {
                KtxImageHelper::DecodeAstcImage(desc);
            }
        }
        Generatemipmaps(desc);
    } else if (path_str.ends_with("dds")) {
        dds::Image image;
        auto       result = dds::readFile(path_str, &image);
        assert(result == dds::Success);
        desc.width         = image.width;
        desc.height        = image.height;
        desc.format        = ConvertFormatFromDxgiFormat(image.format, image.supportsAlpha);
        desc.layers        = image.depth;
        desc.mips          = image.numMips;
        desc.data_size     = image.data.size();
        desc.data          = new uint8[desc.data_size];
        desc.data_callback = [](void* _ptr) {
            delete[] static_cast<uint8*>(_ptr);
        };
        memcpy(desc.data, image.data.data(), desc.data_size);
        Generatemipmaps(desc);
    } else {
        LOG_ERROR("Unsupported image format");
        throw std::runtime_error("Unsupported image format");
    }
    return desc;
}

ImageReadDesc
ImageIO::ReadFromMemory(const unsigned char* _memory_data, size_t _len, uint32_t _desired_channal) {
    ImageReadDesc desc;
    desc.data = stbi_load_from_memory(
        _memory_data,
        _len,
        reinterpret_cast<int*>(&desc.width),
        reinterpret_cast<int*>(&desc.height),
        reinterpret_cast<int*>(&desc.channel),
        _desired_channal
    );
    desc.data_callback = free;
    desc.data_size     = desc.width * desc.height * desc.channel;
    desc.format        = EPixelFormat::PF_R8G8B8A8_UNORM;
    Generatemipmaps(desc);
    return desc;
}

} // namespace Moer