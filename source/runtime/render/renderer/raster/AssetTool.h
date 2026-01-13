#pragma once

#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "scene/Scene.h"
#include "shader/ShaderPipeline.h"
#include "shaderheaders/shared/raster/lighting_pass/ShaderParameters.h"
#include "shaderheaders/shared/raster/post_process/ShaderParameters.h"
#include <config/ConfigManager.h>
#include <shader/ShaderResourceManager.h>
#include <stb_image.h>

#include "RasterCompileTimeConstants.h"
#include "RasterTextures.h"

namespace Moer::Render::Raster {
struct Tex2DTag {};
struct TexCubeTag {};
struct TexDepthTag {};

template<typename...>
inline constexpr bool always_false = false;

struct TextureWithHandle {
    TextureRef tex;
    uint       handle;

    uint2 GetSize() {
        return uint2(tex->GetExtent().x, tex->GetExtent().y);
    }
    uint GetSizeX() {
        return tex->GetExtent().x;
    }
    uint GetSizeY() {
        return tex->GetExtent().y;
    }
    Rect2D GetRect2D() {
        return Rect2D(0, 0, GetSizeX(), GetSizeY());
    }
};
struct DepthBufferWithHandle {
    DepthBufferRef tex;
    uint           handle;
};

struct BufferWithHandle {
    BufferRef buf;
    uint      handle;
};

// 如果texture的名字不是编译期决定的，则需要找一个地方存名字。否则string_view会出现悬垂指针
struct DepthBufferWithHandleAndName {
    DepthBufferRef tex;
    uint           handle;
    std::string    name;
};

enum TexType {
    TEX_TYPE_2D,
    TEX_TYPE_CUBE,
    TEX_TYPE_DEPTH,
    NUMBITS = 3
};

struct TexConfig {

    EPixelFormat       format;
    ETextureUsageFlags usage;
    TexType            type = TexType::TEX_TYPE_2D;
    ETextureDimension  dim  = ETextureDimension::TEX_2D;
    Sampler            sampler{ESamplerFilter::SF_LINEAR, ESamplerAddressMode::SAM_REPEAT};
    Extent3D           size       = {0, 0, 1};
    uint               mip_cnt    = 1;
    uint               array_size = 1;

    std::string asset_path_relative;
    void*       alias_ptr          = nullptr;
    bool        is_asset           = false;
    bool        b_create_mip_views = false;
    bool        b_super_resolution = false;

    //为Depth设计，共用一张纹理
    template<typename T>
    TexConfig& From(T& master) {
        alias_ptr = &master;
        return *this;
    }

    // 资源纹理配置
    static TexConfig Asset(const std::string& path) {
        TexConfig cfg;
        cfg.asset_path_relative = path;
        cfg.is_asset            = true;
        return cfg;
    }

    // 瞬时纹理配置
    static TexConfig Default(EPixelFormat pf) {
        TexConfig cfg;
        cfg.format = pf;
        return cfg;
    }

    static TexConfig Color(EPixelFormat pf) {
        TexConfig cfg;
        cfg.format = pf;
        cfg.type   = TexType::TEX_TYPE_2D;
        cfg.dim    = ETextureDimension::TEX_2D;
        return cfg;
    }

    static TexConfig Depth(EPixelFormat pf) {
        TexConfig cfg;
        cfg.format = pf;
        cfg.type   = TexType::TEX_TYPE_DEPTH;
        cfg.dim    = ETextureDimension::TEX_2D;
        return cfg;
    }

    static TexConfig CubeMap(EPixelFormat pf) {
        TexConfig cfg;
        cfg.format = pf;
        cfg.size   = {0, 0, 6};
        cfg.type   = TexType::TEX_TYPE_CUBE;
        cfg.dim    = ETextureDimension::TEX_CUBE;
        return cfg;
    }

    //设定Mip层级数量
    TexConfig& Mips(uint m) {
        mip_cnt = m;
        return *this;
    }

    //只为我们需要显式访问不同Mip的纹理开启
    TexConfig& IndividualMips() {
        b_create_mip_views = true;
        return *this;
    }

