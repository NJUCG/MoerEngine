#pragma once
#include "RendererInterface.h"
#include "RenderGraphResource.h"
#include <atomic>

namespace Moer::Render::RenderGraph {

// Forward declarations
class FRenderTargetPool;

/** Concrete pooled render target. Implements IPooledRenderTarget with std::atomic refcounting.
 *  Adapted from UE: removed ShaderResourceTexture/UAV (viewless RHI),
 *  replaced FPlatformAtomics with std::atomic, removed TCHAR. */
struct FPooledRenderTarget final : public IPooledRenderTarget {
    FPooledRenderTarget(
        TextureRef                     InTexture,
        const FPooledRenderTargetDesc& InDesc,
        FRenderTargetPool*             InRenderTargetPool = nullptr
    ) :
        RenderTargetPool(InRenderTargetPool),
        Desc(InDesc),
        PooledTexture(InTexture) {
        RenderTargetItem.TargetableTexture = std::move(InTexture);
    }

    uint32 GetUnusedForNFrames() const {
        return UnusedForNFrames;
    }

    const FPooledRenderTargetDesc& GetDesc() const override {
        return Desc;
    }

    uint32 AddRef() const override {
        return static_cast<uint32>(NumRefs.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    uint32 Release() override {
        const int32_t Refs = NumRefs.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (Refs == 0) {
            delete this;
        }
        return static_cast<uint32>(Refs);
    }

    uint32 GetRefCount() const override {
        return static_cast<uint32>(NumRefs.load(std::memory_order_relaxed));
    }

    bool IsFree() const override {
        return NumRefs.load(std::memory_order_relaxed) == 0;
    }

    bool IsTracked() const override {
        return RenderTargetPool != nullptr;
    }

    uint32 ComputeMemorySize() const override {
        // TODO: implement proper memory size computation from Desc
        return 0;
    }

private:
    /** Pointer back to the pool for render targets which are actually pooled, otherwise nullptr. */
    FRenderTargetPool* RenderTargetPool = nullptr;

    /** All necessary data to create the render target */
    FPooledRenderTargetDesc Desc;

    /** For pool management (only if NumRefs == 0 the element can be reused) */
    mutable std::atomic<int32_t> NumRefs{0};

    /** Allows to defer the release to save performance on some hardware */
    uint32 UnusedForNFrames = 0;

    /** Pooled texture wrapper for use with RDG. */
    FRDGPooledTexture PooledTexture;

    /** @return true: release this one, false: keep */
    bool OnFrameStart() {
        ++UnusedForNFrames;
        return IsFree();
    }

    friend class FRDGTexture;
    friend class FRDGBuilder;
    friend class FRenderTargetPool;
};

} // namespace Moer::Render::RenderGraph
