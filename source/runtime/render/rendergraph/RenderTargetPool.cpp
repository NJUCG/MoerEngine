/**
 * @file RenderTargetPool.cpp
 * @brief FRenderTargetPool implementation — pooled render target management.
 *
 * Adapted from UE's FRenderTargetPool with the following simplifications:
 *   - No transient resource / memory aliasing support.
 *   - No mutex — single render-thread assumption.
 *   - No FRenderResource lifecycle (InitDynamicRHI / ReleaseDynamicRHI).
 *   - Creation goes through RenderDevice::Get().CreateTexture / CreateCubeMap directly.
 */
#include "RenderTargetPool.h"
#include "rhi/RHI.h"

#include <algorithm>
#include <cassert>
#include <functional>

namespace Moer::Render::RenderGraph {

/** Combine a hash seed with a new value (FNV-style folding). */
static inline uint32 HashCombine(uint32 Seed, uint32 Value) {
    // Boost-style hash combine.
    Seed ^= Value + 0x9e3779b9u + (Seed << 6) + (Seed >> 2);
    return Seed;
}

uint32 FRenderTargetPool::ComputeDescHash(const FPooledRenderTargetDesc& Desc) {
    uint32 H = 0;
    H        = HashCombine(H, static_cast<uint32>(Desc.Format));
    H        = HashCombine(H, static_cast<uint32>(Desc.Flags));
    H        = HashCombine(H, static_cast<uint32>(Desc.UAVFormat));
    H        = HashCombine(H, Desc.Extent.x);
    H        = HashCombine(H, Desc.Extent.y);
    H        = HashCombine(H, static_cast<uint32>(Desc.Depth));
    H        = HashCombine(H, static_cast<uint32>(Desc.ArraySize));
    H        = HashCombine(H, static_cast<uint32>(Desc.NumMips));
    H        = HashCombine(H, static_cast<uint32>(Desc.NumSamples));
    H        = HashCombine(H, static_cast<uint32>(Desc.PackedBits));
    return H;
}

/** Map FPooledRenderTargetDesc to the appropriate RenderDevice texture dimension
 *  and call the corresponding creation API. */
FPooledRenderTarget*
FRenderTargetPool::CreateRenderTarget(const FPooledRenderTargetDesc& Desc, const char* Name) {
    assert(Desc.IsValid());

    RenderDevice& Device = RenderDevice::Get();
    TextureRef    Tex;

    if (Desc.IsCubemap()) {
        // Cubemap (or cubemap array — CreateCubeMap only creates single cubes;
        // cubemap arrays are implicitly handled via ArraySize / 6).
        Tex = Device.CreateCubeMap(Name, Desc.Extent, Desc.Format, Desc.Flags, Desc.NumMips);
    } else {
        // 2D, 2D-array, or volume (3D) texture.
        Tex =
            Device.CreateTexture(Name, Desc.GetSize(), Desc.Format, Desc.Flags, Desc.NumMips, Desc.ArraySize);
    }

    assert(Tex && "RenderDevice::CreateTexture returned null");
    return new FPooledRenderTarget(std::move(Tex), Desc, this);
}

bool FRenderTargetPool::FindFreeElement(
    const FPooledRenderTargetDesc&     Desc,
    CountableRef<IPooledRenderTarget>& Out,
    const char*                        DebugName
) {
    assert(Desc.IsValid());
    const uint32 DescHash = ComputeDescHash(Desc);

    // Try to find an existing idle render target with a matching descriptor.
    for (size_t i = 0; i < PooledRenderTargets.size(); ++i) {
        auto& Slot = PooledRenderTargets[i];
        if (!Slot) {
            continue; // compacted-out slot
        }

        if (PooledRenderTargetHashes[i] != DescHash) {
            continue; // hash mismatch — fast reject
        }

        if (!Slot->IsFree()) {
            continue; // still in use
        }

        // Full field-by-field comparison.
        if (!Slot->Desc.Compare(Desc, /*bExact=*/false)) {
            continue;
        }

        // Found a reusable render target. Reset its age counter.
        Slot->UnusedForNFrames = 0;
        Out                    = Slot.Get(); // implicit AddRef via CountableRef assignment
        return true;
    }

    // No match — allocate a new one.
    FPooledRenderTarget* NewRT = CreateRenderTarget(Desc, DebugName);
    PooledRenderTargetHashes.push_back(DescHash);
    PooledRenderTargets.push_back(NewRT);
    Out = NewRT; // implicit AddRef
    return false;
}

void FRenderTargetPool::TickPoolElements() {
    for (size_t i = 0; i < PooledRenderTargets.size(); ++i) {
        auto& Slot = PooledRenderTargets[i];
        if (!Slot) {
            continue;
        }

        // OnFrameStart increments UnusedForNFrames and returns true if free.
        if (Slot->OnFrameStart()) {
            if (Slot->GetUnusedForNFrames() > kMaxUnusedFrames) {
                // Evict: clear the strong reference. If no external refs remain, RT is destroyed.
                Slot.Reset();
            }
        } else {
            // In use — reset age counter.
            Slot->UnusedForNFrames = 0;
        }
    }

    CompactPool();
}

void FRenderTargetPool::FreeUnusedResources() {
    for (size_t i = 0; i < PooledRenderTargets.size(); ++i) {
        auto& Slot = PooledRenderTargets[i];
        if (Slot && Slot->IsFree()) {
            Slot.Reset();
        }
    }

    CompactPool();
}

void FRenderTargetPool::CompactPool() {
    // Walk backwards and remove trailing null entries.
    // A full O(n) compaction (erasing from the middle) is fine because the pool
    // typically holds dozens, not thousands, of entries.
    size_t WriteIdx = 0;
    for (size_t ReadIdx = 0; ReadIdx < PooledRenderTargets.size(); ++ReadIdx) {
        if (PooledRenderTargets[ReadIdx]) {
            if (WriteIdx != ReadIdx) {
                PooledRenderTargets[WriteIdx]      = std::move(PooledRenderTargets[ReadIdx]);
                PooledRenderTargetHashes[WriteIdx] = PooledRenderTargetHashes[ReadIdx];
            }
            ++WriteIdx;
        }
    }
    PooledRenderTargets.resize(WriteIdx);
    PooledRenderTargetHashes.resize(WriteIdx);
}

} // namespace Moer::Render::RenderGraph
