#pragma once
#include <algorithm>

#include "PooledRenderTarget.h"
#include "RHIAccess.h"
#include "RenderGraphDefinitions.h"
#include "RenderGraphFwd.h"
#include "RenderGraphTextureSubresource.h"
#include "misc/CountableRef.h"
#include "misc/STL.h"
#include "rhi/RHI.h"
#include "rhi/RHICommand.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

namespace Moer::Render::RenderGraph {

using FRDGPassHandlesByPipeline = TRHIPipelineArray<FRDGPassHandle>;

// 在 GPU 渲染中，为了加速性能，硬件会在显存里为 Texture 额外维护一些“缩略信息”或“压缩标记”，这些被称为 Hardware Metadata。
// 这个枚举的作用是告诉 RDG：“我这个操作是否需要访问这些硬件加速位？”
// 目前应该采用None就行
enum class ERDGTextureMetaDataAccess : uint8 {
    None,
    HTILE,
    CCS,
    Stencil
};

/** Barrier location controls where the barrier is 'Ended' relative to the pass lambda being executed.
 *  Most barrier locations are done in the prologue prior to the executing lambda. But certain cases
 *  like an aliasing discard operation need to be done *after* the pass being invoked. Therefore, when
 *  adding a transition the user can specify where to place the barrier.
 */
enum class ERDGBarrierLocation : uint8 {
    /** The barrier occurs in the prologue of the pass (before execution). */
    Prologue,

    /** The barrier occurs in the epilogue of the pass (after execution). */
    Epilogue
};

/** Used for tracking pass producer / consumer edges in the graph for culling and pipe fencing. */
struct FRDGProducerState {
    FRDGPass*      Pass                 = nullptr;
    FRDGPass*      PassIfSkipUAVBarrier = nullptr;
    FRDGPass*      PassIfReadAccess     = nullptr;
    ERHIAccess     Access               = ERHIAccess::Unknown;
    FRDGViewHandle NoUAVBarrierHandle;
};

using FRDGProducerStatesByPipeline = TRHIPipelineArray<FRDGProducerState>;

/** Used for tracking the state of an individual subresource during execution. */
struct FRDGSubresourceState {
    /** Given a before and after state, returns whether a resource barrier is required. */
    static bool IsTransitionRequired(const FRDGSubresourceState& Previous, const FRDGSubresourceState& Next);

    /** Given a before and after state, returns whether they can be merged into a single state. */
    static bool IsMergeAllowed(
        ERDGViewableResourceType    ResourceType,
        const FRDGSubresourceState& Previous,
        const FRDGSubresourceState& Next
    );

    FRDGSubresourceState() = default;

    explicit FRDGSubresourceState(ERHIAccess InAccess) : Access(InAccess) {}

    explicit FRDGSubresourceState(ERHIPipeline Pipeline, FRDGPassHandle PassHandle) {
        SetPass(Pipeline, PassHandle);
    }

    /** Initializes the first and last pass and the pipeline. Clears any other pass state. */
    void SetPass(ERHIPipeline Pipeline, FRDGPassHandle PassHandle);

    /** Validates that the state is in a correct configuration for use. */
    void Validate();

    /** Returns whether the state is used by the pipeline. */
    bool IsUsedBy(ERHIPipeline Pipeline) const;

    /** Returns the last pass across either pipe. */
    FRDGPassHandle GetLastPass() const;

    /** Returns the first pass across either pipe. */
    FRDGPassHandle GetFirstPass() const;

    /** Returns the pipeline mask this state is used on. */
    ERHIPipeline GetPipelines() const;

    /** The last used access on the pass. */
    ERHIAccess Access = ERHIAccess::Unknown;

    /** The first pass in this state. */
    FRDGPassHandlesByPipeline FirstPass;

    /** The last pass in this state. */
    FRDGPassHandlesByPipeline LastPass;

    //TODO：Recover
    // /** The last no-UAV barrier to be used by this subresource. */
    // FRDGViewUniqueFilter NoUAVBarrierFilter;

    // /** Whether this subresource state represents a commit operation for a reserved resource. */
    // FRDGBufferReservedCommitHandle ReservedCommitHandle;

    // /** The last used transition flags on the pass. */
    // EResourceTransitionFlags Flags = EResourceTransitionFlags::None;

    // /** Used to specify whether the state is applied during the prologue or epilogue of the pass. This is only used when transitioning on the same pass / pipe. */
    // ERDGBarrierLocation BarrierLocation = ERDGBarrierLocation::Prologue;
};

class FRDGResource {
public:
    FRDGResource(const FRDGResource&) = delete;
    virtual ~FRDGResource()           = default;

    // Name of the resource for debugging purpose.
    const char* Name = nullptr;

    /** Marks this resource as actually used by a resource. This is to track what dependencies on pass was actually unnecessary. */
    inline RENDER_API void MarkResourceAsUsed() {}

    RHIResource* GetRHI() const {
        return ResourceRHI;
    }

protected:
    FRDGResource(const char* InName) : Name(InName) {}

    RHIResource* GetRHIUnchecked() const {
        return ResourceRHI;
    }

    bool HasRHI() const {
        return ResourceRHI != nullptr;
    }

    RHIResource* ResourceRHI = nullptr;

private:
    // friend FRDGBuilder;
    // friend FRDGUserValidation;
    // friend FRDGBarrierValidation;
};

class FRDGViewableResource : public FRDGResource {
public:
    /** The type of this resource; useful for casting between types. */
    const ERDGViewableResourceType Type;

