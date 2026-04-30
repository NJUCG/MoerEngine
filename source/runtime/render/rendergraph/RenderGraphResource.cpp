#include "rendergraph/RenderGraphResource.h"

#include <algorithm>
#include <limits>

namespace Moer {

bool RGTextureRange::Overlaps(const RGTextureRange& other) const {
    if (mip_count == 0 || array_count == 0 || other.mip_count == 0 || other.array_count == 0) {
        return false;
    }
    if (!EnumHasAnyFlag(aspect, other.aspect)) {
        return false;
    }

    const uint32_t mip_end = mip_min + mip_count;
    const uint32_t other_mip_end = other.mip_min + other.mip_count;
    const uint32_t array_end = array_min + array_count;
    const uint32_t other_array_end = other.array_min + other.array_count;

    return mip_min < other_mip_end && other.mip_min < mip_end && array_min < other_array_end && other.array_min < array_end;
}

bool RGBufferRange::IsWholeResource() const {
    return offset == 0 && size == 0;
}

bool RGBufferRange::Overlaps(const RGBufferRange& other) const {
    if ((!IsWholeResource() && size == 0) || (!other.IsWholeResource() && other.size == 0)) {
        return false;
    }
    if (IsWholeResource() || other.IsWholeResource()) {
        return true;
    }

    const uint64_t end = offset + size;
    const uint64_t other_end = other.offset + other.size;
    return offset < other_end && other.offset < end;
}

bool RGAccessWrites(ERGAccessMode mode) {
    return mode == ERGAccessMode::Write || mode == ERGAccessMode::ReadWrite;
}

bool RGAccessConflicts(ERGAccessMode lhs, ERGAccessMode rhs) {
    return RGAccessWrites(lhs) || RGAccessWrites(rhs);
}

} // namespace Moer
