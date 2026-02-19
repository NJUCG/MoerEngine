#pragma once
#include "rhi/RHIResource.h"
#include <string>

namespace Moer::Render::RenderGraph {
using ::Moer::Render::RHIClearAttachment;
using ::Moer::Render::Texture;
using ::Moer::Render::TextureRef;

/** All necessary data to create a render target from the pooled render targets. */
struct FPooledRenderTargetDesc {
public:
    /** Default constructor, use one of the factory functions below to make a valid description */
    FPooledRenderTargetDesc() : PackedBits(0) {
        assert(!IsValid());
    }

    /**
	 * Factory function to create 2D texture description
	 * @param InFlags bit mask combined from elements of ETextureCreateFlags e.g. TexCreate_UAV
	 */
    static FPooledRenderTargetDesc Create2DDesc(
        Extent2D             InExtent,
        EPixelFormat         InFormat,
        const RHIClearAttachment& InClearValue,
        ETextureUsageFlags   InFlags,
        ETextureUsageFlags   InTargetableFlags,
        bool                 bInForceSeparateTargetAndShaderResource,
        uint8                InNumMips           = 1,
        bool                 InAutowritable      = true,
        bool                 InCreateRTWriteMask = false,
        bool                 InCreateFmask       = false
    ) {
        (void)bInForceSeparateTargetAndShaderResource;
        (void)InAutowritable;
        (void)InCreateRTWriteMask;
        (void)InCreateFmask;
        assert(InExtent.x);
        assert(InExtent.y);

        FPooledRenderTargetDesc NewDesc;
        NewDesc.ClearValue = InClearValue;
        NewDesc.Extent     = InExtent;
        NewDesc.Depth      = 1;
        NewDesc.ArraySize  = 1;
        NewDesc.bIsArray   = false;
        NewDesc.bIsCubemap = false;
        NewDesc.NumMips    = InNumMips;
        NewDesc.NumSamples = 1;
        NewDesc.Format     = InFormat;
        NewDesc.Flags      = InFlags | InTargetableFlags;
        NewDesc.DebugName  = "UnknownTexture2D";
        assert(NewDesc.Is2DTexture());
        return NewDesc;
    }

    /**
 * Factory function to create 2D array texture description
 * @param InFlags bit mask combined from elements of ETextureCreateFlags e.g. TexCreate_UAV
 */
    static FPooledRenderTargetDesc Create2DArrayDesc(
        Extent2D             InExtent,
        EPixelFormat         InFormat,
        const RHIClearAttachment& InClearValue,
        ETextureUsageFlags   InFlags,
        ETextureUsageFlags   InTargetableFlags,
        bool                 bInForceSeparateTargetAndShaderResource,
        uint16               InArraySize,
        uint8                InNumMips           = 1,
        bool                 InAutowritable      = true,
        bool                 InCreateRTWriteMask = false,
        bool                 InCreateFmask       = false
    ) {
        (void)bInForceSeparateTargetAndShaderResource;
        (void)InAutowritable;
        (void)InCreateRTWriteMask;
        (void)InCreateFmask;
        assert(InExtent.x);
        assert(InExtent.y);

        FPooledRenderTargetDesc NewDesc;
        NewDesc.ClearValue = InClearValue;
        NewDesc.Extent     = InExtent;
        NewDesc.Depth      = 1;
        NewDesc.ArraySize  = InArraySize;
        NewDesc.bIsArray   = true;
        NewDesc.bIsCubemap = false;
        NewDesc.NumMips    = InNumMips;
        NewDesc.NumSamples = 1;
        NewDesc.Format     = InFormat;
        NewDesc.Flags      = InFlags | InTargetableFlags;
        NewDesc.DebugName  = "UnknownTexture2DArray";
        assert(NewDesc.Is2DTexture());

        return NewDesc;
    }

    /**
	 * Factory function to create 3D texture description
	 * @param InFlags bit mask combined from elements of ETextureCreateFlags e.g. TexCreate_UAV
	 */
    static FPooledRenderTargetDesc CreateVolumeDesc(
        uint32               InSizeX,
        uint32               InSizeY,
        uint16               InSizeZ,
        EPixelFormat         InFormat,
        const RHIClearAttachment& InClearValue,
        ETextureUsageFlags   InFlags,
        ETextureUsageFlags   InTargetableFlags,
        bool                 bInForceSeparateTargetAndShaderResource,
        uint8                InNumMips      = 1,
        bool                 InAutowritable = true
    ) {
        (void)bInForceSeparateTargetAndShaderResource;
        (void)InAutowritable;
        assert(InSizeX);
        assert(InSizeY);

        FPooledRenderTargetDesc NewDesc;
        NewDesc.ClearValue = InClearValue;
        NewDesc.Extent     = Extent2D(InSizeX, InSizeY);
        NewDesc.Depth      = InSizeZ;
        NewDesc.ArraySize  = 1;
        NewDesc.bIsArray   = false;
        NewDesc.bIsCubemap = false;
        NewDesc.NumMips    = InNumMips;
        NewDesc.NumSamples = 1;
        NewDesc.Format     = InFormat;
        NewDesc.Flags      = InFlags | InTargetableFlags;
        NewDesc.DebugName  = "UnknownTextureVolume";
        assert(NewDesc.Is3DTexture());
        return NewDesc;
    }

