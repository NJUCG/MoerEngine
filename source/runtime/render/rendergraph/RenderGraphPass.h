#pragma once

#include "RenderGraphFwd.h"
#include "RenderGraphResource.h"

namespace Moer::Render::RenderGraph {

enum class ERDGPassTaskMode : uint8 {
    /** Execute must be called inline on the render thread. */
    Inline,

    /** Execute may be called in a task that is awaited at the end of FRDGBuilder::Execute. */
    Await,

    /** Execute may be called in a task that must be manually awaited. */
    Async
};

struct FRDGBarrierBatchBeginId {
    FRDGBarrierBatchBeginId() = default;

    bool operator==(FRDGBarrierBatchBeginId Other) const {
        return Passes == Other.Passes && PipelinesAfter == Other.PipelinesAfter;
    }

    bool operator!=(FRDGBarrierBatchBeginId Other) const {
        return !(*this == Other);
    }

    friend uint32 GetTypeHash(FRDGBarrierBatchBeginId Id) {
        static_assert(sizeof(Id.Passes) <= 8);
        uint32 Hash = GetTypeHash(*(const uint64*)Id.Passes.GetData());
        return HashCombineFast(Hash, (uint32)Id.PipelinesAfter);
    }

    FRDGPassHandlesByPipeline Passes;
    ERHIPipeline              PipelinesAfter = ERHIPipeline::None;
};

struct FRDGTransitionInfo {
    static_assert(
        (int32)ERHIAccess::Last <= (1 << 20) && (int32)ERDGViewableResourceType::MAX <= 3 &&
            (int32)EResourceTransitionFlags::Last <= (1 << 2),
        "FRDGTransitionInfo packing is no longer correct."
    );

    uint64 AccessBefore : 21;           // 21
    uint64 AccessAfter : 21;            // 42
    uint64 ResourceHandle : 16;         // 58
    uint64 ResourceType : 3;            // 61
    uint64 ResourceTransitionFlags : 3; // 64

    union {
        struct {
            uint16 ArraySlice;
            uint8  MipIndex;
            uint8  PlaneSlice;

        } Texture;

        struct {
            uint64 CommitSize;

        } Buffer;
    };
};

struct FRDGBarrierBatchEndId {
    FRDGBarrierBatchEndId() = default;
    FRDGBarrierBatchEndId(FRDGPassHandle InPassHandle, ERDGBarrierLocation InBarrierLocation) :
        PassHandle(InPassHandle),
        BarrierLocation(InBarrierLocation) {}

    bool operator==(FRDGBarrierBatchEndId Other) const {
        return PassHandle == Other.PassHandle && BarrierLocation == Other.BarrierLocation;
    }

    bool operator!=(FRDGBarrierBatchEndId Other) const {
        return !(*this == Other);
    }

    FRDGPassHandle      PassHandle;
    ERDGBarrierLocation BarrierLocation = ERDGBarrierLocation::Epilogue;
};

class FRDGBarrierBatchBegin {
public:
    RENDER_API FRDGBarrierBatchBegin(
        ERHIPipeline PipelinesToBegin,
        ERHIPipeline PipelinesToEnd,
        const TCHAR* Name,
        FRDGPass*    Pass
    );
    RENDER_API FRDGBarrierBatchBegin(
        ERHIPipeline         PipelinesToBegin,
        ERHIPipeline         PipelinesToEnd,
        const TCHAR*         Name,
        FRDGPassesByPipeline Passes
    );

    RENDER_API void AddTransition(FRDGViewableResource* Resource, FRDGTransitionInfo Info);

    //void AddAlias(FRDGViewableResource* Resource, const FRHITransientAliasingInfo& Info);

    RENDER_API void SetUseCrossPipelineFence(bool bUseSeparateTransition) {
        if (bUseSeparateTransition) {
            bSeparateFenceTransitionNeeded = true;
        } else {
            EnumRemoveFlags(TransitionFlags, ERHITransitionCreateFlags::NoFence);
        }
        bTransitionNeeded = true;
    }

    // TODO: Single-queue minimal path
    // - Input: TransitionsRHI (array of FRHITransitionInfo)
    // - Output: translate to backend barriers and submit on RHICmdList in Submit()
    // - For split/cross-pipeline: enqueue Transition into FRDGTransitionQueue instead
    RENDER_API void CreateTransition(Array<const FRHITransitionInfo> TransitionsRHI);

