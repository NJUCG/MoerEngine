#include "rendergraph/RenderGraphResource.h"
#include "rendergraph/PassNode.h"
#include "rendergraph/RenderGraph.h"
#include "resources/GlobalRenderResources.h"
#include "rhi/RHI.h"
#include "rhi/RHIResource.h"
namespace Moer {
    void RenderGraphResource::ConnectForRead(DepdencyGraph& graph, PassNode* pass_node, uint32_t usage) {
        auto edge = MoerNew(DepdencyGraph::Edge)(graph, this, pass_node, usage);
    }
    void RenderGraphResource::ConnectForWrite(DepdencyGraph& graph, PassNode* pass_node, uint32_t usage) {
        auto edge = MoerNew(DepdencyGraph::Edge)(graph, pass_node, this, usage);
    }
    RenderGraphResource::RenderGraphResource(const std::string& name, Type type, bool imported) : Node(name), m_type(type), m_imported(imported) {
    }
    RenderGraphBuffer::RenderGraphBuffer(const std::string& name, Descriptor desc) : RenderGraphResource(name, Type::Buffer, false) {
    }
    RenderGraphBuffer::RenderGraphBuffer(const std::string& name, RHIBufferRef buffer) : RenderGraphResource(name, Type::Buffer, true), m_buffer(buffer) {
    }
    void RenderGraphBuffer::Create() {
        //  m_buffer = RenderGraphResourceCache::Get().GetBuffer(name, m_desc);
    }

    static std::tuple<ERHIAccessFlags, ERHIPipelineStageFlags>
    GetBufferTransitation(EBufferLayout layout, EPassType pass_type) {
        if (layout == EBufferLayout::INDIRECT_COMMAND_READ) {
            return {ERHIAccessFlags::INDIRECT_COMMAND_READ, ERHIPipelineStageFlags::PS_DRAW_INDIRECT};
        }
        if (EnumHasAnyFlag(layout, EBufferLayout::WRITE)) {
            return {ERHIAccessFlags::SHADER_WRITE, ERHIPipelineStageFlags::PS_COMPUTE_SHADER};
        }
        if (EnumHasAnyFlag(layout, EBufferLayout::READ)) {
            return {ERHIAccessFlags::SHADER_READ, ERHIPipelineStageFlags::PS_COMPUTE_SHADER};
        }
        return {};
    }

    static std::tuple<ERHIAccessFlags, ERHIAccessFlags, ERHIPipelineStageFlags, ERHIPipelineStageFlags>
    GetBufferTransitation(EBufferLayout src, EBufferLayout dst, EPassType pass_type) {
        // ERHIAccessFlags src_access_flags, dst_access_flags;
        // ERHIPipelineStageFlags src_stage, dst_stage;
        // if(src == EBufferLayout::INDIRECT_COMMAND_READ) {
        //
        // }
        return {};
    }

    uint32_t RenderGraphBuffer::ResloveResourceUsage(uint32_t usage, RHIBarrierDependencyInfo& barrier_info, EPassType pass_type) {
        if (usage == NO_CHAGNE)
            return usage;
        barrier_info.buffer_barriers.push_back(RHIBufferBarrierInfo{});
        auto& buffer_barrier_info          = barrier_info.buffer_barriers.back();
        auto [src_access_flags, src_stage] = GetBufferTransitation(m_buffer->GetLayout(), pass_type);
        auto [dst_access_flags, dst_stage] = GetBufferTransitation(static_cast<RenderGraphBuffer::Usage>(usage), pass_type);
        buffer_barrier_info.SetBuffer(m_buffer).SetSrcAccessFlags(src_access_flags).SetDstAccessFlags(dst_access_flags).SetSrcStage(src_stage).SetDstStage(dst_stage);
        m_buffer->SetLayout(static_cast<RenderGraphBuffer::Usage>(usage));
        m_usage = static_cast<RenderGraphBuffer::Usage>(usage);
        return usage;
    }
    RHIBufferRef RenderGraphBuffer::GetBuffer() const {
        assert(m_buffer);
        return m_buffer;
    }
    RHISRVRef RenderGraphBuffer::GetSRV() const {
        assert(m_buffer);
        return RenderGraphResourceCache::Get().GetSRV(m_buffer);
    }

    RHIUAVRef RenderGraphBuffer::GetUAV() const {
        assert(m_buffer);
        return RenderGraphResourceCache::Get().GetUAV(m_buffer);
    }

