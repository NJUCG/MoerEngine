#include "rendergraph/RenderGraphResource.h"
#include "rendergraph/DepdencyGraph.h"
#include "rendergraph/PassNode.h"
#include "rendergraph/RenderGraph.h"
#include "resources/GlobalRenderResources.h"
#include "resources/ResourceTransition.h"
#include "rhi/RHI.h"
#include "rhi/RHICommon.h"
#include "rhi/RHIResource.h"
#include "rhi/RHIResourceInitilizer.h"
namespace Moer {
    void RenderGraphResource::ConnectForRead(DepdencyGraph& graph, PassNode* pass_node, DepdencyGraph::ResourceDesc _desc) {
        auto edge = MoerNew(DepdencyGraph::Edge)(graph, this, pass_node, _desc);
    }
    void RenderGraphResource::ConnectForWrite(DepdencyGraph& graph, PassNode* pass_node, DepdencyGraph::ResourceDesc _desc) {
        auto edge = MoerNew(DepdencyGraph::Edge)(graph, pass_node, this, _desc);
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

    uint32_t RenderGraphBuffer::ResloveResourceUsage(const DepdencyGraph::ResourceDesc& _desc, RHIBarrierDependencyInfo& barrier_info, EPassType pass_type) {
        const auto& desc = std::get<DepdencyGraph::BufferSubDesc>(_desc);
        if (desc.layout == NO_CHAGNE)
            return desc.layout;
        barrier_info.buffer_barriers.push_back(RHIBufferBarrierInfo{});
        auto& buffer_barrier_info          = barrier_info.buffer_barriers.back();
        auto [src_access_flags, src_stage] = ResourceTransition::GetBufferTransitation(m_buffer->GetLayout(), pass_type);
        auto [dst_access_flags, dst_stage] = ResourceTransition::GetBufferTransitation(static_cast<RenderGraphBuffer::Usage>(desc.layout), pass_type);
        buffer_barrier_info.SetBuffer(m_buffer).SetSrcAccessFlags(src_access_flags).SetDstAccessFlags(dst_access_flags).SetSrcStage(src_stage).SetDstStage(dst_stage);
        m_buffer->SetLayout(static_cast<RenderGraphBuffer::Usage>(desc.layout));
        m_usage = static_cast<RenderGraphBuffer::Usage>(desc.layout);
        return desc.layout;
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
        if (mip_num == -1) {
            mip_num = m_is_sub_resource ? m_sub_res.num_mips : 1;
        }
        if (array_min == -1) {
            array_min = m_is_sub_resource ? m_sub_res.array_index : 0;
        }
        if (array_num == -1) {
            array_num = m_is_sub_resource ? m_sub_res.array_count : 1;
        }
        if (mip_min == -1) {
            mip_min = 0;
        }
        return RenderGraphResourceCache::Get().GetSRV(m_texture, format == PF_UNDEFINED ? GetFormat() : format, mip_min, mip_num, array_min, array_num);
    }
    RHITextureRef RenderGraphTexture::GetTexture() const {
        return m_texture;
    }
    RHIUAVRef RenderGraphTexture::GetUAV(EPixelFormat format, uint32_t _mip_level, uint32_t array_min, uint32_t array_num) {
        if (_mip_level == -1) {
            _mip_level = m_is_sub_resource ? m_sub_res.mip_index : 0;
        }
        if (array_min == -1) {
            array_min = m_is_sub_resource ? m_sub_res.array_index : 0;
        }
        if (array_num == -1) {
            array_num = m_is_sub_resource ? m_sub_res.array_count : 1;
        }
        return RenderGraphResourceCache::Get().GetUAV(m_texture, format == PF_UNDEFINED ? GetFormat() : format, _mip_level, array_min, array_num);
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
        if (m_is_sub_resource) {
            m_texture = m_parent->GetTexture();
            return;
        }
        m_texture = RenderGraphResourceCache::Get().GetTexture(name, m_desc.extent2D, m_desc.format, m_desc.usage, m_desc.mipLevels, m_desc.arrayLayers);
    }
    RenderGraphTexture::RenderGraphTexture(const std::string& name, Descriptor desc) : RenderGraphResource(name, Type::Texture2D, false), m_desc(desc) {
    }
    RenderGraphTexture::RenderGraphTexture(const std::string& name, RHITextureRef texture) : RenderGraphResource(name, Type::Texture2D, true), m_texture(texture) {
    }

    RenderGraphTexture::RenderGraphTexture(const std::string& _name, RenderGraphTexture* _parent, RHISubresourceRange _sub_res) : RenderGraphResource(name, Type::Texture2D, true), m_parent(_parent), m_sub_res(_sub_res), m_is_sub_resource(true) {
    }

    static inline ETextureLayout GetTextureLayout(RenderGraphTexture::Usage usage) {
        if (usage == RenderGraphTexture::Usage::UNDEFINED)
            return TEXTURE_LAYOUT_UNDEFINED;
        if (EnumHasAnyFlag(usage, RenderGraphTexture::Usage::UNORDERED_ACCESS)) {
            return TEXTURE_LAYOUT_COMMON;
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

    uint32_t RenderGraphTexture::ResloveResourceUsage(const DepdencyGraph::ResourceDesc& _desc, RHIBarrierDependencyInfo& _barrier_info, EPassType _pass_type) {
        const auto& desc        = std::get<DepdencyGraph::TextureSubDesc>(_desc);
        Usage       usage_flags = static_cast<Usage>(desc.usage);
        //Todo is this correct?
        bool is_depth_stencil = EnumHasAnyFlag(usage_flags, RenderGraphTexture::Usage::DEPTH_STENCIL_ATTACHMENT) ||
                                GetFormat() == EPixelFormat::PF_D32_SFLOAT_S8_UINT ||
                                GetFormat() == EPixelFormat::PF_D24_UNORM_S8_UINT ||
                                GetFormat() == EPixelFormat::PF_D16_UNORM_S8_UINT;

        auto aspect_flags                  = is_depth_stencil ? ETextureAspectFlags::DEPTH_SLICE : ETextureAspectFlags::COLOR;
        auto dst_layout                    = GetTextureLayout(static_cast<RenderGraphTexture::Usage>(desc.usage));
        auto [dst_access_flags, dst_stage] = ResourceTransition::GetTextureTransition(usage_flags, _pass_type);

        ETextureUsageFlags cur_src_usage    = ETextureUsageFlags::UNDEFINED;
        ETextureLayout     cur_src_layout   = TEXTURE_LAYOUT_UNDEFINED;
        EPassType          cur_pass_type    = _pass_type;
        uint               current_mip_size = 0;

        auto setup_new_barrier = [&](ETextureUsageFlags _src_usage, ETextureLayout _src_layout, EPassType _src_pass_type, uint _mip_idx, uint _mip_size) {
            RHITextureBarrierInfo barrier_info;
            RHISubresourceRange   subresource_range(aspect_flags, _mip_idx, _mip_size, desc.array_index, desc.array_count, 0, 1);
            auto [src_access_flags, src_stage] = ResourceTransition::GetTextureTransition(_src_usage, _src_pass_type);
            barrier_info.SetSubResourceRange(subresource_range)
                .SetTexture(m_texture)
                .SetSrcTextureLayout(_src_layout)
                .SetDstTextureLayout(dst_layout)
                .SetSrcAccessFlags(src_access_flags)
                .SetDstAccessFlags(dst_access_flags)
                .SetSrcStage(src_stage)
                .SetDstStage(dst_stage);
            _barrier_info.texture_barriers.push_back(barrier_info);
            m_texture->SetTrackInfo(subresource_range, desc.usage, _pass_type);
        };
        uint num_mips      = std::min(m_texture->GetNumMips() - desc.mip_level, desc.num_mips);
        bool different_src = false;
        for (uint mip_idx = 0; mip_idx < num_mips; ++mip_idx) {
            uint cur_mip               = desc.mip_level + mip_idx;
            auto [src_usage, src_pass] = m_texture->GetTrackedUsage(cur_mip);
            auto src_layout            = GetTextureLayout(src_usage);
            if (src_layout == dst_layout && src_usage == desc.usage && src_pass == _pass_type)
                continue;
            current_mip_size++;
            bool b_first_mip = cur_mip == desc.mip_level;
            if (b_first_mip || different_src) {
                cur_src_usage  = src_usage;
                cur_src_layout = src_layout;
                cur_pass_type  = src_pass;
            }
            different_src = src_layout != cur_src_layout || src_usage != cur_src_usage || src_pass != cur_pass_type;

            if (different_src) {
                setup_new_barrier(cur_src_usage, cur_src_layout, src_pass, cur_mip - current_mip_size, current_mip_size);
                current_mip_size = 0;
            }
        }
        if (current_mip_size > 0) {
            setup_new_barrier(cur_src_usage, cur_src_layout, cur_pass_type, desc.mip_level + num_mips - current_mip_size, current_mip_size);
        }

        return dst_layout;
    }

}// namespace Moer