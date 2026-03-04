#pragma once
#include "RenderGraphResource.h"
#include "RendererInterface.h"
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
        uint32 RefCount = GetRefCount();
        assert(RefCount >= 1);
        // If the only reference is from the pool's CountableRef, it's unused.
        return RefCount == 1;
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

//////////////////////////////////////////////////////////////////////////
// FRenderTargetPool
//////////////////////////////////////////////////////////////////////////

/** Render target texture pool.
 *
 *  Manages a pool of FPooledRenderTarget objects for reuse across frames.
 *  Simplified from UE:
 *    - No transient resource support (no memory aliasing / placed resources)
 *    - No mutex (single render-thread assumption)
 *    - No FRenderResource base class / global resource lifecycle
 *    - No FRHITextureCreateInfo translation; uses FPooledRenderTargetDesc directly
 *
 *  Lifecycle:
 *    1. Each frame, call TickPoolElements() to age idle RTs and evict stale ones.
 *    2. When a RT is needed, call FindFreeElement() — it returns an existing
 *       compatible idle RT if one is found, or creates a new one via RenderDevice.
 *    3. When the CountableRef goes out of scope (refcount → 0), the RT becomes
 *       "free" and eligible for reuse in subsequent FindFreeElement() calls.
 *    4. Call FreeUnusedResources() between levels or before heavy allocations
 *       to release all idle RTs immediately.
 *
 *  Hash-based matching:
 *    Each FPooledRenderTargetDesc is hashed (format, extent, mips, flags, etc.)
 *    for O(1) candidate lookup. Compare() performs the exact field-by-field check.
 */
class FRenderTargetPool {
public:
    FRenderTargetPool() = default;

    /** Find a free (refcount == 0) render target matching Desc, or create a new one.
     *  @param Desc       Desired texture properties (format, extent, mips, flags, etc.)
     *  @param Out        [out] Receives the pooled render target. Previous contents are released.
     *  @param DebugName  Debug label for the texture (pointer must remain valid; typically a literal).
     *  @return true if an existing idle RT was reused; false if a new one was allocated. */
    RENDER_API bool FindFreeElement(
        const FPooledRenderTargetDesc&     Desc,
        CountableRef<IPooledRenderTarget>& Out,
        const char*                        DebugName
    );

    /** Call once per frame (render thread) to age idle RTs.
     *  RTs that have been free for more than kMaxUnusedFrames consecutive frames
     *  are released to reclaim GPU memory. */
    RENDER_API void TickPoolElements();

    /** Immediately release all RTs whose refcount is 0.
     *  Useful between levels or before memory-intensive operations. */
    RENDER_API void FreeUnusedResources();

    /** @return Number of slots in the pool (including empty / compacted-out entries). */
    uint32 GetElementCount() const {
        return static_cast<uint32>(PooledRenderTargets.size());
    }

private:
    /** Number of consecutive idle frames before a free RT is evicted. */
    static constexpr uint32 kMaxUnusedFrames = 3;

    /** Compute a fast hash from a FPooledRenderTargetDesc for pool lookup. */
    static uint32 ComputeDescHash(const FPooledRenderTargetDesc& Desc);

    /** Remove nullptr entries from PooledRenderTargets / PooledRenderTargetHashes. */
    void CompactPool();

    /** Create the RHI texture described by Desc and wrap it in a new FPooledRenderTarget. */
    FPooledRenderTarget* CreateRenderTarget(const FPooledRenderTargetDesc& Desc, const char* Name);

    //////////////////////////////////////////////////////////////////////////
    // Pool storage

    /** Parallel arrays: hash[i] corresponds to PooledRenderTargets[i].
     *  Entries may be null after eviction; CompactPool() removes them. */
    Array<uint32>                            PooledRenderTargetHashes;
    Array<CountableRef<FPooledRenderTarget>> PooledRenderTargets;

    friend struct FPooledRenderTarget;
    friend class FRDGBuilder;
};

} // namespace Moer::Render::RenderGraph