    // TODO: Single-queue minimal path
    // - RHICmdList: target command list
    // - Pipeline: Graphics/AsyncCompute (used to choose stages if needed)
    // - Translate Transitions/Aliases into Vk/D3D12 barriers and emit
    RENDER_API void Submit(FRHIComputeCommandList& RHICmdList, ERHIPipeline Pipeline);
    // TODO: Split-barrier path
    // - TransitionsToBegin: queue of transitions to begin now and end later
    RENDER_API void Submit(
        FRHIComputeCommandList& RHICmdList,
        ERHIPipeline            Pipeline,
        FRDGTransitionQueue&    TransitionsToBegin
    );

    void Reserve(uint32 TransitionCount) {
        Transitions.Reserve(TransitionCount);
        Aliases.Reserve(TransitionCount);
    }

    bool IsTransitionNeeded() const {
        return bTransitionNeeded;
    }

private:
    TRHIPipelineArray<FRDGBarrierBatchEndId>             BarriersToEnd;
    Array<FRDGTransitionInfo, FRDGArrayAllocator>        Transitions;
    Array<FRHITransientAliasingInfo, FRDGArrayAllocator> Aliases;
    ERHITransitionCreateFlags                            TransitionFlags =
        ERHITransitionCreateFlags::NoFence | ERHITransitionCreateFlags::AllowDecayPipelines;
    ERHIPipeline PipelinesToBegin;
    ERHIPipeline PipelinesToEnd;
    bool         bTransitionNeeded              = false;
    bool         bSeparateFenceTransitionNeeded = false;

    friend class FRDGBarrierBatchEnd;
    friend class FRDGBarrierValidation;
    friend class FRDGBuilder;
};

using FRDGTransitionCreateQueue = Array<FRDGBarrierBatchBegin*, FRDGArrayAllocator>;

class FRDGBarrierBatchEnd {
public:
    FRDGBarrierBatchEnd(FRDGPass* InPass, ERDGBarrierLocation InBarrierLocation) :
        Pass(InPass),
        BarrierLocation(InBarrierLocation) {}

    /** Inserts a dependency on a begin batch. A begin batch can be inserted into more than one end batch. */
    RENDER_API void AddDependency(FRDGBarrierBatchBegin* BeginBatch);

    //TODO
    RENDER_API void Submit(FRHIComputeCommandList& RHICmdList, ERHIPipeline Pipeline);

    void Reserve(uint32 TransitionBatchCount) {
        Dependencies.Reserve(TransitionBatchCount);
    }

    RENDER_API FRDGBarrierBatchEndId GetId() const;

    RENDER_API bool IsPairedWith(const FRDGBarrierBatchBegin& BeginBatch) const;

private:
    Array<FRDGBarrierBatchBegin*, FRDGArrayAllocator> Dependencies;
    FRDGPass*                                         Pass;
    ERDGBarrierLocation                               BarrierLocation;

    friend class FRDGBarrierBatchBegin;
    friend class FRDGBarrierValidation;
};

/** Base class of a render graph pass. */
class FRDGPass {
public:
    RENDER_API FRDGPass(
        FRDGEventName&&     InName,
        FRDGParameterStruct InParameterStruct,
        ERDGPassFlags       InFlags,
        ERDGPassTaskMode    InTaskMode
    );
    FRDGPass(const FRDGPass&) = delete;
    virtual ~FRDGPass()       = default;

    const char* GetName() const {
        return Name.GetCStr();
    }

    const FRDGEventName& GetEventName() const {
        return Name;
    }

    ERDGPassFlags GetFlags() const {
        return Flags;
    }

    ERHIPipeline GetPipeline() const {
        return Pipeline;
    }

    FRDGParameterStruct GetParameters() const {
        return ParameterStruct;
    }

    FRDGPassHandle GetHandle() const {
        return Handle;
    }

    ERDGPassTaskMode GetTaskMode() const {
        return TaskMode;
    }

    bool IsParallelExecuteAllowed() const {
        return TaskMode != ERDGPassTaskMode::Inline;
    }

    bool SkipRenderPassBegin() const {
        return bSkipRenderPassBegin;
    }

    bool SkipRenderPassEnd() const {
        return bSkipRenderPassEnd;
    }

    bool IsAsyncCompute() const {
        return Pipeline == ERHIPipeline::AsyncCompute;
    }