    /** Whether this resource is externally registered with the graph (i.e. the user holds a reference to the underlying resource outside the graph). */
    bool IsExternal() const {
        return bExternal;
    }

    /** Whether this resource is has been queued for extraction at the end of graph execution. */
    bool IsExtracted() const {
        return bExtracted;
    }

    /** Whether a prior pass added to the graph produced contents for this resource. External resources are not considered produced
	 *  until used for a write operation. This is a union of all subresources, so any subresource write will set this to true.
	 */
    bool HasBeenProduced() const {
        return bProduced;
    }

protected:
    RENDER_API FRDGViewableResource(const char* InName, ERDGViewableResourceType InType);

    bool IsCullRoot() const {
        return bExternal || bExtracted;
    }

    static constexpr ERHIAccess DefaultEpilogueAccess = ERHIAccess::SRVMask;

    enum class EAccessMode : uint8 {
        Internal,
        External
    };

    /** Whether this is an externally registered resource. */
    uint8 bExternal : 1;

    /** Whether this is an extracted resource. */
    uint8 bExtracted : 1;

    /** Whether any sub-resource has been used for write by a pass. */
    uint8 bProduced : 1;

    struct FAccessModeState {
        bool IsExternalAccess() const {
            return ActiveMode == EAccessMode::External;
        }

        ERHIAccess   Access    = ERHIAccess::Unknown;
        ERHIPipeline Pipelines = ERHIPipeline::None;
        EAccessMode  Mode      = EAccessMode::Internal;
        bool         bLocked   = false;
        bool         bQueued   = false;

        /** The actual access mode replayed on the setup pass timeline. */
        EAccessMode ActiveMode = EAccessMode::Internal;
    } AccessModeState;

    EAccessMode AccessMode = EAccessMode::Internal;

    FRDGPassHandle            AcquirePass;
    FRDGPassHandle            DiscardPass;
    FRDGPassHandle            FirstPass;
    FRDGPassHandlesByPipeline LastPasses;

    /** Number of references in passes and deferred queries. */
    uint32 ReferenceCount;

    /** Scratch index allocated for the resource in the pass being setup. */
    uint32 PassStateIndex = 0;

    /** The state of the resource at the graph epilogue. */
    ERHIAccess EpilogueAccess = DefaultEpilogueAccess;

private:
    static constexpr uint32 DeallocatedReferenceCount = ~0;

    void SetRHI(RHIResource* Resource) {
        assert(!ResourceRHI);
        ResourceRHI = Resource;
    }

    void SetExternalAccessMode(ERHIAccess InAccess, ERHIPipeline InPipelines) {
        assert(!AccessModeState.bLocked);

        AccessModeState.Mode      = EAccessMode::External;
        AccessModeState.Access    = InAccess;
        AccessModeState.Pipelines = InPipelines;

        EpilogueAccess = InAccess;
    }

    friend bool IsExtendedLifetimeResource(FRDGViewableResource*);

    friend FRDGBuilder;
    friend FRDGUserValidation;
    friend FRDGBarrierBatchBegin;
    friend FRDGResourceDumpContext;
    friend FRDGTrace;
    friend FRDGPass;
};

using FRDGTextureSubresourceState =
    TRDGTextureSubresourceArray<FRDGSubresourceState*, FRDGArrayAllocator<FRDGSubresourceState*>>;

/** Render graph tracked Texture. */
class FRDGTexture final : public FRDGViewableResource {
public:
    static constexpr ERDGViewableResourceType StaticType = ERDGViewableResourceType::Texture;

    const FRDGTextureDesc  Desc;
    const ERDGTextureFlags Flags;

    //! The following methods may only be called during pass execution.

    /** Returns the allocated RHI texture. */
    inline Texture* GetRHI() const {
        return static_cast<Texture*>(FRDGResource::GetRHI());
    }

    inline FRDGTextureHandle GetHandle() const {
        return Handle;
    }

    inline FRDGTextureSubresourceLayout GetSubresourceLayout() const {
        return Layout;
    }

    inline FRDGTextureSubresourceRange GetSubresourceRange() const {
        return WholeRange;
    }

    inline uint32 GetSubresourceCount() const {
        return SubresourceCount;
    }

    inline FRDGTextureSubresource GetSubresource(uint32 SubresourceIndex) const {
        return Layout.GetSubresource(SubresourceIndex);
    }

    RENDER_API FRDGTextureSubresourceRange GetSubresourceRangeSRV() const {
        FRDGTextureSubresourceRange Range = GetSubresourceRange();

        // When binding a whole texture for shader read (SRV), we only use the first plane.
        // Other planes like stencil require a separate view to access for read in the shader.
        Range.PlaneSlice     = FRHITransitionInfo::kDepthPlaneSlice;
        Range.NumPlaneSlices = 1;

        return Range;
    }

    bool IsCulled() const {
        return ReferenceCount == 0;
    }

private:
    RENDER_API FRDGTexture(const char* InName, const FRDGTextureDesc& InDesc, ERDGTextureFlags InFlags);

    /** Returns RHI texture without access checks. */
    Texture* GetRHIUnchecked() const {
        return static_cast<Texture*>(FRDGResource::GetRHIUnchecked());
    }

    /** The handle registered with the builder. */
    FRDGTextureHandle Handle;