    /**
	 * Factory function to create cube map texture description
	 * @param InFlags bit mask combined from elements of ETextureCreateFlags e.g. TexCreate_UAV
	 */
    static FPooledRenderTargetDesc CreateCubemapDesc(
        uint32               InExtent,
        EPixelFormat         InFormat,
        const RHIClearAttachment& InClearValue,
        ETextureUsageFlags   InFlags,
        ETextureUsageFlags   InTargetableFlags,
        bool                 bInForceSeparateTargetAndShaderResource,
        uint16               InArraySize    = 1,
        uint8                InNumMips      = 1,
        bool                 InAutowritable = true
    ) {
        (void)bInForceSeparateTargetAndShaderResource;
        (void)InAutowritable;
        assert(InExtent);

        FPooledRenderTargetDesc NewDesc;
        NewDesc.ClearValue = InClearValue;
        NewDesc.Extent     = Extent2D(InExtent, InExtent);
        NewDesc.Depth      = 1;
        NewDesc.ArraySize  = InArraySize * 6u;
        // Note: this doesn't allow an array of size 1
        NewDesc.bIsArray   = InArraySize > 1;
        NewDesc.bIsCubemap = true;
        NewDesc.NumMips    = InNumMips;
        NewDesc.NumSamples = 1;
        NewDesc.Format     = InFormat;
        NewDesc.Flags      = InFlags | InTargetableFlags;
        NewDesc.DebugName  = "UnknownTextureCube";
        assert(NewDesc.IsCubemap());

        return NewDesc;
    }

    /**
	 * Factory function to create cube map array texture description
	 * @param InFlags bit mask combined from elements of ETextureCreateFlags e.g. TexCreate_UAV
	 */
    static FPooledRenderTargetDesc CreateCubemapArrayDesc(
        uint32               InExtent,
        EPixelFormat         InFormat,
        const RHIClearAttachment& InClearValue,
        ETextureUsageFlags   InFlags,
        ETextureUsageFlags   InTargetableFlags,
        bool                 bInForceSeparateTargetAndShaderResource,
        uint16               InArraySize,
        uint8                InNumMips      = 1,
        bool                 InAutowritable = true
    ) {
        (void)bInForceSeparateTargetAndShaderResource;
        (void)InAutowritable;
        assert(InExtent);

        FPooledRenderTargetDesc NewDesc;
        NewDesc.ClearValue = InClearValue;
        NewDesc.Extent     = Extent2D(InExtent, InExtent);
        NewDesc.Depth      = 1;
        NewDesc.ArraySize  = static_cast<uint16>(InArraySize * 6u);
        NewDesc.bIsArray   = true;
        NewDesc.bIsCubemap = true;
        NewDesc.NumMips    = InNumMips;
        NewDesc.NumSamples = 1;
        NewDesc.Format     = InFormat;
        NewDesc.Flags      = InFlags | InTargetableFlags;
        NewDesc.DebugName  = "UnknownTextureCubeArray";
        assert(NewDesc.IsCubemap());

        return NewDesc;
    }

    /** Comparison operator to test if a render target can be reused */
    bool Compare(const FPooledRenderTargetDesc& rhs, bool bExact) const {
        (void)bExact;
        return ClearValue == rhs.ClearValue && Flags == rhs.Flags && Format == rhs.Format &&
               UAVFormat == rhs.UAVFormat && Extent == rhs.Extent && Depth == rhs.Depth &&
               ArraySize == rhs.ArraySize && NumMips == rhs.NumMips && NumSamples == rhs.NumSamples &&
               PackedBits == rhs.PackedBits && AliasableFormats == rhs.AliasableFormats;
    }

    bool IsCubemap() const {
        return bIsCubemap;
    }

    bool Is2DTexture() const {
        return Extent.x != 0 && Extent.y != 0 && Depth <= 1 && !bIsCubemap;
    }

    bool Is3DTexture() const {
        return Extent.x != 0 && Extent.y != 0 && Depth > 1 && !bIsCubemap;
    }

    // @return true if this texture is a texture array
    bool IsArray() const {
        return bIsArray;
    }