    bool IsAsyncComputeBegin() const {
        return bAsyncComputeBegin;
    }

    bool IsAsyncComputeEnd() const {
        return bAsyncComputeEnd;
    }

    bool IsGraphicsFork() const {
        return bGraphicsFork;
    }

    bool IsGraphicsJoin() const {
        return bGraphicsJoin;
    }

    bool IsCulled() const {
        return bCulled;
    }

    bool IsSentinel() const {
        return bSentinel;
    }

    /** Returns the graphics pass responsible for forking the async interval this pass is in. */
    FRDGPassHandle GetGraphicsForkPass() const {
        return GraphicsForkPass;
    }

    /** Returns the graphics pass responsible for joining the async interval this pass is in. */
    FRDGPassHandle GetGraphicsJoinPass() const {
        return GraphicsJoinPass;
    }

protected:
    RENDER_API FRDGBarrierBatchBegin&
    GetPrologueBarriersToBegin(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue);
    RENDER_API FRDGBarrierBatchBegin&
    GetEpilogueBarriersToBeginForGraphics(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue);
    RENDER_API FRDGBarrierBatchBegin& GetEpilogueBarriersToBeginForAsyncCompute(
        FRDGAllocator&             Allocator,
        FRDGTransitionCreateQueue& CreateQueue
    );
    RENDER_API FRDGBarrierBatchBegin&
    GetEpilogueBarriersToBeginForAll(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue);

    FRDGBarrierBatchBegin& GetEpilogueBarriersToBeginFor(
        FRDGAllocator&             Allocator,
        FRDGTransitionCreateQueue& CreateQueue,
        ERHIPipeline               PipelineForEnd
    ) {
        switch (PipelineForEnd) {

            case ERHIPipeline::Graphics:
                return GetEpilogueBarriersToBeginForGraphics(Allocator, CreateQueue);

            case ERHIPipeline::AsyncCompute:
                return GetEpilogueBarriersToBeginForAsyncCompute(Allocator, CreateQueue);

            case ERHIPipeline::All:
                return GetEpilogueBarriersToBeginForAll(Allocator, CreateQueue);
        }
    }

    RENDER_API FRDGBarrierBatchEnd& GetPrologueBarriersToEnd(FRDGAllocator& Allocator);
    RENDER_API FRDGBarrierBatchEnd& GetEpilogueBarriersToEnd(FRDGAllocator& Allocator);

    virtual void Execute(FRHIComputeCommandList& RHICmdList) {}

    const FRDGEventName       Name;
    const FRDGParameterStruct ParameterStruct;
    const ERDGPassFlags       Flags;
    const ERDGPassTaskMode    TaskMode;
    const ERHIPipeline        Pipeline;
    FRDGPassHandle            Handle;

    union {
        struct {
            /** Whether the render pass begin / end should be skipped. */
            //uint16 bSkipRenderPassBegin : 1;
            //uint16 bSkipRenderPassEnd : 1;

            /** (AsyncCompute only) Whether this is the first / last async compute pass in an async interval. */
            uint16 bAsyncComputeBegin : 1;
            uint16 bAsyncComputeEnd : 1;

            /** (Graphics only) Whether this is a graphics fork / join pass. */
            uint16 bGraphicsFork : 1;
            uint16 bGraphicsJoin : 1;

            /** Whether the pass only writes to resources in its render pass. */
            uint16 bRenderPassOnlyWrites : 1;

            /** Whether this pass is a sentinel (prologue / epilogue) pass. */
            uint16 bSentinel : 1;

            /** If set, dispatches to the RHI thread after executing this pass. */
            //uint16 bDispatchAfterExecute : 1;

            /** If set, this is a dispatch pass. */
            //uint16 bDispatchPass : 1;
        };
        uint16 PackedBits = 0;
    };

    union {
        // Task-specific bits which are written in a task in parallel with reads from the other set.
        struct {
            /** Whether this pass does not contain parameters. */
            uint8 bEmptyParameters : 1;

            /** Whether this pass has external UAVs that are not tracked by RDG. */
            uint8 bHasExternalOutputs : 1;

            /** Whether this pass has been culled. */
            uint8 bCulled : 1;

            /** Whether this pass is used for external access transitions. */
            uint8 bExternalAccessPass : 1;
        };
        uint8 PacketBits_AsyncSetupQueue = 0;
    };