    /** The previous / next texture to own the PooledTexture allocation during execution. */
    FRDGTextureHandle PreviousOwner;
    FRDGTextureHandle NextOwner;

    /** The layout used to facilitate subresource transitions. */
    FRDGTextureSubresourceLayout Layout;
    FRDGTextureSubresourceRange  WholeRange;
    const uint16                 SubresourceCount;

    /** Tracks subresource states as the graph is built. */
    FRDGTextureSubresourceState State;

    /** Tracks the first state in the graph for each subresource. */
    FRDGTextureSubresourceState FirstState;

    /** Tracks merged subresource states as the graph is built. */
    FRDGTextureSubresourceState MergeState;

    /** Tracks pass producers for each subresource as the graph is built. */
    TRDGTextureSubresourceArray<FRDGProducerStatesByPipeline, FRDGArrayAllocator> LastProducers;

    /** The assigned render target to use during execution. Never reset. */
    IPooledRenderTarget* RenderTarget = nullptr;

    //TODO：Currently abandoned
    /** The assigned view cache for this texture (sourced from transient / pooled texture). Never reset. */
    //FRHITextureViewCache* ViewCache = nullptr;

    /** Valid strictly when holding a strong reference; use PooledRenderTarget instead. */
    CountableRef<IPooledRenderTarget> Allocation;

private:
    friend FRDGBuilder;
    friend FRDGUserValidation;
    friend FRDGBarrierValidation;
    friend FRDGTextureRegistry;
    friend FRDGAllocator;
    friend FPooledRenderTarget;
    friend FRDGTrace;
    friend FRDGTextureUAV;
};

//brand new desc, modified from UE
class FRDGTextureSRVDesc final {
public:
    FRDGTextureRef Texture = nullptr;
    EPixelFormat   Format  = EPixelFormat::PF_UNDEFINED;

    uint8  MipLevel        = 0;
    uint8  NumMipLevels    = 1;
    uint16 FirstArraySlice = 0;
    uint16 NumArraySlices  = 0;

    ERDGTextureMetaDataAccess        MetaData = ERDGTextureMetaDataAccess::None;
    std::optional<ETextureDimension> DimensionOverride;

    FRDGTextureSRVDesc() = default;

    explicit FRDGTextureSRVDesc(FRDGTextureRef InTexture) : Texture(InTexture) {
        if (InTexture) {
            NumMipLevels = InTexture->Desc.num_mips;
            if (InTexture->Desc.IsTextureArray()) {
                NumArraySlices = InTexture->Desc.array_size;
            }
        }
    }

    FRDGTextureSubresourceRange GetRange() const {
        assert(Texture != nullptr);
        FRDGTextureSubresourceRange Range;

        Range.MipIndex = MipLevel;
        if (NumMipLevels == 0) {
            Range.NumMips = Texture->Desc.num_mips - MipLevel;
        } else {
            Range.NumMips = NumMipLevels;
        }

        Range.ArraySlice = FirstArraySlice;
        if (NumArraySlices == 0) {
            if (Texture->Desc.IsArray() || Texture->Desc.IsTextureCube()) {
                Range.NumArraySlices = Texture->Desc.array_size - FirstArraySlice;
            } else {
                Range.NumArraySlices = 1;
            }
        } else {
            Range.NumArraySlices = NumArraySlices;
        }

        if (MetaData == ERDGTextureMetaDataAccess::Stencil) {
            Range.PlaneSlice = 1; // 1 是 Stencil 平面的索引
        } else {
            Range.PlaneSlice = 0; // 默认是 Color 或 Depth 平面
        }
        Range.NumPlaneSlices = 1;

        assert(Range.MipIndex + Range.NumMips <= Texture->Desc.num_mips);
        assert(Range.ArraySlice + Range.NumArraySlices <= Texture->Desc.array_size);

        return Range;
    }

    static FRDGTextureSRVDesc Create(FRDGTextureRef InTexture) {
        return FRDGTextureSRVDesc(InTexture);
    }

    static FRDGTextureSRVDesc CreateForMipLevel(FRDGTextureRef InTexture, int32 MipLevel) {
        assert(InTexture);
        FRDGTextureSRVDesc Desc(InTexture);
        Desc.MipLevel     = static_cast<uint8>(MipLevel);
        Desc.NumMipLevels = 1;
        return Desc;
    }

    static FRDGTextureSRVDesc CreateForSlice(FRDGTextureRef InTexture, int32 SliceIndex) {
        assert(InTexture);
        FRDGTextureSRVDesc Desc(InTexture);
        Desc.FirstArraySlice   = static_cast<uint16>(SliceIndex);
        Desc.NumArraySlices    = 1;
        Desc.DimensionOverride = ETextureDimension::TEX_2D;
        return Desc;
    }

    static FRDGTextureSRVDesc CreateWithPixelFormat(FRDGTextureRef InTexture, EPixelFormat PixelFormat) {
        FRDGTextureSRVDesc Desc(InTexture);
        Desc.Format = PixelFormat;
        return Desc;
    }

    static FRDGTextureSRVDesc
    CreateForMetaData(FRDGTextureRef InTexture, ERDGTextureMetaDataAccess MetaData) {
        FRDGTextureSRVDesc Desc(InTexture);
        Desc.MetaData = MetaData;
        return Desc;
    }

