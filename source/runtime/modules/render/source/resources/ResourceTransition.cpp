#include "resources/ResourceTransition.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"

namespace Moer {
    std::tuple<ERHIAccessFlags, ERHIAccessFlags, ERHIPipelineStageFlags, ERHIPipelineStageFlags>
    ResourceTransition::GetImageTransition(ETextureLayout oldLayout, ETextureLayout new_layout) {
        ERHIAccessFlags        src_access_flags, dst_access_flags;
        ERHIPipelineStageFlags src_stage, dst_stage;

        switch (oldLayout) {
            case ETextureLayout::TEXTURE_LAYOUT_UNDEFINED:
                src_access_flags = ERHIAccessFlags::UNDEFINED;
                src_stage        = PS_TRANSFER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT:
                src_access_flags = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::COLOR_ATTACHMENT_READ | ERHIAccessFlags::COLOR_ATTACHMENT_WRITE;
                src_stage        = PS_VERTEX_SHADER | PS_FRAGMENT_SHADER | PS_COLOR_ATTACHMENT_OUTPUT;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_COMMON:
                src_access_flags = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE;
                src_stage        = PS_FRAGMENT_SHADER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                src_access_flags = ERHIAccessFlags::SHADER_READ;
                src_stage        = PS_FRAGMENT_SHADER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_DEPTH_READ:
                src_access_flags = ERHIAccessFlags::SHADER_READ;
                src_stage        = PS_FRAGMENT_SHADER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC:
                src_access_flags = ERHIAccessFlags::TRANSFER_READ;
                src_stage        = PS_TRANSFER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST:
                src_access_flags = ERHIAccessFlags::TRANSFER_WRITE;
                src_stage        = PS_TRANSFER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE:
                src_access_flags = ERHIAccessFlags::DEPTH_STENCIL_READ | ERHIAccessFlags::DEPTH_STENCIL_WRITE;
                src_stage        = PS_LATE_FRAGMENT_TESTS;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_READ:
                src_access_flags = ERHIAccessFlags::MEMORY_READ;
                src_stage        = PS_FRAGMENT_SHADER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC:
                src_access_flags = ERHIAccessFlags::UNDEFINED;
                src_stage        = PS_TRANSFER;
                break;
        }

        switch (new_layout) {
            case ETextureLayout::TEXTURE_LAYOUT_COLOR_ATTACHMENT:
                dst_access_flags = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE | ERHIAccessFlags::COLOR_ATTACHMENT_READ | ERHIAccessFlags::COLOR_ATTACHMENT_WRITE;
                dst_stage        = PS_VERTEX_SHADER | PS_FRAGMENT_SHADER | PS_COLOR_ATTACHMENT_OUTPUT;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_COMMON:
                dst_access_flags = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE;
                dst_stage        = PS_FRAGMENT_SHADER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
                dst_access_flags = ERHIAccessFlags::SHADER_READ;
                dst_stage        = PS_FRAGMENT_SHADER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_DEPTH_READ:
                dst_access_flags = ERHIAccessFlags::SHADER_READ;
                dst_stage        = PS_FRAGMENT_SHADER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_TRANSFER_SRC:
                dst_access_flags = ERHIAccessFlags::TRANSFER_READ;
                dst_stage        = PS_TRANSFER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_TRANSFER_DST:
                dst_access_flags = ERHIAccessFlags::TRANSFER_WRITE;
                dst_stage        = PS_TRANSFER;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE:
                dst_access_flags = ERHIAccessFlags::DEPTH_STENCIL_READ | ERHIAccessFlags::DEPTH_STENCIL_WRITE;
                dst_stage        = PS_EARLY_FRAGMENT_TESTS;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_DEPTH_STENCIL_READ:
                dst_access_flags = ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::DEPTH_STENCIL_WRITE;
                dst_stage        = PS_FRAGMENT_SHADER | PS_EARLY_FRAGMENT_TESTS;
                break;
            case ETextureLayout::TEXTURE_LAYOUT_PRESENT_SRC:
            case ETextureLayout::TEXTURE_LAYOUT_UNDEFINED:
                dst_access_flags = ERHIAccessFlags::UNDEFINED;
                dst_stage        = PS_TOP_OF_PIPE;
                break;
        }

        return std::make_tuple(src_access_flags, dst_access_flags, src_stage, dst_stage);
    }
    ERHIPipelineStageFlags GetPipelineStageFromPassType(EPassType pass_type) {
        switch (pass_type) {
            case EPassType::Compute:
                return PS_COMPUTE_SHADER;
            case EPassType::Graphics:
                return PS_FRAGMENT_SHADER;
            case EPassType::Raytracing:
                return PS_RAY_TRACING_SHADER;
            default:
                return PS_TOP_OF_PIPE;
        }
    }
    std::tuple<ERHIAccessFlags, ERHIPipelineStageFlags>
    ResourceTransition::GetTextureTransition(ETextureStateFlags _src_state, EPassType _src_pass, bool _is_src) {
        ERHIAccessFlags        src_access_flags = ERHIAccessFlags::UNDEFINED;
        ERHIPipelineStageFlags src_stage        = PS_NONE;

        auto transfer_state_any_or = [&](ETextureStateFlags _state, ERHIAccessFlags _access, ERHIPipelineStageFlags _stage) {
            if (EnumHasAnyFlag(_src_state, _state)) {
                src_access_flags |= _access;
                src_stage = _stage;
            }
        };
        auto pip_stage = GetPipelineStageFromPassType(_src_pass);
        transfer_state_any_or(TS_UNORDERED_READ, ERHIAccessFlags::SHADER_READ, pip_stage);
        transfer_state_any_or(TS_UNORDERED_WRITE, ERHIAccessFlags::SHADER_WRITE, pip_stage);
        transfer_state_any_or(TS_COLOR_ATTACHMENT, ERHIAccessFlags::COLOR_ATTACHMENT_WRITE, PS_COLOR_ATTACHMENT_OUTPUT);
        transfer_state_any_or(TS_RESOLVE_ATTACHMENT, ERHIAccessFlags::COLOR_ATTACHMENT_WRITE, PS_COLOR_ATTACHMENT_OUTPUT);
        if (src_stage != PS_NONE) {
            return std::make_tuple(src_access_flags, src_stage);
        }
        switch (_src_state) {
            case TS_UNDEFINED:
                src_access_flags = ERHIAccessFlags::UNDEFINED;
                src_stage        = PS_NONE;
                break;
            case TS_TRANSFER_SRC:
                src_access_flags = ERHIAccessFlags::TRANSFER_READ;
                src_stage        = PS_TRANSFER;
                break;
            case TS_TRANSFER_DST:
                src_access_flags = ERHIAccessFlags::TRANSFER_WRITE;
                src_stage        = PS_TRANSFER;
                break;
            case TS_SAMPLED:
                src_access_flags = ERHIAccessFlags::SHADER_READ;
                src_stage        = GetPipelineStageFromPassType(_src_pass);
                break;
            case TS_DEPTH_STENCIL:
                src_access_flags = ERHIAccessFlags::DEPTH_STENCIL_WRITE;
                if (_is_src)
                    src_stage = PS_LATE_FRAGMENT_TESTS;
                else
                    src_stage = PS_EARLY_FRAGMENT_TESTS;
                break;
            default:
                assert(false && "Invalid texture usage");
        }

        return std::make_tuple(src_access_flags, src_stage);
    }