    TexConfig& Format(EPixelFormat pf) {
        format = pf;
        return *this;
    }

    TexConfig& Usage(ETextureUsageFlags u) {
        usage = u;
        return *this;
    }

    TexConfig& SR(bool b) {
        b_super_resolution = b;
        return *this;
    }

    TexConfig& Size(Extent2D s) {
        size = Extent3D(s, 1);
        return *this;
    }

    TexConfig& Size(Extent3D s) {
        size = s;
        return *this;
    }

    TexConfig& SamplerConfig(ESamplerFilter filter, ESamplerAddressMode address_mode) {
        sampler.filter       = filter;
        sampler.address_mode = address_mode;
        return *this;
    }
};

class AssetTool {
public:
    //为了让DepthBuffer编译通过
    template<typename Tag>
        requires(std::is_same_v<Tag, TexDepthTag>)
    static void LoadTexture(
        RenderDevice&          device,
        CommandList&           cmd_list,
        DepthBufferRef&        out_tex,
        const TexConfig&       cfg,
        const std::string_view name = "defaultname"
    ) {}

    template<typename Tag>
        requires(std::is_same_v<Tag, Tex2DTag> || std::is_same_v<Tag, TexDepthTag>)
    static void LoadTexture(
        RenderDevice&          device,
        CommandList&           cmd_list,
        TextureRef&            out_tex,
        const TexConfig&       cfg,
        const std::string_view name = "defaultname"
    ) {

        std::string filepath =
            (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" / cfg.asset_path_relative)
                .string();

        int    width, height;
        ubyte* data = LoadImageData(filepath, width, height);

        if (!data)
            return;

        out_tex = device.CreateTexture(name, Extent2D(width, height), cfg.format, cfg.usage, cfg.mip_cnt);

        UploadTextureData(cmd_list, out_tex, data, width, height, name.data());
    }

    template<typename Tag>
        requires(std::is_same_v<Tag, TexCubeTag>)
    static void LoadTexture(
        RenderDevice&          device,
        CommandList&           cmd_list,
        TextureRef&            out_tex,
        const TexConfig&       cfg,
        const std::string_view name = "defaultname"
    ) {

        const std::array<std::string, 6> skybox_faces = {
            "posx.jpg", "negx.jpg", "posy.jpg", "negy.jpg", "posz.jpg", "negz.jpg"
        };

        TextureView skybox_view;

        for (size_t i = 0; i < skybox_faces.size(); ++i) {
            std::string filepath = (ConfigManager::GetInstance().GetEditorResourcePath() / "textures" /
                                    cfg.asset_path_relative / skybox_faces[i])
                                       .string();

            int    width, height;
            ubyte* data = LoadImageData(filepath, width, height);

            if (!data)
                return;

            if (i == 0) {
                out_tex     = device.CreateCubeMap(name, Extent2D(width, height), cfg.format, cfg.usage);
                skybox_view = TextureView(out_tex);
            }

            UploadTextureData(
                cmd_list, skybox_view.Slice(i), data, width, height, std::format("Skybox Cubemap #{}", i)
            );
        }
    }

private:
    static void UploadTextureData(
        CommandList&       cmd_list,
        TextureView        target,
        ubyte*             data,
        int                width,
        int                height,
        const std::string& debug_name
    ) {
        cmd_list.CopyFrom(std::span<Moer::byte>((Moer::byte*)data, width * height * 4), target, debug_name);
        cmd_list.AddCallback([data]() {
            stbi_image_free(data);
        });
    }

    static ubyte* LoadImageData(const std::string& path, int& width, int& height) {
        FILE* file = nullptr;
        fopen_s(&file, path.c_str(), "rb");

        if (!file) {
            LOG_ERROR("Failed to load texture file: {}", path);
            return nullptr;
        }

        int    channels;
        ubyte* data = stbi_load_from_file(file, &width, &height, &channels, 4);

        if (!data) {
            LOG_ERROR("Failed to decode texture data: {}", path);
            fclose(file);
            return nullptr;
        }

        fclose(file);
        return data;
    }
};
} // namespace Moer::Render::Raster