    bool operator==(const FRDGTextureSRVDesc& Other) const {
        return Texture == Other.Texture && Format == Other.Format && MipLevel == Other.MipLevel &&
               NumMipLevels == Other.NumMipLevels && FirstArraySlice == Other.FirstArraySlice &&
               NumArraySlices == Other.NumArraySlices && MetaData == Other.MetaData &&
               DimensionOverride == Other.DimensionOverride;
    }

    bool operator!=(const FRDGTextureSRVDesc& Other) const {
        return !(*this == Other);
    }

    friend uint32 GetHash(const FRDGTextureSRVDesc& Desc) {
        uint32 Hash = GetHash(reinterpret_cast<uint64>(Desc.Texture));
        HashCombine(Hash, static_cast<uint32>(Desc.Format));
        HashCombine(Hash, static_cast<uint32>(Desc.MipLevel));
        HashCombine(Hash, static_cast<uint32>(Desc.NumMipLevels));
        HashCombine(Hash, static_cast<uint32>(Desc.FirstArraySlice));
        HashCombine(Hash, static_cast<uint32>(Desc.NumArraySlices));
        HashCombine(Hash, static_cast<uint32>(Desc.MetaData));
        if (Desc.DimensionOverride.has_value()) {
            HashCombine(Hash, static_cast<uint32>(Desc.DimensionOverride.value()));
        }
        return Hash;
    }

    bool IsValid() const {
        if (!Texture) {
            return false;
        }
        if (MipLevel + NumMipLevels > Texture->Desc.num_mips) {
            return false;
        }
        return true;
    }
};

/** Descriptor for render graph tracked UAV. */
class FRDGTextureUAVDesc final {
public:
    FRDGTextureRef Texture = nullptr;

    uint8        MipLevel        = 0;
    EPixelFormat Format          = PF_UNDEFINED;
    uint16       FirstArraySlice = 0;
    uint16       NumArraySlices  = 0;

    ERDGTextureMetaDataAccess MetaData = ERDGTextureMetaDataAccess::None;

    FRDGTextureUAVDesc() = default;

    FRDGTextureUAVDesc(
        FRDGTextureRef InTexture,
        uint8          InMipLevel        = 0,
        EPixelFormat   InFormat          = PF_UNDEFINED,
        uint16         InFirstArraySlice = 0,
        uint16         InNumArraySlices  = 0
    ) :
        Texture(InTexture),
        MipLevel(InMipLevel),
        Format(InFormat),
        FirstArraySlice(InFirstArraySlice),
        NumArraySlices(InNumArraySlices) {
        if (Texture) {
            if (Format == PF_UNDEFINED) {
                Format = (Texture->Desc.uav_format != PF_UNDEFINED) ? Texture->Desc.uav_format :
                                                                      Texture->Desc.format;
            }
            if (NumArraySlices == 0 && Texture->Desc.IsTextureArray()) {
                NumArraySlices = Texture->Desc.array_size;
            }
        }
    }

    FRDGTextureSubresourceRange GetRange() const {
        assert(Texture != nullptr);
        FRDGTextureSubresourceRange Range;

        Range.MipIndex = MipLevel;
        Range.NumMips  = 1; // 绝大多数情况下，UAV 只能访问 1 个 Mip 层

        Range.ArraySlice = FirstArraySlice;
        if (NumArraySlices == 0) {
            Range.NumArraySlices = Texture->Desc.IsArray() ? (Texture->Desc.array_size - FirstArraySlice) : 1;
        } else {
            Range.NumArraySlices = NumArraySlices;
        }

        Range.PlaneSlice     = 0; // UAV 通常不支持 Stencil 平面
        Range.NumPlaneSlices = 1;

        return Range;
    }

    static FRDGTextureUAVDesc
    CreateForMetaData(FRDGTextureRef InTexture, ERDGTextureMetaDataAccess InMetaData) {
        FRDGTextureUAVDesc Desc(InTexture, 0);
        Desc.MetaData = InMetaData;
        return Desc;
    }

    bool operator==(const FRDGTextureUAVDesc& Other) const {
        return Texture == Other.Texture && MipLevel == Other.MipLevel && Format == Other.Format &&
               FirstArraySlice == Other.FirstArraySlice && NumArraySlices == Other.NumArraySlices &&
               MetaData == Other.MetaData;
    }

    bool operator!=(const FRDGTextureUAVDesc& Other) const {
        return !(*this == Other);
    }

    friend uint32 GetHash(const FRDGTextureUAVDesc& Desc) {
        uint32 Hash = GetHash(reinterpret_cast<uint64>(Desc.Texture));
        HashCombine(Hash, static_cast<uint32>(Desc.MipLevel));
        HashCombine(Hash, static_cast<uint32>(Desc.Format));
        HashCombine(Hash, static_cast<uint32>(Desc.FirstArraySlice));
        HashCombine(Hash, static_cast<uint32>(Desc.NumArraySlices));
        HashCombine(Hash, static_cast<uint32>(Desc.MetaData));
        return Hash;
    }
};

/** A pooled buffer managed by the render graph buffer pool. Wraps an RHI Buffer with metadata
 *  needed for pool management (descriptor matching, frame tracking, reserved resource commits).
 *  Refcounting is handled via Countable / CountableRef<FRDGPooledBuffer>.
 */
class FRDGPooledBuffer final {
public:
    COUNTABLE_IMPLEMENTATION_AUTO_DESTROY