    std::tuple<ERHIAccessFlags, ERHIPipelineStageFlags>
    ResourceTransition::GetBufferTransitation(EBufferRuntimeUsageFlags layout, EPassType pass_type) {
        if (layout == EBufferRuntimeUsageFlags::UNDEFINED) {
            return {ERHIAccessFlags::UNDEFINED, ERHIPipelineStageFlags::PS_NONE};
        }
        if (layout == EBufferRuntimeUsageFlags::INDIRECT_COMMAND_READ) {
            return {ERHIAccessFlags::INDIRECT_COMMAND_READ, ERHIPipelineStageFlags::PS_DRAW_INDIRECT};
        }
        if (layout == EBufferRuntimeUsageFlags::COMMON) {
            return {ERHIAccessFlags::SHADER_READ | ERHIAccessFlags::SHADER_WRITE, GetPipelineStageFromPassType(pass_type)};
        }
        if (EnumHasAnyFlag(layout, EBufferRuntimeUsageFlags::WRITE)) {
            return {ERHIAccessFlags::SHADER_WRITE, GetPipelineStageFromPassType(pass_type)};
        }
        if (EnumHasAnyFlag(layout, EBufferRuntimeUsageFlags::READ)) {
            return {ERHIAccessFlags::SHADER_READ, GetPipelineStageFromPassType(pass_type)};
        }
        if (EnumHasAnyFlag(layout, EBufferRuntimeUsageFlags::TRANSFER_READ)) {
            return {ERHIAccessFlags::TRANSFER_READ, ERHIPipelineStageFlags::PS_TRANSFER};
        }
        if (EnumHasAnyFlag(layout, EBufferRuntimeUsageFlags::TRANSFER_WRITE)) {
            return {ERHIAccessFlags::TRANSFER_WRITE, ERHIPipelineStageFlags::PS_TRANSFER};
        }
        return {};
    }
}// namespace Moer