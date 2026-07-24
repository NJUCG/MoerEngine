#pragma once

#include "RenderAPI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string_view>
#include <vector>

namespace Moer::Render {

class RenderGraph;

/**
 * Physical allocation description for a transient RDG texture.
 *
 * array_size is the number of array elements passed to the RHI factory. Cube
 * textures therefore expose 6 * array_size physical layers.
 */
struct RENDER_API RGTransientTextureDesc {
    ETextureDimension  dimension    = ETextureDimension::TEX_2D;
    Extent3D           extent       = Extent3D(1, 1, 1);
    EPixelFormat       format       = PF_UNDEFINED;
    ETextureUsageFlags usage        = ETextureUsageFlags::UNDEFINED;
    ETextureAspectFlags aspect_flags = ETextureAspectFlags::COLOR;
    uint32_t           mip_count    = 1;
    uint32_t           array_size   = 1;

    [[nodiscard]] uint32_t PhysicalLayerCount() const;
    [[nodiscard]] bool     IsValid() const;

    friend bool operator==(const RGTransientTextureDesc&, const RGTransientTextureDesc&) = default;
};

/** Physical allocation description for a transient RDG buffer. */
struct RENDER_API RGTransientBufferDesc {
    uint32_t          element_count = 0;
    uint32_t          stride        = 0;
    EBufferUsageFlags usage         = EBufferUsageFlags::NONE;
    EPixelFormat      format        = PF_UNDEFINED;

    [[nodiscard]] uint64_t ByteSize() const;
    [[nodiscard]] bool     IsValid() const;

    friend bool operator==(const RGTransientBufferDesc&, const RGTransientBufferDesc&) = default;
};

/**
 * Cross-frame physical resource pool.
 *
 * The pool owns exactly one reference to every cached resource. A resource is
 * reusable only when its intrusive reference count is one, so CommandList
 * completion callbacks naturally keep in-flight allocations unavailable.
 *
 * Custom pools and all resources acquired from them must be reset or destroyed
 * before RenderDevice::Dispose(). The device automatically resets only Global().
 */
class RENDER_API RenderGraphResourcePool {
public:
    using TextureFactory =
        std::function<TextureRef(std::string_view, const RGTransientTextureDesc&)>;
    using BufferFactory =
        std::function<BufferRef(std::string_view, const RGTransientBufferDesc&)>;

    explicit RenderGraphResourcePool(
        uint32_t       retire_after_idle_frames = 3,
        TextureFactory texture_factory          = {},
        BufferFactory  buffer_factory           = {}
    );

    static RenderGraphResourcePool& Global();

    TextureRef AcquireTexture(std::string_view name, const RGTransientTextureDesc& desc);
    BufferRef  AcquireBuffer(std::string_view name, const RGTransientBufferDesc& desc);

    /** Advances idle retirement once. Call at most once per rendered frame. */
    void Tick();
    /** Drops the pool's references. In-flight users remain protected by their own refs. */
    void Reset();

    [[nodiscard]] uint32_t TextureCount() const;
    [[nodiscard]] uint32_t BufferCount() const;
    [[nodiscard]] uint32_t AvailableTextureCount() const;
    [[nodiscard]] uint32_t AvailableBufferCount() const;

private:
    struct TextureEntry {
        RGTransientTextureDesc desc{};
        TextureRef            resource{};
        uint32_t              idle_frames = 0;
    };

    struct BufferEntry {
        RGTransientBufferDesc desc{};
        BufferRef             resource{};
        uint32_t              idle_frames = 0;
    };

    TextureRef AcquireTextureLocked(std::string_view name, const RGTransientTextureDesc& desc);
    BufferRef  AcquireBufferLocked(std::string_view name, const RGTransientBufferDesc& desc);

    mutable std::mutex       mutex{};
    std::vector<TextureEntry> textures{};
    std::vector<BufferEntry>  buffers{};
    uint32_t                  retire_after_idle_frames = 3;
    TextureFactory            texture_factory{};
    BufferFactory             buffer_factory{};
};

/**
 * Binds compiled transient allocation slots to pooled physical resources.
 *
 * Logical slot coloring belongs to RenderGraphCompiler. This class performs
 * no scheduling decisions; it only materializes the already validated plan.
 */
class RENDER_API RenderGraphTransientAllocator {
public:
    explicit RenderGraphTransientAllocator(RenderGraphResourcePool& pool) : pool(pool) {}

    static RenderGraphTransientAllocator& Global();

    [[nodiscard]] bool Prepare(RenderGraph& graph, std::string& error);
    void               ReleaseNonExported(RenderGraph& graph) noexcept;

private:
    RenderGraphResourcePool& pool;
};

} // namespace Moer::Render