    FRDGPooledBuffer(
        CommandList&          InRHICmdList,
        BufferRef             InBuffer,
        const FRDGBufferDesc& InDesc,
        uint32                InNumAllocatedElements,
        const char*           InName
    ) :
        Desc(InDesc),
        RHIBuffer(std::move(InBuffer)),
        Name(InName),
        NumAllocatedElements(InNumAllocatedElements) {
        (void)InRHICmdList;
    }

    FRDGPooledBuffer(
        BufferRef             InBuffer,
        const FRDGBufferDesc& InDesc,
        uint32                InNumAllocatedElements,
        const char*           InName
    ) :
        Desc(InDesc),
        RHIBuffer(std::move(InBuffer)),
        Name(InName),
        NumAllocatedElements(InNumAllocatedElements) {}

    const FRDGBufferDesc Desc;

    /** Returns the underlying RHI buffer. */
    Buffer* GetRHI() const {
        return RHIBuffer.Get();
    }

    uint32 GetSize() const {
        return Desc.GetSize();
    }

    uint32 GetAlignedSize() const {
        return Desc.BytesPerElement * NumAllocatedElements;
    }

    uint64 GetCommittedSize() const {
        return std::min<uint64>(CommittedSizeInBytes, GetSize());
    }

    const char* GetName() const {
        return Name;
    }

private:
    BufferRef RHIBuffer;

    FRDGBufferDesc GetAlignedDesc() const {
        FRDGBufferDesc AlignedDesc = Desc;
        AlignedDesc.NumElements    = NumAllocatedElements;
        return AlignedDesc;
    }

    void SetDebugLabelName(CommandList& InRHICmdList, const char* InName);

    /** Used internally by FRDGBuilder::QueueCommitReservedBuffer() to resize physical memory. */
    void SetCommittedSize(uint64 InCommittedSizeInBytes) {
        if (InCommittedSizeInBytes == UINT64_MAX) {
            InCommittedSizeInBytes = GetSize();
        }
        assert(InCommittedSizeInBytes <= GetSize() && "Attempting to commit more memory than reserved");
        CommittedSizeInBytes = InCommittedSizeInBytes;
    }

    const char* Name = nullptr;

    /** Size of the GPU physical memory committed to a reserved buffer.
     *  May be UINT64_MAX for regular (non-reserved) buffers or when the entire resource is committed. */
    uint64 CommittedSizeInBytes = UINT64_MAX;

    const uint32 NumAllocatedElements;
    uint32       LastUsedFrame = 0;

    friend FRDGBuilder;
    friend class FRDGBufferPool;
};

/** A pooled texture resource managed by the render graph texture pool.
 *  Mirrors FRDGPooledBuffer but for textures. MoerEngine uses a viewless RHI,
 *  so no SRV/UAV view cache is needed here — descriptors bind directly. */
class FRDGPooledTexture final {
public:
    COUNTABLE_IMPLEMENTATION_AUTO_DESTROY

    FRDGPooledTexture(TextureRef InTexture, const FRDGTextureDesc& InDesc, const char* InName) :
        Desc(InDesc),
        RHITexture(std::move(InTexture)),
        Name(InName) {}

    /** Lightweight constructor — Desc is default-initialized. */
    explicit FRDGPooledTexture(TextureRef InTexture) : Desc{}, RHITexture(std::move(InTexture)) {}

    const FRDGTextureDesc Desc;

    /** Returns the underlying RHI texture. */
    Texture* GetRHI() const {
        return RHITexture.Get();
    }

    const char* GetName() const {
        return Name;
    }

private:
    TextureRef RHITexture;

    const char* Name          = nullptr;
    uint32      LastUsedFrame = 0;

    friend FRDGBuilder;
};

/** A render graph tracked buffer. */
class FRDGBuffer final : public FRDGViewableResource {
public:
    static const ERDGViewableResourceType StaticType = ERDGViewableResourceType::Buffer;

    FRDGBufferDesc        Desc;
    const ERDGBufferFlags Flags;

    //////////////////////////////////////////////////////////////////////////
    //! The following methods may only be called during pass execution.

    /** Returns the underlying RHI buffer resource */
    Buffer* GetRHI() const {
        return static_cast<Buffer*>(FRDGViewableResource::GetRHI());
    }

    /** Returns the buffer to use for indirect RHI calls. */
    inline Buffer* GetIndirectRHICallBuffer() const {
        assert(
            EnumHasAnyFlag(Desc.Usage, EBufferUsageFlags::INDIRECT_BUFFER) &&
            "Buffer was not flagged for indirect draw usage."
        );
        return GetRHI();
    }

    //////////////////////////////////////////////////////////////////////////

    FRDGBufferHandle GetHandle() const {
        return Handle;
    }

    inline uint32 GetSize() const {
        return Desc.GetSize();
    }

    inline uint32 GetStride() const {
        return Desc.BytesPerElement;
    }

    bool IsCulled() const {
        return ReferenceCount == 0 && PendingCommitSize == 0;
    }

private:
    RENDER_API FRDGBuffer(const char* InName, const FRDGBufferDesc& InDesc, ERDGBufferFlags InFlags);
    RENDER_API FRDGBuffer(
        const char*                    InName,
        const FRDGBufferDesc&          InDesc,
        ERDGBufferFlags                InFlags,
        FRDGBufferNumElementsCallback* InNumElementsCallback
    );

