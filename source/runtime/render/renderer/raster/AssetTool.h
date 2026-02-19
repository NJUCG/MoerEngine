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
    TextureRef  tex;
    uint        handle;      //主Handle
    Array<uint> mip_handles; //每个Mip的Handle

    uint2 GetSize(uint mip = 0) {
        return uint2(std::max(1u, tex->GetExtent().x >> mip), std::max(1u, tex->GetExtent().y >> mip));
    }
    uint GetSizeX(uint mip = 0) {
        return std::max(1u, tex->GetExtent().x >> mip);
    }
    uint GetSizeY(uint mip = 0) {
        return std::max(1u, tex->GetExtent().y >> mip);
    }
    Rect2D GetRect2D(uint mip = 0) {
        uint2 size = GetSize(mip);
        return Rect2D(0, 0, size.x, size.y);
    }

    uint GetMipHandle(uint mip) {
        if (mip_handles.size() > mip) {
            return mip_handles[mip];
        }
        return handle;
    }
};
struct DepthBufferWithHandle {
    DepthBufferRef tex;
    uint           handle = 0;
};

struct BufferWithHandle {
    BufferRef buf;
    uint      handle = 0;
};

// 如果texture的名字不是编译期决定的，则需要找一个地方存名字。否则string_view会出现悬垂指针
struct DepthBufferWithHandleAndName {
    DepthBufferRef tex;
    uint           handle = 0;
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
    TexConfig& IndivisualMips() {
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
        requires(std::is_same_v<Tag, Tex2DTag>)
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

        LOG_DEBUG("Load 2D Texture: {}, size ({}, {}) from \"{}\"", name, width, height, filepath);
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

            LOG_DEBUG(
                "Load CubeMap Texture: {}_{}, size ({}, {}) from \"{}\"",
                name,
                skybox_faces[i],
                width,
                height,
                filepath
            );
        }
    }

    template<typename IntentTag, typename T_Holder>
    static void CreateRasterResource(
        T_Holder&        target,
        RenderDevice&    device,
        std::string_view name,
        const uint2&     size,
        TexConfig&       cfg,
        bool             is_verbose = true
    ) {
        if (cfg.is_asset) {
            // 资源纹理不在这里创建
            return;
        }
        if (cfg.alias_ptr) {
            target.tex = static_cast<T_Holder*>(cfg.alias_ptr)->tex;
        } else {
            if constexpr (std::is_same_v<IntentTag, TexDepthTag>) {
                cfg.type = TexType::TEX_TYPE_DEPTH;
                cfg.dim  = ETextureDimension::TEX_2D;

                target.tex = device.CreateDepthBuffer(
                    name,
                    (cfg.b_super_resolution ? Extent2D(size.x / 2, size.y / 2) : Extent2D(size.x, size.y)),
                    cfg.format,
                    1,
                    cfg.usage
                );
            } else if constexpr (std::is_same_v<IntentTag, TexCubeTag>) {
                cfg.type = TexType::TEX_TYPE_CUBE;
                cfg.dim  = ETextureDimension::TEX_CUBE;
                cfg.size = {0, 0, 6};

                target.tex = device.CreateCubeMap(
                    name,
                    (cfg.b_super_resolution ? Extent2D(size.x / 2, size.y / 2) : Extent2D(size.x, size.y)),
                    cfg.format,
                    cfg.usage,
                    cfg.mip_cnt
                );
            } else if constexpr (std::is_same_v<IntentTag, Tex2DTag>) {
                cfg.type = TexType::TEX_TYPE_2D;
                cfg.dim  = ETextureDimension::TEX_2D;

                target.tex = device.CreateTexture(
                    name,
                    (cfg.b_super_resolution ? Extent2D(size.x / 2, size.y / 2) : Extent2D(size.x, size.y)),
                    cfg.format,
                    cfg.usage,
                    cfg.mip_cnt
                );
            } else {
                static_assert(always_false<T_Holder>, "Unsupported Tex IntentTag");
            }

            if (is_verbose) {
                LOG_DEBUG(
                    "tex {}, size {} x {}",
                    name,
                    (cfg.b_super_resolution ? size.x / 2 : size.x),
                    (cfg.b_super_resolution ? size.y / 2 : size.y)
                );
            }
        }
    }

    template<typename T>
        requires requires(T t) {
            t.handle;
            t.mip_handles;
            t.tex;
        }
    static void
    AllocateRasterResourceHandle(BindlessArrayRef& bindless_array, T& target, const TexConfig& cfg) {
        //Main Handle
        target.handle = bindless_array->AllocateTexture(target.tex->GetView(), cfg.sampler);

        // Mip Handles
        if (cfg.b_create_mip_views) {
            target.mip_handles.resize(cfg.mip_cnt);
            for (uint mip = 0; mip < cfg.mip_cnt; ++mip) {
                target.mip_handles[mip] = bindless_array->AllocateTexture(
                    target.tex->GetView(static_cast<uint8>(mip), 1), cfg.sampler
                );
            }
        }
    }

    template<typename T>
        requires requires(T t) {
            t.handle;
            t.tex;
        } && (!requires(T t) { t.mip_handles; })
    static void
    AllocateRasterResourceHandle(BindlessArrayRef& bindless_array, T& target, const TexConfig& cfg) {
        //Main Handle
        target.handle = bindless_array->AllocateTexture(target.tex->GetView(), cfg.sampler);
    }

    //方案：tex和handle的生命周期绑定
    //TODO:理论上外部资源纹理分辨率固定，不需要释放显存，但如何管理好呢？
    template<typename T>
        requires requires(T t) {
            t.handle;
            t.mip_handles;
            t.tex;
        }
    static void FreeRasterResourceHandle(BindlessArrayRef& bindless_array, T& target) {
        //Main Handle
        if (target.handle != 0) {
            bindless_array->UnbindTexture(target.handle);
            target.handle = 0;
            target.tex    = nullptr;
        } else {
            LOG_WARNING("Trying to free a texture handle that is already zeroed.");
        }

        // Mip Handles
        for (uint& hdl : target.mip_handles) {
            if (hdl != 0) {
                bindless_array->UnbindTexture(hdl);
                hdl = 0;
            } else {
                LOG_WARNING("Trying to free a mip texture handle that is already zeroed.");
            }
        }
        target.mip_handles.clear();
    }

    template<typename T>
        requires requires(T t) {
            t.handle;
            t.tex;
        } && (!requires(T t) { t.mip_handles; })
    static void FreeRasterResourceHandle(BindlessArrayRef& bindless_array, T& target) {
        //Main Handle
        if (target.handle != 0) {
            bindless_array->UnbindTexture(target.handle);
            target.handle = 0;
            target.tex    = nullptr;
        } else {
            LOG_WARNING("Trying to free a texture handle that is already zeroed.");
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