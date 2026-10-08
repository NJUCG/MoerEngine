#pragma once

#include "rhi/VertexFormat.h"
#include "rhi/vulkan/VulkanCommon.h"

namespace Moer::Render {

[[nodiscard]] constexpr VkFormat ToVulkanVertexFormat(EVertexFormat format) noexcept {
    switch (format) {
        case EVertexFormat::Float1:
            return VK_FORMAT_R32_SFLOAT;
        case EVertexFormat::Float2:
            return VK_FORMAT_R32G32_SFLOAT;
        case EVertexFormat::Float3:
            return VK_FORMAT_R32G32B32_SFLOAT;
        case EVertexFormat::Float4:
            return VK_FORMAT_R32G32B32A32_SFLOAT;
        case EVertexFormat::UInt:
            return VK_FORMAT_R32_UINT;
        case EVertexFormat::UByte4Normalized:
            return VK_FORMAT_R8G8B8A8_UNORM;
        default:
            return VK_FORMAT_UNDEFINED;
    }
}

} // namespace Moer::Render