    RHISRVRef RenderGraphTexture::GetSRV(EPixelFormat format, uint32_t mip_min, uint32_t mip_num, uint32_t array_min, uint32_t array_num) {
        return RenderGraphResourceCache::Get().GetSRV(m_texture, format == PF_UNDEFINED ? GetFormat() : format, mip_min, mip_num, array_min, array_num);
    }
    RHITextureRef RenderGraphTexture::GetTexture() const {
        return m_texture;
    }
    RHIUAVRef RenderGraphTexture::GetUAV(EPixelFormat format, uint32_t mip_num, uint32_t array_min, uint32_t array_num) {
        return RenderGraphResourceCache::Get().GetUAV(m_texture, format == PF_UNDEFINED ? GetFormat() : format, mip_num, array_min, array_num);
    }

    EPixelFormat RenderGraphTexture::GetFormat() const {
        return m_imported ? m_texture->GetFormat() : m_desc.format;
    }
    void RenderGraphTexture::Create() {
        if (m_imported)
            return;
        if (m_texture) {
            LOG_ERROR("Texture already created");
            return;
        }
        m_texture = RenderGraphResourceCache::Get().GetTexture(name, m_desc.extent2D, m_desc.format, m_desc.usage, m_desc.mipLevels, m_desc.arrayLayers);
    }
    RenderGraphTexture::RenderGraphTexture(const std::string& name, Descriptor desc) : RenderGraphResource(name, Type::Texture2D, false), m_desc(desc) {
    }
    RenderGraphTexture::RenderGraphTexture(const std::string& name, RHITextureRef texture) : RenderGraphResource(name, Type::Texture2D, true), m_texture(texture) {
    }

    static inline ETextureLayout GetTextureLayout(RenderGraphTexture::Usage usage) {
        if (EnumHasAnyFlag(usage, RenderGraphTexture::Usage::UNORDERED_ACCESS)) {
            return ETextureLayout::TEXTURE_LAYOUT_COMMON;
        }
        if (EnumHasAnyFlag(usage, RenderGraphTexture::Usage::COLOR_ATTACHMENT)) {
            return TEXTURE_LAYOUT_COLOR_ATTACHMENT;
        }
        if (EnumHasAnyFlag(usage, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT)) {
            if (EnumHasAnyFlag(usage, ETextureUsageFlags::SAMPLED)) {
                return TEXTURE_LAYOUT_STENCIL_READ;
            }
            return TEXTURE_LAYOUT_DEPTH_STENCIL_WRITE;
        }
        if (EnumHasAnyFlag(usage, RenderGraphTexture::Usage::SAMPLED)) {
            return TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        if (EnumHasAnyFlag(usage, RenderGraphTexture::Usage::TRANSFER_SRC)) {
            return TEXTURE_LAYOUT_TRANSFER_SRC;
        }
        if (EnumHasAnyFlag(usage, RenderGraphTexture::Usage::TRANSFER_DST)) {
            return TEXTURE_LAYOUT_TRANSFER_DST;
        }

        return TEXTURE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    std::tuple<ERHIAccessFlags, ERHIAccessFlags, ERHIPipelineStageFlags, ERHIPipelineStageFlags>
    GetImageTransition(ETextureLayout oldLayout, ETextureLayout new_layout) {
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

    uint32_t RenderGraphTexture::ResloveResourceUsage(uint32_t usage, RHIBarrierDependencyInfo& barrier_info, EPassType pass_type) {
        Usage usage_flags = static_cast<Usage>(usage);
        //Todo is this correct?
        bool                is_depth_stencil = EnumHasAnyFlag(usage_flags, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT) || GetFormat() == EPixelFormat::PF_D32_SFLOAT_S8_UINT;
        RHISubresourceRange subresource_range(is_depth_stencil ? ETextureAspectFlags::DEPTH_SLICE : ETextureAspectFlags::COLOR, 0, 1, 0, 1, 0, 1);
        auto                src_layout = m_texture->GetLayout(subresource_range);
        auto                dst_layout = GetTextureLayout(static_cast<RenderGraphTexture::Usage>(usage));
        if (src_layout == dst_layout)
            return src_layout;
        barrier_info.texture_barriers.push_back(RHITextureBarrierInfo{});
        auto& texture_barrier_info                                      = barrier_info.texture_barriers.back();
        auto [src_access_flags, dst_access_flags, src_stage, dst_stage] = GetImageTransition(src_layout, dst_layout);
        texture_barrier_info
            .SetSubResourceRange(subresource_range)
            .SetTexture(m_texture)
            .SetSrcTextureLayout(src_layout)
            .SetDstTextureLayout(dst_layout)
            .SetSrcAccessFlags(src_access_flags)
            .SetDstAccessFlags(dst_access_flags)
            .SetSrcStage(src_stage)
            .SetDstStage(dst_stage);
        m_texture->SetLayout(subresource_range, dst_layout);
        return dst_layout;
    }

}// namespace Moer