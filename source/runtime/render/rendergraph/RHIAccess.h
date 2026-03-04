#pragma once
#include <cassert>
#include <cstdint>
#include <limits>

#include "rhi/RHI.h"

namespace Moer::Render {
class FRHIViewableResource;
} // namespace Moer::Render

namespace Moer::Render::RenderGraph {

enum class ERHIAccess : uint32 {
    Unknown = 0,

    // Read
    CPURead             = 1 << 0,
    Present             = 1 << 1,
    IndirectArgs        = 1 << 2,
    VertexOrIndexBuffer = 1 << 3,
    SRVCompute          = 1 << 4,
    SRVGraphics         = 1 << 5,
    CopySrc             = 1 << 6,
    DSVRead             = 1 << 7,

    // Write
    UAVCompute  = 1 << 8,
    UAVGraphics = 1 << 9,
    RTV         = 1 << 10,
    CopyDest    = 1 << 11,
    DSVWrite    = 1 << 12,

    // --- Helper masks (core RDG logic) ---
    SRVMask = SRVCompute | SRVGraphics,
    UAVMask = UAVCompute | UAVGraphics,

    ReadOnlyMask = CPURead | Present | IndirectArgs | VertexOrIndexBuffer | SRVMask | CopySrc | DSVRead,
    ReadableMask = ReadOnlyMask,
    ReadOnlyExclusiveMask = ReadOnlyMask,

    WritableMask = UAVMask | RTV | CopyDest | DSVWrite,

    WriteOnlyMask          = RTV | CopyDest | DSVWrite,
    WriteOnlyExclusiveMask = WriteOnlyMask
};

ENUM_BIT_OP_IMPL(ERHIAccess, FLAG)

inline constexpr bool IsWritableAccess(ERHIAccess Access) {
    return (uint32(Access) & uint32(ERHIAccess::WritableMask)) != 0;
}

inline constexpr bool IsReadableAccess(ERHIAccess Access) {
    return (uint32(Access) & uint32(ERHIAccess::ReadableMask)) != 0;
}

inline constexpr bool IsInvalidAccess(ERHIAccess Access) {
    bool bReadWriteConflict = ((uint32(Access) & uint32(ERHIAccess::ReadOnlyExclusiveMask)) != 0) &&
                              ((uint32(Access) & uint32(ERHIAccess::WritableMask)) != 0);

    return bReadWriteConflict;
}

inline void ValidateAccess(ERHIAccess Access) {
    assert(!IsInvalidAccess(Access) && "Detected invalid RDG resource access state!");
}

struct FRHITrackedAccess {
    FRHITrackedAccess() = default;

    explicit FRHITrackedAccess(ERHIAccess InAccess) : Access(InAccess) {}

    FRHITrackedAccess(ERHIAccess InAccess, Moer::Render::ERHIPipeline InPipelines) :
        Access(InAccess),
        Pipelines(InPipelines) {}

    ERHIAccess                 Access    = ERHIAccess::Unknown;
    Moer::Render::ERHIPipeline Pipelines = Moer::Render::ERHIPipeline::None;
};

struct FRHITrackedAccessInfo : FRHITrackedAccess {
    FRHITrackedAccessInfo() = default;

    FRHITrackedAccessInfo(
        Moer::Render::FRHIViewableResource* InResource,
        ERHIAccess                          InAccess,
        Moer::Render::ERHIPipeline          InPipelines
    ) :
        FRHITrackedAccess(InAccess, InPipelines),
        Resource(InResource) {}

    Moer::Render::FRHIViewableResource* Resource = nullptr;
};

} // namespace Moer::Render::RenderGraph