    bool IsValid() const {
        if (NumSamples != 1) {
            if (NumSamples < 1 || NumSamples > 8) {
                // D3D11 limitations
                return false;
            }

            if (!Is2DTexture()) {
                return false;
            }
        }

        return Extent.x != 0 && NumMips != 0 && NumSamples >= 1 && NumSamples <= 16 && Format != PF_UNDEFINED;
    }

    Extent3D GetSize() const {
        return Extent3D(Extent, Depth);
    }

    /** 
	 * for debugging purpose
	 * @return e.g. (2D 128x64 PF_R8)
	 */
    std::string GenerateInfoString() const {
        std::string type;
        if (IsCubemap()) {
            type = "Cube";
        } else if (Is3DTexture()) {
            type = "3D";
        } else if (Is2DTexture()) {
            type = "2D";
        } else {
            return "(INVALID)";
        }

        std::string array_suffix;
        if (IsArray()) {
            array_suffix = "[" + std::to_string(ArraySize) + "]";
        }

        std::string samples_suffix;
        if (NumSamples > 1) {
            samples_suffix = " " + std::to_string(NumSamples) + "xMSAA";
        }

        return "(" + type + array_suffix + " " + std::to_string(Extent.x) + "x" +
               std::to_string(Extent.y) + " PF_" + std::to_string(static_cast<uint32>(Format)) +
               samples_suffix + ")";
    }

    // useful when compositing graph takes an input as output format
    void Reset() {
        // Usually we don't want to propagate MSAA samples.
        NumSamples = 1;
        // Remove UAV flag for rendertargets that don't need it (some formats are incompatible)
        EnumRemoveFlags(Flags, ETextureUsageFlags::UNORDERED_ACCESS);
    }

    /** only set a pointer to memory that never gets released */
    const char* DebugName = "UnknownTexture";
    /** Value allowed for fast clears for this target. */
    RHIClearAttachment ClearValue;
    /** Usage flags for this texture (bit mask combined from elements of ETextureUsageFlags). */
    ETextureUsageFlags Flags = ETextureUsageFlags::UNDEFINED;
    /** Texture format e.g. PF_B8G8R8A8 */
    EPixelFormat Format = PF_UNDEFINED;
    /** Texture format used when creating the UAV (if TexCreate_UAV is also passed in TargetableFlags, ignored otherwise). PF_Unknown == use default (same as Format) */
    EPixelFormat UAVFormat = PF_UNDEFINED;
    /** In pixels, (0,0) if not set, (x,0) for cube maps, todo: make 3d int vector for volume textures */
    Extent2D Extent = Extent2D(0, 0);
    /** 1 for 2D textures, >1 for volume textures. */
    uint16 Depth = 1;
    /** >1 if a texture array should be used (not supported on DX9) */
    uint16 ArraySize = 1;
    /** Number of mips */
    uint8 NumMips = 0;
    /** Number of MSAA samples, default: 1  */
    uint8 NumSamples = 1;
    /** Resource memory percentage which should be allocated onto fast VRAM (hint-only). (encoding into 8bits, 0..255 -> 0%..100%) */
    /** Formats this texture is aliasable to */
    Array<EPixelFormat> AliasableFormats;

    union {
        struct {
            /** true if an array texture. Note that ArraySize still can be 1 */
            uint8 bIsArray : 1;
            /** true if a cubemap texture */
            uint8 bIsCubemap : 1;
            /** Unused flags. */
            uint8 bReserved0 : 6;
        };
        uint8 PackedBits;
    };
};

/**
 * Render thread side, use TRefCountPtr<IPooledRenderTarget>, allows sharing and VisualizeTexture
 */
struct FSceneRenderTargetItem {
    TextureRef TargetableTexture;
};

struct IPooledRenderTarget {
    virtual ~IPooledRenderTarget() {}

    /** Checks if the reference count indicated that the rendertarget is unused and can be reused. */
    virtual bool IsFree() const = 0;

    /** Get all the data that is needed to create the render target. */
    virtual const FPooledRenderTargetDesc& GetDesc() const = 0;

    /**
	 * Only for debugging purpose
	 * @return in bytes
	 **/
    virtual uint32 ComputeMemorySize() const = 0;

    /** Returns if the render target is tracked by a pool. */
    virtual bool IsTracked() const = 0;

    /** Returns a transient texture if this is a container for one. */
    virtual Texture* GetTransientTexture() const {
        return nullptr;
    }

    // Refcounting
    virtual uint32 AddRef() const      = 0;
    virtual uint32 Release()           = 0;
    virtual uint32 GetRefCount() const = 0;

    Texture* GetRHI() const {
        return RenderTargetItem.TargetableTexture.Get();
    }

protected:
    /** The internal references to the created render target */
    FSceneRenderTargetItem RenderTargetItem;
};

} // namespace Moer::Render::RenderGraph