    union {
        // Task-specific bits which are written in a task in parallel with reads from the other set.
        struct {
            /** If set, marks the begin / end of a span of passes executed in parallel in a task. */
            uint8 bParallelExecuteBegin : 1;
            uint8 bParallelExecuteEnd : 1;

            /** If set, marks that a pass is executing in parallel. */
            uint8 bParallelExecute : 1;
        };
        uint8 PacketBits_ParallelExecute = 0;
    };

    /** Handle of the latest cross-pipeline producer. */
    FRDGPassHandle CrossPipelineProducer;

    /** (AsyncCompute only) Graphics passes which are the fork / join for async compute interval this pass is in. */
    FRDGPassHandle GraphicsForkPass;
    FRDGPassHandle GraphicsJoinPass;

    /** The passes which are handling the epilogue / prologue barriers meant for this pass. */
    FRDGPassHandle PrologueBarrierPass;
    FRDGPassHandle EpilogueBarrierPass;

    /** Number of transitions to reserve. Basically an estimate of the number of textures / buffers. */
    uint32 NumTransitionsToReserve = 0;

    /** Lists of producer passes and the full list of cross-pipeline consumer passes. */
    Array<FRDGPassHandle, FRDGArrayAllocator> CrossPipelineConsumers;
    Array<FRDGPass*, FRDGArrayAllocator>      Producers;

    struct FTextureState {
        FTextureState() = default;

        FTextureState(FRDGTextureRef InTexture) : Texture(InTexture) {
            const uint32 SubresourceCount = Texture->GetSubresourceCount();
            State.SetNum(SubresourceCount);
            MergeState.SetNum(SubresourceCount);
        }

        FRDGTextureRef              Texture = nullptr;
        FRDGTextureSubresourceState State;
        FRDGTextureSubresourceState MergeState;
        uint32                      ReferenceCount = 0;
    };

    struct FBufferState {
        FBufferState() = default;

        FBufferState(FRDGBufferRef InBuffer) : Buffer(InBuffer) {}

        FRDGBufferRef         Buffer = nullptr;
        FRDGSubresourceState  State;
        FRDGSubresourceState* MergeState     = nullptr;
        uint32                ReferenceCount = 0;
    };

    /** Maps textures / buffers to information on how they are used in the pass. */
    Array<FTextureState, FRDGArrayAllocator>           TextureStates;
    Array<FBufferState, FRDGArrayAllocator>            BufferStates;
    Array<FRDGViewHandle, FRDGArrayAllocator>          Views;
    Array<FRDGUniformBufferHandle, FRDGArrayAllocator> UniformBuffers;

    struct FExternalAccessOp {
        FExternalAccessOp() = default;

        FExternalAccessOp(FRDGViewableResource* InResource, FRDGViewableResource::EAccessMode InMode) :
            Resource(InResource),
            Mode(InMode) {}

        FRDGViewableResource*             Resource;
        FRDGViewableResource::EAccessMode Mode;
    };

    Array<FExternalAccessOp, FRDGArrayAllocator> ExternalAccessOps;

    /** Lists of pass parameters scheduled for begin during execution of this pass. */
    Array<FRDGPass*, FRDGArrayAllocator<FRDGPass*>> ResourcesToBegin;
    Array<FRDGPass*, FRDGArrayAllocator<FRDGPass*>> ResourcesToEnd;

    /** Split-barrier batches at various points of execution of the pass. */
    FRDGBarrierBatchBegin*                            PrologueBarriersToBegin                = nullptr;
    FRDGBarrierBatchEnd*                              PrologueBarriersToEnd                  = nullptr;
    FRDGBarrierBatchBegin*                            EpilogueBarriersToBeginForGraphics     = nullptr;
    FRDGBarrierBatchBegin*                            EpilogueBarriersToBeginForAsyncCompute = nullptr;
    FRDGBarrierBatchBegin*                            EpilogueBarriersToBeginForAll          = nullptr;
    Array<FRDGBarrierBatchBegin*, FRDGArrayAllocator> SharedEpilogueBarriersToBegin;
    FRDGBarrierBatchEnd*                              EpilogueBarriersToEnd = nullptr;

    //uint32 ParallelPassSetIndex = 0;

    friend FRDGBuilder;
    friend FRDGPassRegistry;
    friend FRDGTrace;
    friend FRDGUserValidation;
};
} // namespace Moer::Render::RenderGraph