    /** Finalizes any pending field of the buffer descriptor. */
    RENDER_API void FinalizeDesc();

    Buffer* GetRHIUnchecked() const {
        return static_cast<Buffer*>(FRDGResource::GetRHIUnchecked());
    }

    /** Registered handle set by the builder. */
    FRDGBufferHandle Handle;

    /** The previous / next buffer to own the PooledBuffer allocation during execution. */
    FRDGBufferHandle PreviousOwner;
    FRDGBufferHandle NextOwner;

    /** Assigned pooled buffer pointer. Never reset once assigned. */
    FRDGPooledBuffer* PooledBuffer = nullptr;

    /** Assigned transient buffer pointer. Never reset once assigned. */
    // Transient buffers and view cache are not supported yet.
    // FRHITransientBuffer* TransientBuffer = nullptr;
    // FRHIBufferViewCache* ViewCache       = nullptr;

    /** Valid strictly when holding a strong reference; use PooledBuffer instead. */
    CountableRef<FRDGPooledBuffer> Allocation;

    /** Tracks the last pass that produced this resource as the graph is built. */
    FRDGProducerStatesByPipeline LastProducer;

    /** Optional callback to supply NumElements after the creation of this FRDGBuffer. */
    FRDGBufferNumElementsCallback* NumElementsCallback = nullptr;

    /** Optional reserved resource commit size to apply on the first resource transition. */
    uint64 PendingCommitSize = 0;

    /** Cached state pointer from the pooled / transient buffer. */
    FRDGSubresourceState* State = nullptr;

    /** Tracks the first state in the graph for this buffer. */
    FRDGSubresourceState* FirstState = nullptr;

    /** Tracks the merged subresource state as the graph is built. */
    FRDGSubresourceState* MergeState = nullptr;

    friend FRDGBuilder;
    friend FRDGBarrierValidation;
    friend FRDGUserValidation;
    friend FRDGBufferRegistry;
    friend FRDGAllocator;
    friend FRDGTrace;
};

/** Descriptor for render graph tracked Buffer. */
struct FRDGBufferDesc {
    static FRDGBufferDesc CreateByteAddressDesc(uint32 NumBytes) {
        assert(NumBytes % 4 == 0);
        FRDGBufferDesc Desc;
        Desc.Usage           = EBufferUsageFlags::UNORDERED_ACCESS;
        Desc.BytesPerElement = 4;
        Desc.NumElements     = NumBytes / 4;
        return Desc;
    }

    static FRDGBufferDesc CreateIndirectDesc(uint32 BytesPerElement, uint32 NumElements) {
        FRDGBufferDesc Desc;
        Desc.Usage           = EBufferUsageFlags::INDIRECT_BUFFER | EBufferUsageFlags::UNORDERED_ACCESS;
        Desc.BytesPerElement = BytesPerElement;
        Desc.NumElements     = NumElements;
        return Desc;
    }

    static FRDGBufferDesc CreateRawIndirectDesc(uint32 NumBytes) {
        FRDGBufferDesc Desc = CreateByteAddressDesc(NumBytes);
        Desc.Usage |= EBufferUsageFlags::INDIRECT_BUFFER;
        return Desc;
    }

    /** Create the descriptor for an indirect RHI call.
     *
     * Note, IndirectParameterStruct should be one of the:
     *     struct FRHIDispatchIndirectParameters
     *     struct FRHIDrawIndirectParameters
     *     struct FRHIDrawIndexedIndirectParameters
     */
    template<typename IndirectParameterStruct>
    static FRDGBufferDesc CreateIndirectDesc(uint32 NumElements = 1) {
        return CreateIndirectDesc(sizeof(IndirectParameterStruct), NumElements);
    }

    static FRDGBufferDesc CreateIndirectDesc(uint32 NumElements = 1) {
        return CreateIndirectDesc(4u, NumElements);
    }

    static FRDGBufferDesc CreateStructuredDesc(uint32 BytesPerElement, uint32 NumElements) {
        FRDGBufferDesc Desc;
        Desc.Usage           = EBufferUsageFlags::UNORDERED_ACCESS;
        Desc.BytesPerElement = BytesPerElement;
        Desc.NumElements     = NumElements;
        return Desc;
    }

    static FRDGBufferDesc CreateBufferDesc(uint32 BytesPerElement, uint32 NumElements) {
        FRDGBufferDesc Desc;
        Desc.Usage           = EBufferUsageFlags::TEXTURE_BUFFER | EBufferUsageFlags::UNORDERED_ACCESS;
        Desc.BytesPerElement = BytesPerElement;
        Desc.NumElements     = NumElements;
        return Desc;
    }

    static FRDGBufferDesc CreateUploadDesc(uint32 BytesPerElement, uint32 NumElements) {
        FRDGBufferDesc Desc;
        Desc.Usage           = EBufferUsageFlags::CPU_VISIBLE | EBufferUsageFlags::TEXTURE_BUFFER;
        Desc.BytesPerElement = BytesPerElement;
        Desc.NumElements     = NumElements;
        return Desc;
    }

    static FRDGBufferDesc CreateStructuredUploadDesc(uint32 BytesPerElement, uint32 NumElements) {
        FRDGBufferDesc Desc;
        Desc.Usage           = EBufferUsageFlags::CPU_VISIBLE | EBufferUsageFlags::UNORDERED_ACCESS;
        Desc.BytesPerElement = BytesPerElement;
        Desc.NumElements     = NumElements;
        return Desc;
    }

