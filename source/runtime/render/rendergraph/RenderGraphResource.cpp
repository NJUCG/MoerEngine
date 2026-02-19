#include "RenderGraphResource.h"

namespace Moer::Render::RenderGraph {

namespace {
constexpr ERHIAccess GRHIMergeableAccessMask =
    static_cast<ERHIAccess>(ERHIAccess::ReadableMask | ERHIAccess::WritableMask);
// Be conservative: do not merge across different pipelines.
constexpr ERHIAccess GRHIMultiPipelineMergeableAccessMask = ERHIAccess::Unknown;

inline bool SkipUAVBarrier(const FRDGSubresourceState&, const FRDGSubresourceState&) {
    // Minimal compatibility: always require UAV barriers.
    return false;
}
} // namespace

bool FRDGSubresourceState::IsTransitionRequired(
    const FRDGSubresourceState& Previous,
    const FRDGSubresourceState& Next
) {
    // This function only needs to filter out identical states and handle UAV barriers.
    assert(Next.Access != ERHIAccess::Unknown);

    if (Previous.Access != Next.Access || Previous.GetPipelines() != Next.GetPipelines()) {
        return true;
    }

    // UAV is a special case as a barrier may still be required even if the states match.
    if (EnumHasAnyFlag(Next.Access, ERHIAccess::UAVMask) && !SkipUAVBarrier(Previous, Next)) {
        return true;
    }

    return false;
}

bool FRDGSubresourceState::IsMergeAllowed(
    ERDGViewableResourceType    ResourceType,
    const FRDGSubresourceState& Previous,
    const FRDGSubresourceState& Next
) {
    /** State merging occurs during compilation and before resource transitions are collected. It serves to remove the bulk
	 *  of unnecessary transitions by looking ahead in the resource usage chain. A resource transition cannot occur within
	 *  a merged state, so a merge is not allowed to proceed if a barrier might be required. Merging is also where multi-pipe
	 *  transitions are determined, if supported by the platform.
	 */

    const ERHIAccess AccessUnion = Previous.Access | Next.Access;
    const ERHIAccess DSVMask     = ERHIAccess::DSVRead | ERHIAccess::DSVWrite;

    // If we have the same access between the two states, we don't need to check for invalid access combinations.
    if (Previous.Access != Next.Access) {
        // Not allowed to merge read-only and writable states.
        if (EnumHasAnyFlag(Previous.Access, ERHIAccess::ReadOnlyExclusiveMask) &&
            EnumHasAnyFlag(Next.Access, ERHIAccess::WritableMask)) {
            return false;
        }

        // Not allowed to merge write-only and readable states.
        if (EnumHasAnyFlag(Previous.Access, ERHIAccess::WriteOnlyExclusiveMask) &&
            EnumHasAnyFlag(Next.Access, ERHIAccess::ReadableMask)) {
            return false;
        }

        // UAVs will filter through the above checks because they are both read and write. UAV can only merge it itself.
        if (EnumHasAnyFlag(AccessUnion, ERHIAccess::UAVMask) &&
            EnumHasAnyFlag(AccessUnion, ~ERHIAccess::UAVMask)) {
            return false;
        }

        // Depth Read / Write should never merge with anything other than itself.
        if (EnumHasAllFlag(AccessUnion, DSVMask) && EnumHasAnyFlag(AccessUnion, ~DSVMask)) {
            return false;
        }

        // Filter out platform-specific unsupported mergeable states.
        if (EnumHasAnyFlag(AccessUnion, ~GRHIMergeableAccessMask)) {
            return false;
        }
    }

    // Not allowed if the resource is being used as a UAV and needs a barrier.
    if (EnumHasAnyFlag(Next.Access, ERHIAccess::UAVMask) && !SkipUAVBarrier(Previous, Next)) {
        return false;
    }

    // Filter out unsupported platform-specific multi-pipeline merged accesses.
    if (EnumHasAnyFlag(AccessUnion, ~GRHIMultiPipelineMergeableAccessMask) &&
        Previous.GetPipelines() != Next.GetPipelines()) {
        return false;
    }

    return true;
}

FRDGViewableResource::FRDGViewableResource(
    const char*                    InName,
    const ERDGViewableResourceType InType
) :
    FRDGResource(InName),
    Type(InType),
    bExternal(0),
    bExtracted(0),
    bProduced(0),
    ReferenceCount(0) {}

FRDGTexture::FRDGTexture(const char* InName, const FRDGTextureDesc& InDesc, ERDGTextureFlags InFlags) :
    FRDGViewableResource(InName, ERDGViewableResourceType::Texture),
    Desc(InDesc),
    Flags(InFlags),
    Layout(InDesc),
    WholeRange(Layout),
    SubresourceCount(Layout.GetSubresourceCount()) {
    if (EnumHasAnyFlag(InFlags, ERDGTextureFlags::SkipTracking)) {
        // Skip tracking implies external read-only usage managed outside RDG.
        AccessModeState.Mode      = EAccessMode::External;
        AccessModeState.Access    = ERHIAccess::ReadableMask;
        AccessModeState.Pipelines = ERHIPipeline::All;
        EpilogueAccess            = AccessModeState.Access;

        AccessModeState.bLocked    = true;
        AccessModeState.ActiveMode = AccessModeState.Mode;
    }

    State.SetNum(SubresourceCount);
    FirstState.SetNum(SubresourceCount);
    MergeState.SetNum(SubresourceCount);
    LastProducers.SetNum(SubresourceCount);
}

FRDGBuffer::FRDGBuffer(const char* InName, const FRDGBufferDesc& InDesc, ERDGBufferFlags InFlags) :
    FRDGViewableResource(InName, ERDGViewableResourceType::Buffer),
    Desc(InDesc),
    Flags(InFlags) {
    if (EnumHasAnyFlag(InFlags, ERDGBufferFlags::SkipTracking)) {
        AccessModeState.Mode      = EAccessMode::External;
        AccessModeState.Access    = ERHIAccess::ReadableMask;
        AccessModeState.Pipelines = ERHIPipeline::All;
        EpilogueAccess            = AccessModeState.Access;

        AccessModeState.bLocked    = true;
        AccessModeState.ActiveMode = AccessModeState.Mode;
    }
}

FRDGBuffer::FRDGBuffer(
    const char*                    InName,
    const FRDGBufferDesc&          InDesc,
    ERDGBufferFlags                InFlags,
    FRDGBufferNumElementsCallback* InNumElementsCallback
) :
    FRDGBuffer(InName, InDesc, InFlags) {
    NumElementsCallback = InNumElementsCallback;
}

void FRDGBuffer::FinalizeDesc() {
    if (NumElementsCallback) {
        uint32 NumElements   = (*NumElementsCallback)();
        Desc.NumElements     = NumElements > 0 ? NumElements : 1u;
        *NumElementsCallback = {};
        NumElementsCallback  = nullptr;
    }
}

} // namespace Moer::Render::RenderGraph
