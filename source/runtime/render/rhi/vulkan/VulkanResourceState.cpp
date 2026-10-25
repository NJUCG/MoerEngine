#include "VulkanResourceState.h"

#include <stdexcept>

namespace Moer::Render {
namespace {

[[noreturn]] void RejectLegacyResourceState(const char* _message) {
    throw std::invalid_argument(_message);
}

void ValidateLegacyPassType(EPassType _pass_type) {
    switch (_pass_type) {
        case EPassType::Graphics:
        case EPassType::Compute:
        case EPassType::Raytracing:
        case EPassType::Copy:
            return;
        default:
            RejectLegacyResourceState("Unknown legacy pass type");
    }
}

VkPipelineStageFlags2 LegacyShaderStagesForPass(EPassType _pass_type) {
    switch (_pass_type) {
        case EPassType::Graphics:
            return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        case EPassType::Compute:
            return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        case EPassType::Raytracing:
            return VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        case EPassType::Copy:
            RejectLegacyResourceState("Copy pass cannot use a shader resource state");
        default:
            RejectLegacyResourceState("Unknown legacy pass type");
    }
}

} // namespace

VulkanBufferAccessState ResolveVulkanBufferState(EBufferState _state, EPassType _pass_type, bool _is_write) {
    ValidateLegacyPassType(_pass_type);
    switch (_state) {
        case EBufferState::UNDEFINED:
            return {VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_NONE};
        case EBufferState::TRANSFER:
            return {
                _is_write ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT
            };
        case EBufferState::VERTEX:
            if (_is_write || _pass_type != EPassType::Graphics) {
                RejectLegacyResourceState("Vertex buffers are read-only graphics resources");
            }
            return {VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT};
        case EBufferState::INDEX:
            if (_is_write || _pass_type != EPassType::Graphics) {
                RejectLegacyResourceState("Index buffers are read-only graphics resources");
            }
            return {VK_ACCESS_2_INDEX_READ_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT};
        case EBufferState::INDIRECT:
            if (_is_write || _pass_type == EPassType::Copy) {
                RejectLegacyResourceState("Indirect buffers are read-only non-copy resources");
            }
            return {VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT};
        case EBufferState::SHADER_RESOURCE:
            if (_is_write || _pass_type == EPassType::Copy) {
                RejectLegacyResourceState("Shader-resource buffers are read-only non-copy resources");
            }
            return {VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT, LegacyShaderStagesForPass(_pass_type)};
        case EBufferState::UNORDERED_ACCESS:
            if (_pass_type == EPassType::Copy) {
                RejectLegacyResourceState("Copy pass cannot use an unordered-access buffer state");
            }
            return {
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                LegacyShaderStagesForPass(_pass_type)
            };
        case EBufferState::Num:
        default:
            RejectLegacyResourceState("Unknown legacy buffer state");
    }
}

VulkanTextureAccessState ResolveVulkanTextureState(
    ETextureState       _state,
    EPassType           _pass_type,
    bool                _is_write,
    ETextureAspectFlags _aspects
) {
    const bool is_depth = uint(_aspects & ETextureAspectFlags::DEPTH_SLICE) != 0u;
    ValidateLegacyPassType(_pass_type);
    switch (_state) {
        case ETextureState::UNDEFINED:
            return {VK_ACCESS_2_NONE, VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE};
        case ETextureState::TRANSFER:
            return {
                _is_write ? VK_ACCESS_2_TRANSFER_WRITE_BIT : VK_ACCESS_2_TRANSFER_READ_BIT,
                _is_write ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT
            };
        case ETextureState::SHADER_RESOURCE:
        case ETextureState::SAMPLE:
            if (_is_write || _pass_type == EPassType::Copy) {
                RejectLegacyResourceState("Shader-resource textures are read-only non-copy resources");
            }
            return {
                _state == ETextureState::SAMPLE ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT :
                                                  VK_ACCESS_2_SHADER_READ_BIT,
                is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL :
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                LegacyShaderStagesForPass(_pass_type)
            };
        case ETextureState::RENDER_TARGET:
            if (_pass_type != EPassType::Graphics) {
                RejectLegacyResourceState("Render targets are graphics attachments");
            }
            return {
                _is_write ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
            };
        case ETextureState::DEPTH_STENCIL:
            if (_pass_type != EPassType::Graphics) {
                RejectLegacyResourceState("Depth-stencil textures are graphics attachments");
            }
            return {
                _is_write ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
                            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                _is_write ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
                            VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT
            };
        case ETextureState::UNORDERED_ACCESS:
            if (_pass_type == EPassType::Copy) {
                RejectLegacyResourceState("Copy pass cannot use an unordered-access texture state");
            }
            return {
                VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
                VK_IMAGE_LAYOUT_GENERAL,
                LegacyShaderStagesForPass(_pass_type)
            };
        case ETextureState::Num:
        default:
            RejectLegacyResourceState("Unknown legacy texture state");
    }
}

} // namespace Moer::Render