    static FRDGBufferDesc CreateByteAddressUploadDesc(uint32 NumBytes) {
        assert(NumBytes % 4 == 0);
        FRDGBufferDesc Desc;
        Desc.Usage           = EBufferUsageFlags::CPU_VISIBLE | EBufferUsageFlags::UNORDERED_ACCESS;
        Desc.BytesPerElement = 4;
        Desc.NumElements     = NumBytes / 4;
        return Desc;
    }

    /** Returns the total number of bytes allocated for a such buffer. */
    uint32 GetSize() const {
        return BytesPerElement * NumElements;
    }

    friend uint32 GetHash(const FRDGBufferDesc& Desc) {
        uint32 Hash = GetHash(Desc.BytesPerElement);
        HashCombine(Hash, GetHash(Desc.NumElements));
        HashCombine(Hash, GetHash(Desc.Usage));
        return Hash;
    }

    bool operator==(const FRDGBufferDesc& Other) const {
        return BytesPerElement == Other.BytesPerElement && NumElements == Other.NumElements &&
               Usage == Other.Usage;
    }

    bool operator!=(const FRDGBufferDesc& Other) const {
        return !(*this == Other);
    }

    /** Stride in bytes for index and structured buffers. */
    uint32 BytesPerElement = 1;

    /** Number of elements. */
    uint32 NumElements = 1;

    /** Bitfields describing the uses of that buffer. */
    EBufferUsageFlags Usage = EBufferUsageFlags::NONE;
};

struct FRDGBufferSRVDesc final {
    FRDGBufferRef Buffer = nullptr;

    EPixelFormat Format           = EPixelFormat::PF_UNDEFINED;
    uint32       StartOffsetBytes = 0;
    uint32       NumElements      = 0;

    RaytracingScene* RayTracingScene = nullptr;

    FRDGBufferSRVDesc() = default;

    explicit FRDGBufferSRVDesc(FRDGBufferRef InBuffer) : Buffer(InBuffer) {}

    FRDGBufferSRVDesc(FRDGBufferRef InBuffer, EPixelFormat InFormat) : Buffer(InBuffer), Format(InFormat) {}

    FRDGBufferSRVDesc(FRDGBufferRef InBuffer, uint32 InStartOffsetBytes, uint32 InNumElements) :
        Buffer(InBuffer),
        StartOffsetBytes(InStartOffsetBytes),
        NumElements(InNumElements) {}

    FRDGBufferSRVDesc(FRDGBufferRef InBuffer, RaytracingScene* InRayTracingScene, uint32 InStartOffsetBytes) :
        Buffer(InBuffer),
        StartOffsetBytes(InStartOffsetBytes),
        RayTracingScene(InRayTracingScene) {}

    bool operator==(const FRDGBufferSRVDesc& Other) const {
        return Buffer == Other.Buffer && Format == Other.Format &&
               StartOffsetBytes == Other.StartOffsetBytes && NumElements == Other.NumElements &&
               RayTracingScene == Other.RayTracingScene;
    }

    bool operator!=(const FRDGBufferSRVDesc& Other) const {
        return !(*this == Other);
    }

    friend uint32 GetHash(const FRDGBufferSRVDesc& Desc) {
        uint32 Hash = GetHash(reinterpret_cast<uint64>(Desc.Buffer));
        HashCombine(Hash, static_cast<uint32>(Desc.Format));
        HashCombine(Hash, GetHash(Desc.StartOffsetBytes));
        HashCombine(Hash, GetHash(Desc.NumElements));
        HashCombine(Hash, GetHash(reinterpret_cast<uint64>(Desc.RayTracingScene)));
        return Hash;
    }
};

struct FRDGBufferUAVDesc final {
    FRDGBufferRef Buffer = nullptr;

    EPixelFormat Format           = EPixelFormat::PF_UNDEFINED;
    uint32       StartOffsetBytes = 0;
    uint32       NumElements      = 0;

    FRDGBufferUAVDesc() = default;

    explicit FRDGBufferUAVDesc(FRDGBufferRef InBuffer) : Buffer(InBuffer) {}

    FRDGBufferUAVDesc(FRDGBufferRef InBuffer, EPixelFormat InFormat) : Buffer(InBuffer), Format(InFormat) {}

    FRDGBufferUAVDesc(FRDGBufferRef InBuffer, uint32 InStartOffsetBytes, uint32 InNumElements) :
        Buffer(InBuffer),
        StartOffsetBytes(InStartOffsetBytes),
        NumElements(InNumElements) {}

    bool operator==(const FRDGBufferUAVDesc& Other) const {
        return Buffer == Other.Buffer && Format == Other.Format &&
               StartOffsetBytes == Other.StartOffsetBytes && NumElements == Other.NumElements;
    }

    bool operator!=(const FRDGBufferUAVDesc& Other) const {
        return !(*this == Other);
    }

    friend uint32 GetHash(const FRDGBufferUAVDesc& Desc) {
        uint32 Hash = GetHash(reinterpret_cast<uint64>(Desc.Buffer));
        HashCombine(Hash, static_cast<uint32>(Desc.Format));
        HashCombine(Hash, GetHash(Desc.StartOffsetBytes));
        HashCombine(Hash, GetHash(Desc.NumElements));
        return Hash;
    }
};

//=============================================================================
// FRDGView - Lightweight RDG-layer view for dependency tracking.
// These are NOT RHI views; they exist purely so the render graph can track
// which passes read/write which sub-resources before any GPU object exists.
//=============================================================================

class FRDGView {
public:
    const char* const  Name;
    const ERDGViewType Type;
    FRDGViewHandle     Handle;

    /** The viewable resource this view references. */
    FRDGViewableResource* GetResource() const {
        return Resource;
    }

protected:
    FRDGView(const char* InName, ERDGViewType InType, FRDGViewableResource* InResource) :
        Name(InName),
        Type(InType),
        Resource(InResource) {}

private:
    FRDGViewableResource* Resource = nullptr;

    friend FRDGBuilder;
    friend FRDGViewRegistry;
};

/** RDG tracked Texture SRV. */
class FRDGTextureSRV final : public FRDGView {
public:
    static constexpr ERDGViewType StaticType = ERDGViewType::TextureSRV;

    const FRDGTextureSRVDesc Desc;

    FRDGTexture* GetParent() const {
        return Desc.Texture;
    }

private:
    FRDGTextureSRV(const char* InName, const FRDGTextureSRVDesc& InDesc) :
        FRDGView(InName, StaticType, InDesc.Texture),
        Desc(InDesc) {}

    friend FRDGViewRegistry;
    friend FRDGBuilder;
};

/** RDG tracked Texture UAV. */
class FRDGTextureUAV final : public FRDGView {
public:
    static constexpr ERDGViewType StaticType = ERDGViewType::TextureUAV;

    const FRDGTextureUAVDesc           Desc;
    const ERDGUnorderedAccessViewFlags Flags;

    FRDGTexture* GetParent() const {
        return Desc.Texture;
    }

private:
    FRDGTextureUAV(
        const char*                  InName,
        const FRDGTextureUAVDesc&    InDesc,
        ERDGUnorderedAccessViewFlags InFlags = ERDGUnorderedAccessViewFlags::None
    ) :
        FRDGView(InName, StaticType, InDesc.Texture),
        Desc(InDesc),
        Flags(InFlags) {}

    friend FRDGViewRegistry;
    friend FRDGBuilder;
};

/** RDG tracked Buffer SRV. */
class FRDGBufferSRV final : public FRDGView {
public:
    static constexpr ERDGViewType StaticType = ERDGViewType::BufferSRV;

    const FRDGBufferSRVDesc Desc;

    FRDGBuffer* GetParent() const {
        return Desc.Buffer;
    }

private:
    FRDGBufferSRV(const char* InName, const FRDGBufferSRVDesc& InDesc) :
        FRDGView(InName, StaticType, InDesc.Buffer),
        Desc(InDesc) {}

    friend FRDGViewRegistry;
    friend FRDGBuilder;
};

/** RDG tracked Buffer UAV. */
class FRDGBufferUAV final : public FRDGView {
public:
    static constexpr ERDGViewType StaticType = ERDGViewType::BufferUAV;

    const FRDGBufferUAVDesc            Desc;
    const ERDGUnorderedAccessViewFlags Flags;

    FRDGBuffer* GetParent() const {
        return Desc.Buffer;
    }

private:
    FRDGBufferUAV(
        const char*                  InName,
        const FRDGBufferUAVDesc&     InDesc,
        ERDGUnorderedAccessViewFlags InFlags = ERDGUnorderedAccessViewFlags::None
    ) :
        FRDGView(InName, StaticType, InDesc.Buffer),
        Desc(InDesc),
        Flags(InFlags) {}

    friend FRDGViewRegistry;
    friend FRDGBuilder;
};

//=============================================================================
// GetAs<> helper templates
//=============================================================================

template<typename ViewableResourceType>
inline ViewableResourceType* GetAs(FRDGViewableResource* Resource) {
    assert(ViewableResourceType::StaticType == Resource->Type);
    return static_cast<ViewableResourceType*>(Resource);
}

template<typename ViewType>
inline ViewType* GetAs(FRDGView* View) {
    assert(ViewType::StaticType == View->Type);
    return static_cast<ViewType*>(View);
}

inline FRDGBuffer* GetAsBuffer(FRDGViewableResource* Resource) {
    return GetAs<FRDGBuffer>(Resource);
}

inline FRDGTexture* GetAsTexture(FRDGViewableResource* Resource) {
    return GetAs<FRDGTexture>(Resource);
}

inline FRDGBufferUAV* GetAsBufferUAV(FRDGView* View) {
    return GetAs<FRDGBufferUAV>(View);
}

inline FRDGBufferSRV* GetAsBufferSRV(FRDGView* View) {
    return GetAs<FRDGBufferSRV>(View);
}

inline FRDGTextureUAV* GetAsTextureUAV(FRDGView* View) {
    return GetAs<FRDGTextureUAV>(View);
}

inline FRDGTextureSRV* GetAsTextureSRV(FRDGView* View) {
    return GetAs<FRDGTextureSRV>(View);
}

inline FGraphicsPipelineRenderTargetsInfo
ExtractRenderTargetsInfo(const FRDGParameterStruct& ParameterStruct);
inline FGraphicsPipelineRenderTargetsInfo
ExtractRenderTargetsInfo(const FRenderTargetBindingSlots& RenderTargets);

#include "RenderGraphResources.inl"

} // namespace Moer::Render::RenderGraph
